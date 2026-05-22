#include "MCPServerRunnable.h"

#include "Containers/StringConv.h"
#include "Dom/JsonObject.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "SocketSubsystem.h"
#include "UnrealMCPBridge.h"

namespace
{
    constexpr int32 ReceiveBufferSize = 8192;
    constexpr int32 MaxInboundMessageBytes = 1024 * 1024;
    constexpr double ReceiveTimeoutSeconds = 30.0;

    FString BytesToUtf8String(const TArray<uint8>& MessageBytes)
    {
        if (MessageBytes.IsEmpty())
        {
            return FString();
        }

        FUTF8ToTCHAR Converted(
            reinterpret_cast<const ANSICHAR*>(MessageBytes.GetData()),
            MessageBytes.Num()
        );
        return FString(Converted.Length(), Converted.Get());
    }

    bool TryParseJsonMessage(
        const TArray<uint8>& MessageBytes,
        TSharedPtr<FJsonObject>& OutJsonObject,
        FString& OutMessageText
    )
    {
        OutMessageText = BytesToUtf8String(MessageBytes);
        TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(OutMessageText);
        return FJsonSerializer::Deserialize(Reader, OutJsonObject) && OutJsonObject.IsValid();
    }

    FString SerializeErrorResponse(const FString& ErrorMessage)
    {
        TSharedPtr<FJsonObject> ResponseJson = MakeShared<FJsonObject>();
        ResponseJson->SetStringField(TEXT("status"), TEXT("error"));
        ResponseJson->SetStringField(TEXT("error"), ErrorMessage);

        FString Response;
        TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Response);
        FJsonSerializer::Serialize(ResponseJson.ToSharedRef(), Writer);
        return Response;
    }
}

FMCPServerRunnable::FMCPServerRunnable(FUnrealMCPBridge* InBridge, TSharedPtr<FSocket> InListenerSocket)
    : Bridge(InBridge)
    , ListenerSocket(InListenerSocket)
    , bRunning(true)
{
    UE_LOG(LogTemp, Display, TEXT("MCPServerRunnable: Created server runnable"));
}

FMCPServerRunnable::~FMCPServerRunnable()
{
}

bool FMCPServerRunnable::Init()
{
    return true;
}

uint32 FMCPServerRunnable::Run()
{
    UE_LOG(LogTemp, Display, TEXT("MCPServerRunnable: Server thread starting"));

    while (bRunning)
    {
        bool bPending = false;
        if (!ListenerSocket.IsValid() || !ListenerSocket->HasPendingConnection(bPending) || !bPending)
        {
            FPlatformProcess::Sleep(0.1f);
            continue;
        }

        UE_LOG(LogTemp, Display, TEXT("MCPServerRunnable: Client connection pending, accepting"));
        ClientSocket = MakeShareable(ListenerSocket->Accept(TEXT("MCPClient")));
        if (!ClientSocket.IsValid())
        {
            UE_LOG(LogTemp, Warning, TEXT("MCPServerRunnable: Failed to accept client connection"));
            continue;
        }

        ClientSocket->SetNoDelay(true);
        ClientSocket->SetNonBlocking(true);

        int32 SocketBufferSize = 65536;
        ClientSocket->SetSendBufferSize(SocketBufferSize, SocketBufferSize);
        ClientSocket->SetReceiveBufferSize(SocketBufferSize, SocketBufferSize);

        TSharedPtr<FJsonObject> JsonObject;
        FString ReceivedText;
        FString ReadError;
        const EReadJsonMessageResult ReadResult = ReadCompleteJsonMessage(JsonObject, ReceivedText, ReadError);

        if (ReadResult == EReadJsonMessageResult::Complete)
        {
            ProcessJsonCommand(JsonObject, ReceivedText);
        }
        else if (ReadResult == EReadJsonMessageResult::Error)
        {
            SendUtf8Response(SerializeErrorResponse(ReadError));
        }
        else
        {
            UE_LOG(LogTemp, Display, TEXT("MCPServerRunnable: Client disconnected before a complete command"));
        }

        ClientSocket->Close();
        ClientSocket.Reset();
    }

    UE_LOG(LogTemp, Display, TEXT("MCPServerRunnable: Server thread stopping"));
    return 0;
}

void FMCPServerRunnable::Stop()
{
    bRunning = false;
}

void FMCPServerRunnable::Exit()
{
}

FMCPServerRunnable::EReadJsonMessageResult FMCPServerRunnable::ReadCompleteJsonMessage(
    TSharedPtr<FJsonObject>& OutJsonObject,
    FString& OutMessageText,
    FString& OutError
)
{
    TArray<uint8> MessageBytes;
    MessageBytes.Reserve(ReceiveBufferSize);

    uint8 Buffer[ReceiveBufferSize];
    const double StartedAtSeconds = FPlatformTime::Seconds();

    while (bRunning && ClientSocket.IsValid())
    {
        int32 BytesRead = 0;
        if (ClientSocket->Recv(Buffer, sizeof(Buffer), BytesRead))
        {
            if (BytesRead == 0)
            {
                OutError = MessageBytes.IsEmpty()
                    ? TEXT("client disconnected before sending data")
                    : TEXT("client disconnected before sending a complete JSON command");
                return MessageBytes.IsEmpty()
                    ? EReadJsonMessageResult::Disconnected
                    : EReadJsonMessageResult::Error;
            }

            if (MessageBytes.Num() + BytesRead > MaxInboundMessageBytes)
            {
                OutError = FString::Printf(
                    TEXT("JSON command exceeds maximum size of %d bytes"),
                    MaxInboundMessageBytes
                );
                return EReadJsonMessageResult::Error;
            }

            MessageBytes.Append(Buffer, BytesRead);
            if (TryParseJsonMessage(MessageBytes, OutJsonObject, OutMessageText))
            {
                UE_LOG(LogTemp, Display, TEXT("MCPServerRunnable: Received complete command (%d bytes)"), MessageBytes.Num());
                return EReadJsonMessageResult::Complete;
            }
        }
        else
        {
            const ESocketErrors LastError = ISocketSubsystem::Get()->GetLastErrorCode();
            if (LastError == SE_EWOULDBLOCK || LastError == SE_EINTR)
            {
                if (FPlatformTime::Seconds() - StartedAtSeconds > ReceiveTimeoutSeconds)
                {
                    OutError = TEXT("timed out waiting for a complete JSON command");
                    return EReadJsonMessageResult::Error;
                }

                FPlatformProcess::Sleep(0.01f);
                continue;
            }

            OutError = FString::Printf(TEXT("socket receive failed with error code %d"), static_cast<int32>(LastError));
            return MessageBytes.IsEmpty()
                ? EReadJsonMessageResult::Disconnected
                : EReadJsonMessageResult::Error;
        }
    }

    OutError = TEXT("server stopped while reading command");
    return EReadJsonMessageResult::Disconnected;
}

void FMCPServerRunnable::ProcessJsonCommand(
    const TSharedPtr<FJsonObject>& JsonObject,
    const FString& ReceivedText
)
{
    if (!JsonObject.IsValid())
    {
        SendUtf8Response(SerializeErrorResponse(TEXT("received JSON command was not an object")));
        return;
    }

    FString CommandType;
    if (!JsonObject->TryGetStringField(TEXT("type"), CommandType))
    {
        UE_LOG(LogTemp, Warning, TEXT("MCPServerRunnable: Missing 'type' field in command: %s"), *ReceivedText);
        SendUtf8Response(SerializeErrorResponse(TEXT("missing required string field: type")));
        return;
    }

    TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
    const TSharedPtr<FJsonObject>* ParamsObject = nullptr;
    if (JsonObject->TryGetObjectField(TEXT("params"), ParamsObject) && ParamsObject && ParamsObject->IsValid())
    {
        Params = *ParamsObject;
    }
    else if (JsonObject->HasField(TEXT("params")))
    {
        SendUtf8Response(SerializeErrorResponse(TEXT("params must be a JSON object when provided")));
        return;
    }

    UE_LOG(LogTemp, Display, TEXT("MCPServerRunnable: Executing command: %s"), *CommandType);
    const FString Response = Bridge
        ? Bridge->ExecuteCommand(CommandType, Params)
        : SerializeErrorResponse(TEXT("Unreal MCP bridge is not available"));

    UE_LOG(LogTemp, Display, TEXT("MCPServerRunnable: Sending response for %s (%d chars)"), *CommandType, Response.Len());
    SendUtf8Response(Response);
}

bool FMCPServerRunnable::SendUtf8Response(const FString& Response)
{
    if (!ClientSocket.IsValid())
    {
        UE_LOG(LogTemp, Warning, TEXT("MCPServerRunnable: Cannot send response without a valid client socket"));
        return false;
    }

    FTCHARToUTF8 ResponseBytes(*Response);
    int32 TotalBytesSent = 0;

    while (TotalBytesSent < ResponseBytes.Length())
    {
        int32 BytesSent = 0;
        const uint8* CurrentData = reinterpret_cast<const uint8*>(ResponseBytes.Get()) + TotalBytesSent;
        const int32 RemainingBytes = ResponseBytes.Length() - TotalBytesSent;

        if (ClientSocket->Send(CurrentData, RemainingBytes, BytesSent) && BytesSent > 0)
        {
            TotalBytesSent += BytesSent;
            continue;
        }

        const ESocketErrors LastError = ISocketSubsystem::Get()->GetLastErrorCode();
        if (LastError == SE_EWOULDBLOCK || LastError == SE_EINTR)
        {
            FPlatformProcess::Sleep(0.01f);
            continue;
        }

        UE_LOG(LogTemp, Warning, TEXT("MCPServerRunnable: Failed to send response. Last error code: %d"), static_cast<int32>(LastError));
        return false;
    }

    UE_LOG(LogTemp, Display, TEXT("MCPServerRunnable: Response sent successfully, bytes: %d"), TotalBytesSent);
    return true;
}
