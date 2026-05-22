#pragma once

#include "CoreMinimal.h"
#include "Json.h"

/**
 * Handler class for interactive asset workflow MCP commands.
 * Starts with read-only intake evidence; later slices can add editor-backed
 * mutation here without mixing asset workflow policy into generic editor tools.
 */
class UNREALMCP_API FUnrealMCPAssetWorkflowCommands
{
public:
    FUnrealMCPAssetWorkflowCommands();

    TSharedPtr<FJsonObject> HandleCommand(
        const FString& CommandType,
        const TSharedPtr<FJsonObject>& Params
    );

private:
    TSharedPtr<FJsonObject> HandleAssetIntakeSnapshot(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleAssetImportFromDisk(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleAssetRename(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleAssetMove(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleAssetDuplicate(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleAssetDelete(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleAssetSavePackages(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleAssetFixupRedirectors(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleAssetPrepareForLevel(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleAssetCreateBlueprintWrapper(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleAssetPlaceInLevel(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleAssetValidateLevelPlacements(const TSharedPtr<FJsonObject>& Params);
};
