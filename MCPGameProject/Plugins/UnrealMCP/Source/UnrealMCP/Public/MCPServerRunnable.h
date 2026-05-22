#pragma once

#include "CoreMinimal.h"
#include "HAL/Runnable.h"
#include "Sockets.h"
#include "Interfaces/IPv4/IPv4Address.h"

class UUnrealMCPBridge;
class FJsonObject;

/**
 * Runnable class for the MCP server thread
 */
class FMCPServerRunnable : public FRunnable
{
public:
	FMCPServerRunnable(UUnrealMCPBridge* InBridge, TSharedPtr<FSocket> InListenerSocket);
	virtual ~FMCPServerRunnable();

	// FRunnable interface
	virtual bool Init() override;
	virtual uint32 Run() override;
	virtual void Stop() override;
	virtual void Exit() override;

protected:
private:
	enum class EReadJsonMessageResult
	{
		Complete,
		Disconnected,
		Error
	};

	EReadJsonMessageResult ReadCompleteJsonMessage(
		TSharedPtr<FJsonObject>& OutJsonObject,
		FString& OutMessageText,
		FString& OutError
	);
	void ProcessJsonCommand(const TSharedPtr<FJsonObject>& JsonObject, const FString& ReceivedText);
	bool SendUtf8Response(const FString& Response);

	UUnrealMCPBridge* Bridge;
	TSharedPtr<FSocket> ListenerSocket;
	TSharedPtr<FSocket> ClientSocket;
	bool bRunning;
};
