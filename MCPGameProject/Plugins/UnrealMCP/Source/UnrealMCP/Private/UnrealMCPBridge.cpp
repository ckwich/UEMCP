#include "UnrealMCPBridge.h"
#include "MCPServerRunnable.h"
#include "Sockets.h"
#include "SocketSubsystem.h"
#include "HAL/RunnableThread.h"
#include "Interfaces/IPv4/IPv4Address.h"
#include "Interfaces/IPv4/IPv4Endpoint.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonWriter.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/DirectionalLight.h"
#include "Engine/PointLight.h"
#include "Engine/SpotLight.h"
#include "Camera/CameraActor.h"
#include "EditorAssetLibrary.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "JsonObjectConverter.h"
#include "CoreGlobals.h"
#include "Editor.h"
#include "GameFramework/Actor.h"
#include "Kismet/GameplayStatics.h"
#include "Async/Async.h"
#include "Templates/Atomic.h"
// Add Blueprint related includes
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Factories/BlueprintFactory.h"
#include "EdGraphSchema_K2.h"
#include "K2Node_Event.h"
#include "K2Node_VariableGet.h"
#include "K2Node_VariableSet.h"
#include "Components/StaticMeshComponent.h"
#include "Components/BoxComponent.h"
#include "Components/SphereComponent.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
// UE5.5 correct includes
#include "Engine/SimpleConstructionScript.h"
#include "Engine/SCS_Node.h"
#include "UObject/Field.h"
#include "UObject/FieldPath.h"
// Blueprint Graph specific includes
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "K2Node_CallFunction.h"
#include "K2Node_InputAction.h"
#include "K2Node_Self.h"
#include "GameFramework/InputSettings.h"
#include "EditorSubsystem.h"
#include "Subsystems/EditorActorSubsystem.h"
// Include our new command handler classes
#include "Commands/UnrealMCPEditorCommands.h"
#include "Commands/UnrealMCPBlueprintCommands.h"
#include "Commands/UnrealMCPBlueprintNodeCommands.h"
#include "Commands/UnrealMCPProjectCommands.h"
#include "Commands/UnrealMCPCommonUtils.h"
#include "Commands/UnrealMCPUMGCommands.h"
#include "Commands/UnrealMCPAssetWorkflowCommands.h"

// Default settings
#define MCP_SERVER_HOST "127.0.0.1"
#define MCP_SERVER_PORT 55557

namespace
{
    FString SerializeBridgeErrorResponse(const FString& ErrorMessage)
    {
        TSharedPtr<FJsonObject> ResponseJson = MakeShared<FJsonObject>();
        ResponseJson->SetStringField(TEXT("status"), TEXT("error"));
        ResponseJson->SetStringField(TEXT("error"), ErrorMessage);

        FString Response;
        TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Response);
        FJsonSerializer::Serialize(ResponseJson.ToSharedRef(), Writer);
        return Response;
    }

    double GetGameThreadCommandTimeoutSeconds(
        const FString& CommandType,
        const TSharedPtr<FJsonObject>& Params
    )
    {
        if (CommandType == TEXT("get_editor_status"))
        {
            return 3.0;
        }

        if (CommandType == TEXT("run_automation_test"))
        {
            double RequestedTimeoutSeconds = 30.0;
            if (Params.IsValid())
            {
                Params->TryGetNumberField(TEXT("timeout_seconds"), RequestedTimeoutSeconds);
            }

            return FMath::Clamp(RequestedTimeoutSeconds + 15.0, 15.0, 135.0);
        }

        return 25.0;
    }
}

FUnrealMCPBridge::FUnrealMCPBridge()
    : bIsRunning(false)
    , ListenerSocket(nullptr)
    , ConnectionSocket(nullptr)
    , ServerRunnable(nullptr)
    , ServerThread(nullptr)
    , Port(MCP_SERVER_PORT)
{
    EditorCommands = MakeShared<FUnrealMCPEditorCommands>();
    BlueprintCommands = MakeShared<FUnrealMCPBlueprintCommands>();
    BlueprintNodeCommands = MakeShared<FUnrealMCPBlueprintNodeCommands>();
    ProjectCommands = MakeShared<FUnrealMCPProjectCommands>();
    UMGCommands = MakeShared<FUnrealMCPUMGCommands>();
    AssetWorkflowCommands = MakeShared<FUnrealMCPAssetWorkflowCommands>();
    FIPv4Address::Parse(MCP_SERVER_HOST, ServerAddress);
}

FUnrealMCPBridge::~FUnrealMCPBridge()
{
    StopServer();

    EditorCommands.Reset();
    BlueprintCommands.Reset();
    BlueprintNodeCommands.Reset();
    ProjectCommands.Reset();
    UMGCommands.Reset();
    AssetWorkflowCommands.Reset();
}

// Start the MCP server
void FUnrealMCPBridge::StartServer()
{
    if (bIsRunning)
    {
        UE_LOG(LogTemp, Warning, TEXT("UnrealMCPBridge: Server is already running"));
        return;
    }

    // Create socket subsystem
    ISocketSubsystem* SocketSubsystem = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
    if (!SocketSubsystem)
    {
        UE_LOG(LogTemp, Error, TEXT("UnrealMCPBridge: Failed to get socket subsystem"));
        return;
    }

    // Create listener socket
    TSharedPtr<FSocket> NewListenerSocket = MakeShareable(SocketSubsystem->CreateSocket(NAME_Stream, TEXT("UnrealMCPListener"), false));
    if (!NewListenerSocket.IsValid())
    {
        UE_LOG(LogTemp, Error, TEXT("UnrealMCPBridge: Failed to create listener socket"));
        return;
    }

    // Allow address reuse for quick restarts
    NewListenerSocket->SetReuseAddr(true);
    NewListenerSocket->SetNonBlocking(true);

    // Bind to address
    FIPv4Endpoint Endpoint(ServerAddress, Port);
    if (!NewListenerSocket->Bind(*Endpoint.ToInternetAddr()))
    {
        UE_LOG(LogTemp, Error, TEXT("UnrealMCPBridge: Failed to bind listener socket to %s:%d"), *ServerAddress.ToString(), Port);
        return;
    }

    // Start listening
    if (!NewListenerSocket->Listen(5))
    {
        UE_LOG(LogTemp, Error, TEXT("UnrealMCPBridge: Failed to start listening"));
        return;
    }

    ListenerSocket = NewListenerSocket;
    bIsRunning = true;
    UE_LOG(LogTemp, Display, TEXT("UnrealMCPBridge: Server started on %s:%d"), *ServerAddress.ToString(), Port);

    // Start server thread
    ServerRunnable = new FMCPServerRunnable(this, ListenerSocket);
    ServerThread = FRunnableThread::Create(
        ServerRunnable,
        TEXT("UnrealMCPServerThread"),
        0, TPri_Normal
    );

    if (!ServerThread)
    {
        UE_LOG(LogTemp, Error, TEXT("UnrealMCPBridge: Failed to create server thread"));
        delete ServerRunnable;
        ServerRunnable = nullptr;
        StopServer();
        return;
    }
}

// Stop the MCP server
void FUnrealMCPBridge::StopServer()
{
    if (!bIsRunning)
    {
        return;
    }

    bIsRunning = false;

    // Clean up thread
    if (ServerRunnable)
    {
        ServerRunnable->Stop();
    }

    if (ServerThread)
    {
        ServerThread->WaitForCompletion();
        delete ServerThread;
        ServerThread = nullptr;
    }

    delete ServerRunnable;
    ServerRunnable = nullptr;

    // Close sockets
    if (ConnectionSocket.IsValid())
    {
        ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->DestroySocket(ConnectionSocket.Get());
        ConnectionSocket.Reset();
    }

    if (ListenerSocket.IsValid())
    {
        ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->DestroySocket(ListenerSocket.Get());
        ListenerSocket.Reset();
    }

    UE_LOG(LogTemp, Display, TEXT("UnrealMCPBridge: Server stopped"));
}

// Execute a command received from a client
FString FUnrealMCPBridge::ExecuteCommand(const FString& CommandType, const TSharedPtr<FJsonObject>& Params)
{
    UE_LOG(LogTemp, Display, TEXT("UnrealMCPBridge: Executing command: %s"), *CommandType);

    if (CommandType == TEXT("ping"))
    {
        TSharedPtr<FJsonObject> ResponseJson = MakeShared<FJsonObject>();
        TSharedPtr<FJsonObject> ResultJson = MakeShared<FJsonObject>();
        ResultJson->SetStringField(TEXT("message"), TEXT("pong"));
        ResponseJson->SetStringField(TEXT("status"), TEXT("success"));
        ResponseJson->SetObjectField(TEXT("result"), ResultJson);

        FString ResultString;
        TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&ResultString);
        FJsonSerializer::Serialize(ResponseJson.ToSharedRef(), Writer);
        return ResultString;
    }

    if (!GIsRunning || !GEditor)
    {
        return SerializeBridgeErrorResponse(
            TEXT("Unreal Editor is still starting up; editor-backed UEMCP commands are not available yet")
        );
    }

    TSharedPtr<FUnrealMCPEditorCommands> EditorCommandsForTask = EditorCommands;
    TSharedPtr<FUnrealMCPBlueprintCommands> BlueprintCommandsForTask = BlueprintCommands;
    TSharedPtr<FUnrealMCPBlueprintNodeCommands> BlueprintNodeCommandsForTask = BlueprintNodeCommands;
    TSharedPtr<FUnrealMCPProjectCommands> ProjectCommandsForTask = ProjectCommands;
    TSharedPtr<FUnrealMCPUMGCommands> UMGCommandsForTask = UMGCommands;
    TSharedPtr<FUnrealMCPAssetWorkflowCommands> AssetWorkflowCommandsForTask = AssetWorkflowCommands;
    TSharedRef<TAtomic<bool>, ESPMode::ThreadSafe> bCommandCancelled = MakeShared<TAtomic<bool>, ESPMode::ThreadSafe>(false);

    auto ExecuteOnGameThread = [
        CommandType,
        Params,
        bCommandCancelled,
        EditorCommandsForTask,
        BlueprintCommandsForTask,
        BlueprintNodeCommandsForTask,
        ProjectCommandsForTask,
        UMGCommandsForTask,
        AssetWorkflowCommandsForTask
    ]() -> FString
    {
        if (bCommandCancelled->Load())
        {
            return SerializeBridgeErrorResponse(FString::Printf(
                TEXT("Command was cancelled before reaching the Unreal editor game thread: %s"),
                *CommandType
            ));
        }

        TSharedPtr<FJsonObject> ResponseJson = MakeShared<FJsonObject>();
        TSharedPtr<FJsonObject> SafeParams = Params.IsValid() ? Params : MakeShared<FJsonObject>();

        try
        {
            TSharedPtr<FJsonObject> ResultJson;

            if (CommandType == TEXT("asset_intake_snapshot"))
            {
                ResultJson = AssetWorkflowCommandsForTask->HandleCommand(CommandType, SafeParams);
            }
            else if (CommandType == TEXT("get_editor_status") ||
                     CommandType == TEXT("get_output_log") ||
                     CommandType == TEXT("get_level_snapshot") ||
                     CommandType == TEXT("get_pie_runtime_snapshot") ||
                     CommandType == TEXT("asset_search") ||
                     CommandType == TEXT("asset_dependencies") ||
                     CommandType == TEXT("asset_referencers") ||
                     CommandType == TEXT("blueprint_query") ||
                     CommandType == TEXT("list_automation_tests") ||
                     CommandType == TEXT("run_automation_test") ||
                     CommandType == TEXT("get_actors_in_level") ||
                     CommandType == TEXT("find_actors_by_name") ||
                     CommandType == TEXT("spawn_actor") ||
                     CommandType == TEXT("create_actor") ||
                     CommandType == TEXT("delete_actor") || 
                     CommandType == TEXT("set_actor_transform") ||
                     CommandType == TEXT("get_actor_properties") ||
                     CommandType == TEXT("set_actor_property") ||
                     CommandType == TEXT("spawn_blueprint_actor") ||
                     CommandType == TEXT("save_current_level") ||
                     CommandType == TEXT("focus_viewport") || 
                     CommandType == TEXT("take_screenshot"))
            {
                ResultJson = EditorCommandsForTask->HandleCommand(CommandType, SafeParams);
            }
            // Blueprint Commands
            else if (CommandType == TEXT("create_blueprint") || 
                     CommandType == TEXT("add_component_to_blueprint") || 
                     CommandType == TEXT("set_component_property") || 
                     CommandType == TEXT("set_physics_properties") || 
                     CommandType == TEXT("compile_blueprint") || 
                     CommandType == TEXT("set_blueprint_property") || 
                     CommandType == TEXT("set_static_mesh_properties") ||
                     CommandType == TEXT("set_pawn_properties"))
            {
                ResultJson = BlueprintCommandsForTask->HandleCommand(CommandType, SafeParams);
            }
            // Blueprint Node Commands
            else if (CommandType == TEXT("connect_blueprint_nodes") || 
                     CommandType == TEXT("add_blueprint_get_self_component_reference") ||
                     CommandType == TEXT("add_blueprint_self_reference") ||
                     CommandType == TEXT("find_blueprint_nodes") ||
                     CommandType == TEXT("add_blueprint_event_node") ||
                     CommandType == TEXT("add_blueprint_input_action_node") ||
                     CommandType == TEXT("add_blueprint_function_node") ||
                     CommandType == TEXT("add_blueprint_get_component_node") ||
                     CommandType == TEXT("add_blueprint_variable"))
            {
                ResultJson = BlueprintNodeCommandsForTask->HandleCommand(CommandType, SafeParams);
            }
            // Project Commands
            else if (CommandType == TEXT("create_input_mapping"))
            {
                ResultJson = ProjectCommandsForTask->HandleCommand(CommandType, SafeParams);
            }
            // UMG Commands
            else if (CommandType == TEXT("create_umg_widget_blueprint") ||
                     CommandType == TEXT("add_text_block_to_widget") ||
                     CommandType == TEXT("add_button_to_widget") ||
                     CommandType == TEXT("bind_widget_event") ||
                     CommandType == TEXT("set_text_block_binding") ||
                     CommandType == TEXT("add_widget_to_viewport"))
            {
                ResultJson = UMGCommandsForTask->HandleCommand(CommandType, SafeParams);
            }
            else
            {
                ResponseJson->SetStringField(TEXT("status"), TEXT("error"));
                ResponseJson->SetStringField(TEXT("error"), FString::Printf(TEXT("Unknown command: %s"), *CommandType));

                FString ResultString;
                TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&ResultString);
                FJsonSerializer::Serialize(ResponseJson.ToSharedRef(), Writer);
                return ResultString;
            }

            if (!ResultJson.IsValid())
            {
                ResponseJson->SetStringField(TEXT("status"), TEXT("error"));
                ResponseJson->SetStringField(TEXT("error"), FString::Printf(TEXT("Command returned no result: %s"), *CommandType));

                FString ResultString;
                TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&ResultString);
                FJsonSerializer::Serialize(ResponseJson.ToSharedRef(), Writer);
                return ResultString;
            }

            // Check if the result contains an error
            bool bSuccess = true;
            FString ErrorMessage;

            if (ResultJson->HasField(TEXT("success")))
            {
                bSuccess = ResultJson->GetBoolField(TEXT("success"));
                if (!bSuccess && ResultJson->HasField(TEXT("error")))
                {
                    ErrorMessage = ResultJson->GetStringField(TEXT("error"));
                }
            }
            
            if (bSuccess)
            {
                // Set success status and include the result
                ResponseJson->SetStringField(TEXT("status"), TEXT("success"));
                ResponseJson->SetObjectField(TEXT("result"), ResultJson);
            }
            else
            {
                // Set error status and include the error message
                ResponseJson->SetStringField(TEXT("status"), TEXT("error"));
                ResponseJson->SetStringField(TEXT("error"), ErrorMessage);
            }
        }
        catch (const std::exception& e)
        {
            ResponseJson->SetStringField(TEXT("status"), TEXT("error"));
            ResponseJson->SetStringField(TEXT("error"), UTF8_TO_TCHAR(e.what()));
        }

        FString ResultString;
        TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&ResultString);
        FJsonSerializer::Serialize(ResponseJson.ToSharedRef(), Writer);
        return ResultString;
    };

    if (IsInGameThread())
    {
        return ExecuteOnGameThread();
    }

    TPromise<FString> Promise;
    TFuture<FString> Future = Promise.GetFuture();

    AsyncTask(ENamedThreads::GameThread, [ExecuteOnGameThread = MoveTemp(ExecuteOnGameThread), Promise = MoveTemp(Promise)]() mutable
    {
        Promise.SetValue(ExecuteOnGameThread());
    });

    const double TimeoutSeconds = GetGameThreadCommandTimeoutSeconds(CommandType, Params);
    if (!Future.WaitFor(FTimespan::FromSeconds(TimeoutSeconds)))
    {
        bCommandCancelled->Store(true);
        return SerializeBridgeErrorResponse(FString::Printf(
            TEXT("Timed out waiting %.1f seconds for the Unreal editor game thread while executing command: %s"),
            TimeoutSeconds,
            *CommandType
        ));
    }

    return Future.Get();
}
