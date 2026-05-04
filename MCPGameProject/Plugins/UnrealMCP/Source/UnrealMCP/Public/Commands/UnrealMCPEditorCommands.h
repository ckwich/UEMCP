#pragma once

#include "CoreMinimal.h"
#include "Json.h"

/**
 * Handler class for Editor-related MCP commands
 * Handles viewport control, actor manipulation, and level management
 */
class UNREALMCP_API FUnrealMCPEditorCommands
{
public:
    FUnrealMCPEditorCommands();

    // Handle editor commands
    TSharedPtr<FJsonObject> HandleCommand(const FString& CommandType, const TSharedPtr<FJsonObject>& Params);

private:
    // Read-only observability commands
    TSharedPtr<FJsonObject> HandleGetEditorStatus(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleGetOutputLog(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleGetLevelSnapshot(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleGetPIERuntimeSnapshot(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleAssetSearch(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleAssetDependencies(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleAssetReferencers(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleBlueprintQuery(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleListAutomationTests(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleRunAutomationTest(const TSharedPtr<FJsonObject>& Params);

    // Actor manipulation commands
    TSharedPtr<FJsonObject> HandleGetActorsInLevel(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleFindActorsByName(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleSpawnActor(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleDeleteActor(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleSetActorTransform(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleGetActorProperties(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleSetActorProperty(const TSharedPtr<FJsonObject>& Params);

    // Blueprint actor spawning
    TSharedPtr<FJsonObject> HandleSpawnBlueprintActor(const TSharedPtr<FJsonObject>& Params);

    // Level persistence
    TSharedPtr<FJsonObject> HandleSaveCurrentLevel(const TSharedPtr<FJsonObject>& Params);

    // Editor viewport commands
    TSharedPtr<FJsonObject> HandleFocusViewport(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleTakeScreenshot(const TSharedPtr<FJsonObject>& Params);
};
