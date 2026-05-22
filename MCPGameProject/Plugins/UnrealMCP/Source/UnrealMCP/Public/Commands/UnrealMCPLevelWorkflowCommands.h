#pragma once

#include "CoreMinimal.h"
#include "Json.h"

/**
 * Handler class for editor-owned level lifecycle and map construction workflows.
 */
class UNREALMCP_API FUnrealMCPLevelWorkflowCommands
{
public:
    FUnrealMCPLevelWorkflowCommands();

    TSharedPtr<FJsonObject> HandleCommand(
        const FString& CommandType,
        const TSharedPtr<FJsonObject>& Params
    );

private:
    TSharedPtr<FJsonObject> HandleLevelListMaps(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleLevelCreate(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleLevelOpen(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleLevelSave(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleLevelApplyConstructionPlan(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleLevelValidateConstruction(const TSharedPtr<FJsonObject>& Params);
};
