#pragma once

#include "CoreMinimal.h"
#include "Sockets.h"
#include "SocketSubsystem.h"
#include "Json.h"
#include "Interfaces/IPv4/IPv4Address.h"
#include "Commands/UnrealMCPEditorCommands.h"
#include "Commands/UnrealMCPBlueprintCommands.h"
#include "Commands/UnrealMCPBlueprintNodeCommands.h"
#include "Commands/UnrealMCPProjectCommands.h"
#include "Commands/UnrealMCPUMGCommands.h"
#include "Commands/UnrealMCPAssetWorkflowCommands.h"
#include "Commands/UnrealMCPLevelWorkflowCommands.h"

class FMCPServerRunnable;

/**
 * Module-owned MCP bridge.
 *
 * Handles communication between external tools and the Unreal Editor
 * through a TCP socket connection. Commands are received as JSON and
 * routed to appropriate command handlers.
 */
class UNREALMCP_API FUnrealMCPBridge
{
public:
	FUnrealMCPBridge();
	~FUnrealMCPBridge();

	// Server functions
	void StartServer();
	void StopServer();
	bool IsRunning() const { return bIsRunning; }

	// Command execution
	FString ExecuteCommand(const FString& CommandType, const TSharedPtr<FJsonObject>& Params);

private:
	// Server state
	bool bIsRunning;
	TSharedPtr<FSocket> ListenerSocket;
	TSharedPtr<FSocket> ConnectionSocket;
	FMCPServerRunnable* ServerRunnable;
	FRunnableThread* ServerThread;

	// Server configuration
	FIPv4Address ServerAddress;
	uint16 Port;

	// Command handler instances
	TSharedPtr<FUnrealMCPEditorCommands> EditorCommands;
	TSharedPtr<FUnrealMCPBlueprintCommands> BlueprintCommands;
	TSharedPtr<FUnrealMCPBlueprintNodeCommands> BlueprintNodeCommands;
	TSharedPtr<FUnrealMCPProjectCommands> ProjectCommands;
	TSharedPtr<FUnrealMCPUMGCommands> UMGCommands;
	TSharedPtr<FUnrealMCPAssetWorkflowCommands> AssetWorkflowCommands;
	TSharedPtr<FUnrealMCPLevelWorkflowCommands> LevelWorkflowCommands;
};
