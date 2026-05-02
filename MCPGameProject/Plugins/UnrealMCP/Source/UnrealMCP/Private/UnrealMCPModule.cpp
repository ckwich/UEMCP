#include "UnrealMCPModule.h"
#include "Observability/UEMCPOutputLogCapture.h"
#include "UnrealMCPBridge.h"
#include "Modules/ModuleManager.h"
#include "EditorSubsystem.h"
#include "Editor.h"

#define LOCTEXT_NAMESPACE "FUnrealMCPModule"

void FUnrealMCPModule::StartupModule()
{
	FUEMCPOutputLogCapture::Get().Register();
	UE_LOG(LogTemp, Display, TEXT("Unreal MCP Module has started"));
}

void FUnrealMCPModule::ShutdownModule()
{
	UE_LOG(LogTemp, Display, TEXT("Unreal MCP Module has shut down"));
	FUEMCPOutputLogCapture::Get().Unregister();
}

#undef LOCTEXT_NAMESPACE
	
IMPLEMENT_MODULE(FUnrealMCPModule, UnrealMCP)
