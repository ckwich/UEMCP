#include "Commands/UnrealMCPLevelWorkflowCommands.h"
#include "Commands/UnrealMCPCommonUtils.h"

#include "AssetRegistry/ARFilter.h"
#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Engine/Blueprint.h"
#include "Engine/Level.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "FileHelpers.h"
#include "GameFramework/Actor.h"
#include "Misc/PackageName.h"
#include "Modules/ModuleManager.h"
#include "ScopedTransaction.h"
#include "Subsystems/EditorActorSubsystem.h"
#include "Subsystems/EditorAssetSubsystem.h"
#include "UObject/Package.h"
#include "UObject/SoftObjectPath.h"

namespace
{
    constexpr int32 MaxLevelListLimit = 10000;
    constexpr int32 MaxLevelOperationLimit = 1000;

    TSharedPtr<FJsonObject> ErrorResponse(const FString& Message)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(Message);
    }

    FString TrimTrailingSlashes(FString Value)
    {
        while (Value.Len() > 1 && Value.EndsWith(TEXT("/")))
        {
            Value.LeftChopInline(1);
        }
        return Value;
    }

    FString NormalizePackagePath(FString Path)
    {
        Path = Path.TrimStartAndEnd();
        Path.ReplaceInline(TEXT("\\"), TEXT("/"));
        return TrimTrailingSlashes(Path);
    }

    bool HasWildcard(const FString& Value)
    {
        return Value.Contains(TEXT("*")) || Value.Contains(TEXT("?"));
    }

    bool IsExactGamePackagePath(const FString& Path)
    {
        return Path.StartsWith(TEXT("/Game/")) && !HasWildcard(Path);
    }

    bool IsGameRootPath(const FString& Path)
    {
        return (Path == TEXT("/Game") || Path.StartsWith(TEXT("/Game/"))) && !HasWildcard(Path);
    }

    FString PackageLeafName(const FString& PackagePath)
    {
        FString Leaf = PackagePath;
        int32 SlashIndex = INDEX_NONE;
        if (Leaf.FindLastChar(TEXT('/'), SlashIndex))
        {
            Leaf = Leaf.Mid(SlashIndex + 1);
        }
        return Leaf;
    }

    FString ObjectPathFromPackagePath(const FString& PackagePath)
    {
        return FString::Printf(TEXT("%s.%s"), *PackagePath, *PackageLeafName(PackagePath));
    }

    FString CurrentMapFromWorld(UWorld* World)
    {
        if (!World)
        {
            return FString();
        }
        return World->GetOutermost() ? World->GetOutermost()->GetName() : World->GetMapName();
    }

    UWorld* ResolveEditorWorld(FString& OutError)
    {
        if (!GEditor)
        {
            OutError = TEXT("Editor is not available");
            return nullptr;
        }

        UWorld* EditorWorld = GEditor->GetEditorWorldContext().World();
        if (EditorWorld && EditorWorld->WorldType == EWorldType::Editor)
        {
            return EditorWorld;
        }

        for (const FWorldContext& WorldContext : GEditor->GetWorldContexts())
        {
            UWorld* World = WorldContext.World();
            if (World && WorldContext.WorldType == EWorldType::Editor)
            {
                return World;
            }
        }

        OutError = TEXT("Failed to resolve editor world");
        return nullptr;
    }

    FString MapFilenameFromPackagePath(const FString& PackagePath)
    {
        return FPackageName::LongPackageNameToFilename(
            PackagePath,
            FPackageName::GetMapPackageExtension()
        );
    }

    TArray<TSharedPtr<FJsonValue>> StringsToJsonArray(const TArray<FString>& Strings)
    {
        TArray<TSharedPtr<FJsonValue>> Values;
        Values.Reserve(Strings.Num());
        for (const FString& StringValue : Strings)
        {
            Values.Add(MakeShared<FJsonValueString>(StringValue));
        }
        return Values;
    }

    TArray<TSharedPtr<FJsonValue>> VectorToJsonArray(const FVector& Vector)
    {
        TArray<TSharedPtr<FJsonValue>> Values;
        Values.Add(MakeShared<FJsonValueNumber>(Vector.X));
        Values.Add(MakeShared<FJsonValueNumber>(Vector.Y));
        Values.Add(MakeShared<FJsonValueNumber>(Vector.Z));
        return Values;
    }

    TArray<TSharedPtr<FJsonValue>> RotatorToJsonArray(const FRotator& Rotator)
    {
        TArray<TSharedPtr<FJsonValue>> Values;
        Values.Add(MakeShared<FJsonValueNumber>(Rotator.Pitch));
        Values.Add(MakeShared<FJsonValueNumber>(Rotator.Yaw));
        Values.Add(MakeShared<FJsonValueNumber>(Rotator.Roll));
        return Values;
    }

    TArray<FString> GetStringArrayField(const TSharedPtr<FJsonObject>& Params, const FString& FieldName)
    {
        TArray<FString> Results;
        if (!Params.IsValid())
        {
            return Results;
        }

        const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
        if (!Params->TryGetArrayField(FieldName, Values) || !Values)
        {
            return Results;
        }

        for (const TSharedPtr<FJsonValue>& Value : *Values)
        {
            if (!Value.IsValid())
            {
                continue;
            }
            const FString Text = Value->AsString().TrimStartAndEnd();
            if (!Text.IsEmpty())
            {
                Results.Add(Text);
            }
        }
        return Results;
    }

    FVector GetVectorField(
        const TSharedPtr<FJsonObject>& Object,
        const FString& FieldName,
        const FVector& DefaultValue
    )
    {
        if (!Object.IsValid() || !Object->HasField(FieldName))
        {
            return DefaultValue;
        }
        return FUnrealMCPCommonUtils::GetVectorFromJson(Object, FieldName);
    }

    FRotator GetRotatorField(
        const TSharedPtr<FJsonObject>& Object,
        const FString& FieldName,
        const FRotator& DefaultValue
    )
    {
        if (!Object.IsValid() || !Object->HasField(FieldName))
        {
            return DefaultValue;
        }
        return FUnrealMCPCommonUtils::GetRotatorFromJson(Object, FieldName);
    }

    void AddUniquePackage(TArray<UPackage*>& Packages, UPackage* Package)
    {
        if (Package)
        {
            Packages.AddUnique(Package);
        }
    }

    TArray<UPackage*> CollectWorldPackages(UWorld* World, bool bIncludeExternalActorPackages)
    {
        TArray<UPackage*> Packages;
        if (!World)
        {
            return Packages;
        }

        AddUniquePackage(Packages, World->GetOutermost());
        if (World->PersistentLevel)
        {
            AddUniquePackage(Packages, World->PersistentLevel->GetOutermost());
        }

        if (bIncludeExternalActorPackages)
        {
            for (TActorIterator<AActor> ActorIt(World); ActorIt; ++ActorIt)
            {
                AActor* Actor = *ActorIt;
                if (!Actor)
                {
                    continue;
                }
                AddUniquePackage(Packages, Actor->GetPackage());
                AddUniquePackage(Packages, Actor->GetExternalPackage());
            }
        }

        return Packages;
    }

    TArray<FString> PackageNames(const TArray<UPackage*>& Packages, bool bOnlyDirty = false)
    {
        TArray<FString> Names;
        for (UPackage* Package : Packages)
        {
            if (!Package)
            {
                continue;
            }
            if (bOnlyDirty && !Package->IsDirty())
            {
                continue;
            }
            Names.Add(Package->GetName());
        }
        Names.Sort();
        return Names;
    }

    bool MapPackageExists(IAssetRegistry& AssetRegistry, const FString& PackagePath)
    {
        const FString ObjectPath = ObjectPathFromPackagePath(PackagePath);
        if (AssetRegistry.GetAssetByObjectPath(FSoftObjectPath(ObjectPath)).IsValid())
        {
            return true;
        }

        const FString MapFilename = MapFilenameFromPackagePath(PackagePath);
        return FPaths::FileExists(MapFilename);
    }

    AActor* FindActorByExactName(UWorld* World, const FString& ActorName)
    {
        if (!World || ActorName.IsEmpty())
        {
            return nullptr;
        }

        for (TActorIterator<AActor> ActorIt(World); ActorIt; ++ActorIt)
        {
            AActor* Actor = *ActorIt;
            if (Actor && Actor->GetName() == ActorName)
            {
                return Actor;
            }
        }

        return nullptr;
    }

    UEditorActorSubsystem* GetEditorActorSubsystem()
    {
        return GEditor ? GEditor->GetEditorSubsystem<UEditorActorSubsystem>() : nullptr;
    }

    UEditorAssetSubsystem* GetEditorAssetSubsystem()
    {
        return GEditor ? GEditor->GetEditorSubsystem<UEditorAssetSubsystem>() : nullptr;
    }

    UClass* ResolveActorClass(const FString& ActorClassPath)
    {
        const FString CleanPath = ActorClassPath.TrimStartAndEnd();
        if (CleanPath.IsEmpty())
        {
            return nullptr;
        }

        UClass* ActorClass = StaticLoadClass(AActor::StaticClass(), nullptr, *CleanPath);
        if (!ActorClass && !CleanPath.StartsWith(TEXT("/")))
        {
            ActorClass = StaticLoadClass(
                AActor::StaticClass(),
                nullptr,
                *FString::Printf(TEXT("/Script/Engine.%s"), *CleanPath)
            );
        }
        if (!ActorClass)
        {
            ActorClass = FindObject<UClass>(nullptr, *CleanPath);
        }
        return ActorClass && ActorClass->IsChildOf(AActor::StaticClass()) ? ActorClass : nullptr;
    }

    void ApplyActorTransform(AActor* Actor, const FVector& Location, const FRotator& Rotation, const FVector& Scale)
    {
        if (!Actor)
        {
            return;
        }

        Actor->SetActorLocation(Location);
        Actor->SetActorRotation(Rotation);
        Actor->SetActorScale3D(Scale);
        Actor->PostEditMove(true);
        Actor->PostEditChange();
        if (ULevel* Level = Actor->GetLevel())
        {
            Level->MarkPackageDirty();
        }
        if (UPackage* Package = Actor->GetPackage())
        {
            Package->SetDirtyFlag(true);
        }
        if (UPackage* ExternalPackage = Actor->GetExternalPackage())
        {
            ExternalPackage->SetDirtyFlag(true);
        }
    }

    void ApplyActorIdentity(AActor* Actor, const TSharedPtr<FJsonObject>& Operation)
    {
        if (!Actor || !Operation.IsValid())
        {
            return;
        }

        FString ActorName;
        Operation->TryGetStringField(TEXT("actor_name"), ActorName);
        FString Label;
        Operation->TryGetStringField(TEXT("label"), Label);
        FString FolderPath;
        Operation->TryGetStringField(TEXT("folder_path"), FolderPath);

        Actor->Modify();
        if (!ActorName.IsEmpty())
        {
            Actor->SetActorLabel(Label.IsEmpty() ? ActorName : Label);
            if (Actor->GetName() != ActorName)
            {
                Actor->Rename(*ActorName);
            }
        }
        else if (!Label.IsEmpty())
        {
            Actor->SetActorLabel(Label);
        }

        if (!FolderPath.IsEmpty())
        {
            Actor->SetFolderPath(FName(*FolderPath));
        }

        Actor->Tags.Reset();
        for (const FString& Tag : GetStringArrayField(Operation, TEXT("tags")))
        {
            Actor->Tags.Add(FName(*Tag));
        }
    }

    TSharedPtr<FJsonObject> ActorEvidence(AActor* Actor)
    {
        TSharedPtr<FJsonObject> Evidence = MakeShared<FJsonObject>();
        if (!Actor)
        {
            return Evidence;
        }

        Evidence->SetStringField(TEXT("name"), Actor->GetName());
        Evidence->SetStringField(TEXT("label"), Actor->GetActorLabel());
        Evidence->SetStringField(TEXT("class"), Actor->GetClass() ? Actor->GetClass()->GetName() : FString());
        Evidence->SetStringField(TEXT("class_path"), Actor->GetClass() ? Actor->GetClass()->GetPathName() : FString());
        Evidence->SetStringField(TEXT("path"), Actor->GetPathName());
        Evidence->SetStringField(TEXT("folder_path"), Actor->GetFolderPath().ToString());
        Evidence->SetArrayField(TEXT("location"), VectorToJsonArray(Actor->GetActorLocation()));
        Evidence->SetArrayField(TEXT("rotation"), RotatorToJsonArray(Actor->GetActorRotation()));
        Evidence->SetArrayField(TEXT("scale"), VectorToJsonArray(Actor->GetActorScale3D()));

        TArray<FString> Tags;
        for (const FName& Tag : Actor->Tags)
        {
            Tags.Add(Tag.ToString());
        }
        Tags.Sort();
        Evidence->SetArrayField(TEXT("tags"), StringsToJsonArray(Tags));
        return Evidence;
    }

    bool SaveWorldPackages(
        UWorld* World,
        bool bOnlyIfDirty,
        bool bIncludeExternalActorPackages,
        TArray<FString>& OutDirtyBefore,
        TArray<FString>& OutSavedPackages,
        TArray<FString>& OutDirtyAfter
    )
    {
        TArray<UPackage*> Packages = CollectWorldPackages(World, bIncludeExternalActorPackages);
        OutDirtyBefore = PackageNames(Packages, true);

        TArray<UPackage*> PackagesToSave;
        for (UPackage* Package : Packages)
        {
            if (!Package)
            {
                continue;
            }
            if (bOnlyIfDirty && !Package->IsDirty())
            {
                continue;
            }
            PackagesToSave.Add(Package);
            OutSavedPackages.Add(Package->GetName());
        }

        bool bSaved = true;
        if (!PackagesToSave.IsEmpty())
        {
            bSaved = UEditorLoadingAndSavingUtils::SavePackages(PackagesToSave, bOnlyIfDirty);
        }
        OutSavedPackages.Sort();
        OutDirtyAfter = PackageNames(Packages, true);
        return bSaved;
    }
}

FUnrealMCPLevelWorkflowCommands::FUnrealMCPLevelWorkflowCommands()
{
}

TSharedPtr<FJsonObject> FUnrealMCPLevelWorkflowCommands::HandleCommand(
    const FString& CommandType,
    const TSharedPtr<FJsonObject>& Params
)
{
    if (CommandType == TEXT("level_list_maps"))
    {
        return HandleLevelListMaps(Params);
    }
    if (CommandType == TEXT("level_create"))
    {
        return HandleLevelCreate(Params);
    }
    if (CommandType == TEXT("level_open"))
    {
        return HandleLevelOpen(Params);
    }
    if (CommandType == TEXT("level_save"))
    {
        return HandleLevelSave(Params);
    }
    if (CommandType == TEXT("level_apply_construction_plan"))
    {
        return HandleLevelApplyConstructionPlan(Params);
    }
    if (CommandType == TEXT("level_validate_construction"))
    {
        return HandleLevelValidateConstruction(Params);
    }

    return ErrorResponse(FString::Printf(TEXT("Unknown level workflow command: %s"), *CommandType));
}

TSharedPtr<FJsonObject> FUnrealMCPLevelWorkflowCommands::HandleLevelListMaps(
    const TSharedPtr<FJsonObject>& Params
)
{
    TArray<FString> Roots = GetStringArrayField(Params, TEXT("roots"));
    if (Roots.IsEmpty())
    {
        return ErrorResponse(TEXT("level_list_maps requires roots"));
    }

    int32 Limit = 500;
    double LimitValue = 500.0;
    if (Params.IsValid() && Params->TryGetNumberField(TEXT("limit"), LimitValue))
    {
        Limit = FMath::Clamp(static_cast<int32>(LimitValue), 1, MaxLevelListLimit);
    }

    FARFilter Filter;
    Filter.bRecursivePaths = true;
    Filter.bIncludeOnlyOnDiskAssets = false;
    Filter.ClassPaths.Add(UWorld::StaticClass()->GetClassPathName());
    for (FString& Root : Roots)
    {
        Root = NormalizePackagePath(Root);
        if (!IsGameRootPath(Root))
        {
            return ErrorResponse(TEXT("roots must be exact /Game paths"));
        }
        Filter.PackagePaths.Add(*Root);
    }

    IAssetRegistry& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(
        TEXT("AssetRegistry")
    ).Get();

    TArray<FAssetData> AssetDataList;
    AssetRegistry.GetAssets(Filter, AssetDataList);
    AssetDataList.Sort([](const FAssetData& Left, const FAssetData& Right)
    {
        return Left.PackageName.LexicalLess(Right.PackageName);
    });

    TArray<TSharedPtr<FJsonValue>> Maps;
    int32 MatchedCount = 0;
    for (const FAssetData& AssetData : AssetDataList)
    {
        ++MatchedCount;
        if (Maps.Num() >= Limit)
        {
            continue;
        }

        TSharedPtr<FJsonObject> MapObj = MakeShared<FJsonObject>();
        MapObj->SetStringField(TEXT("package_name"), AssetData.PackageName.ToString());
        MapObj->SetStringField(TEXT("asset_name"), AssetData.AssetName.ToString());
        MapObj->SetStringField(TEXT("object_path"), AssetData.GetObjectPathString());
        MapObj->SetStringField(TEXT("package_path"), AssetData.PackagePath.ToString());
        MapObj->SetStringField(TEXT("asset_class"), AssetData.AssetClassPath.ToString());
        MapObj->SetBoolField(TEXT("is_world_asset"), true);
        Maps.Add(MakeShared<FJsonValueObject>(MapObj));
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetArrayField(TEXT("roots"), StringsToJsonArray(Roots));
    ResultObj->SetNumberField(TEXT("map_count"), Maps.Num());
    ResultObj->SetNumberField(TEXT("matched_map_count"), MatchedCount);
    ResultObj->SetBoolField(TEXT("truncated"), MatchedCount > Maps.Num());
    ResultObj->SetBoolField(TEXT("asset_registry_loading"), AssetRegistry.IsLoadingAssets());
    ResultObj->SetArrayField(TEXT("maps"), Maps);
    return ResultObj;
}

TSharedPtr<FJsonObject> FUnrealMCPLevelWorkflowCommands::HandleLevelCreate(
    const TSharedPtr<FJsonObject>& Params
)
{
    FString PackagePath;
    if (!Params->TryGetStringField(TEXT("package_path"), PackagePath))
    {
        return ErrorResponse(TEXT("Missing package_path"));
    }
    PackagePath = NormalizePackagePath(PackagePath);
    if (!IsExactGamePackagePath(PackagePath))
    {
        return ErrorResponse(TEXT("package_path must be an exact /Game map package path"));
    }

    FString TemplatePath;
    Params->TryGetStringField(TEXT("template_path"), TemplatePath);
    TemplatePath = NormalizePackagePath(TemplatePath);
    if (!TemplatePath.IsEmpty() && !IsExactGamePackagePath(TemplatePath))
    {
        return ErrorResponse(TEXT("template_path must be an exact /Game map package path"));
    }

    bool bSaveExisting = false;
    Params->TryGetBoolField(TEXT("save_existing"), bSaveExisting);
    bool bSaveNewLevel = true;
    Params->TryGetBoolField(TEXT("save_new_level"), bSaveNewLevel);
    bool bFailIfExists = true;
    Params->TryGetBoolField(TEXT("fail_if_exists"), bFailIfExists);
    bool bDryRun = true;
    Params->TryGetBoolField(TEXT("dry_run"), bDryRun);

    FString WorldError;
    UWorld* CurrentWorld = ResolveEditorWorld(WorldError);
    if (!CurrentWorld)
    {
        return ErrorResponse(WorldError);
    }

    IAssetRegistry& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(
        TEXT("AssetRegistry")
    ).Get();
    const bool bExists = MapPackageExists(AssetRegistry, PackagePath);
    if (bExists && bFailIfExists)
    {
        return ErrorResponse(FString::Printf(TEXT("Map package already exists: %s"), *PackagePath));
    }

    TArray<UPackage*> CurrentPackages = CollectWorldPackages(CurrentWorld, true);
    const TArray<FString> DirtyBefore = PackageNames(CurrentPackages, true);
    if (!DirtyBefore.IsEmpty() && !bSaveExisting)
    {
        TSharedPtr<FJsonObject> ErrorObj = ErrorResponse(TEXT("Current editor map has dirty packages; pass save_existing=true to save before creating a new level"));
        ErrorObj->SetArrayField(TEXT("dirty_packages"), StringsToJsonArray(DirtyBefore));
        return ErrorObj;
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("package_path"), PackagePath);
    ResultObj->SetStringField(TEXT("template_path"), TemplatePath);
    ResultObj->SetBoolField(TEXT("dry_run"), bDryRun);
    ResultObj->SetBoolField(TEXT("exists_before"), bExists);
    ResultObj->SetArrayField(TEXT("dirty_packages_before"), StringsToJsonArray(DirtyBefore));

    if (bDryRun)
    {
        ResultObj->SetBoolField(TEXT("created"), false);
        ResultObj->SetBoolField(TEXT("saved"), false);
        ResultObj->SetStringField(TEXT("planned_operation"), TemplatePath.IsEmpty() ? TEXT("new_blank_map") : TEXT("new_map_from_template"));
        return ResultObj;
    }

    if (!DirtyBefore.IsEmpty() && bSaveExisting)
    {
        TArray<FString> SavedExisting;
        TArray<FString> DirtyAfterSave;
        TArray<FString> DirtyBeforeSave;
        if (!SaveWorldPackages(CurrentWorld, true, true, DirtyBeforeSave, SavedExisting, DirtyAfterSave))
        {
            return ErrorResponse(TEXT("Failed to save existing dirty map packages before creating new level"));
        }
        ResultObj->SetArrayField(TEXT("saved_existing_packages"), StringsToJsonArray(SavedExisting));
    }

    UWorld* NewWorld = nullptr;
    if (TemplatePath.IsEmpty())
    {
        NewWorld = UEditorLoadingAndSavingUtils::NewBlankMap(false);
    }
    else
    {
        NewWorld = UEditorLoadingAndSavingUtils::NewMapFromTemplate(MapFilenameFromPackagePath(TemplatePath), false);
    }

    if (!NewWorld)
    {
        return ErrorResponse(TEXT("Failed to create new editor level"));
    }

    bool bSaved = false;
    TArray<FString> SavedPackages;
    if (bSaveNewLevel)
    {
        bSaved = UEditorLoadingAndSavingUtils::SaveMap(NewWorld, PackagePath);
        if (!bSaved)
        {
            return ErrorResponse(FString::Printf(TEXT("Failed to save new level: %s"), *PackagePath));
        }
        SavedPackages.Add(PackagePath);
    }

    ResultObj->SetBoolField(TEXT("created"), true);
    ResultObj->SetBoolField(TEXT("saved"), bSaved);
    ResultObj->SetStringField(TEXT("current_map"), CurrentMapFromWorld(NewWorld));
    ResultObj->SetArrayField(TEXT("saved_packages"), StringsToJsonArray(SavedPackages));
    ResultObj->SetArrayField(
        TEXT("dirty_packages_after"),
        StringsToJsonArray(PackageNames(CollectWorldPackages(NewWorld, true), true))
    );
    return ResultObj;
}

TSharedPtr<FJsonObject> FUnrealMCPLevelWorkflowCommands::HandleLevelOpen(
    const TSharedPtr<FJsonObject>& Params
)
{
    FString PackagePath;
    if (!Params->TryGetStringField(TEXT("package_path"), PackagePath))
    {
        return ErrorResponse(TEXT("Missing package_path"));
    }
    PackagePath = NormalizePackagePath(PackagePath);
    if (!IsExactGamePackagePath(PackagePath))
    {
        return ErrorResponse(TEXT("package_path must be an exact /Game map package path"));
    }

    bool bSaveExisting = false;
    Params->TryGetBoolField(TEXT("save_existing"), bSaveExisting);
    bool bRequireExists = true;
    Params->TryGetBoolField(TEXT("require_exists"), bRequireExists);

    FString WorldError;
    UWorld* CurrentWorld = ResolveEditorWorld(WorldError);
    if (!CurrentWorld)
    {
        return ErrorResponse(WorldError);
    }
    const FString CurrentMapBefore = CurrentMapFromWorld(CurrentWorld);

    IAssetRegistry& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(
        TEXT("AssetRegistry")
    ).Get();
    if (bRequireExists && !MapPackageExists(AssetRegistry, PackagePath))
    {
        return ErrorResponse(FString::Printf(TEXT("Map package not found: %s"), *PackagePath));
    }

    TArray<UPackage*> CurrentPackages = CollectWorldPackages(CurrentWorld, true);
    const TArray<FString> DirtyBefore = PackageNames(CurrentPackages, true);
    if (!DirtyBefore.IsEmpty() && !bSaveExisting)
    {
        TSharedPtr<FJsonObject> ErrorObj = ErrorResponse(TEXT("Current editor map has dirty packages; pass save_existing=true to save before opening another level"));
        ErrorObj->SetArrayField(TEXT("dirty_packages"), StringsToJsonArray(DirtyBefore));
        return ErrorObj;
    }
    if (!DirtyBefore.IsEmpty() && bSaveExisting)
    {
        TArray<FString> SavedExisting;
        TArray<FString> DirtyAfterSave;
        TArray<FString> DirtyBeforeSave;
        if (!SaveWorldPackages(CurrentWorld, true, true, DirtyBeforeSave, SavedExisting, DirtyAfterSave))
        {
            return ErrorResponse(TEXT("Failed to save existing dirty map packages before opening level"));
        }
    }

    UWorld* OpenedWorld = UEditorLoadingAndSavingUtils::LoadMap(MapFilenameFromPackagePath(PackagePath));
    if (!OpenedWorld)
    {
        return ErrorResponse(FString::Printf(TEXT("Failed to open map: %s"), *PackagePath));
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetBoolField(TEXT("opened"), true);
    ResultObj->SetStringField(TEXT("package_path"), PackagePath);
    ResultObj->SetStringField(TEXT("current_map_before"), CurrentMapBefore);
    ResultObj->SetStringField(TEXT("current_map_after"), CurrentMapFromWorld(OpenedWorld));
    ResultObj->SetArrayField(TEXT("dirty_packages_before"), StringsToJsonArray(DirtyBefore));
    return ResultObj;
}

TSharedPtr<FJsonObject> FUnrealMCPLevelWorkflowCommands::HandleLevelSave(
    const TSharedPtr<FJsonObject>& Params
)
{
    FString PackagePath;
    if (Params.IsValid())
    {
        Params->TryGetStringField(TEXT("package_path"), PackagePath);
    }
    PackagePath = NormalizePackagePath(PackagePath);
    if (!PackagePath.IsEmpty() && !IsExactGamePackagePath(PackagePath))
    {
        return ErrorResponse(TEXT("package_path must be an exact /Game map package path"));
    }

    bool bOnlyIfDirty = true;
    Params->TryGetBoolField(TEXT("only_if_dirty"), bOnlyIfDirty);
    bool bIncludeExternalActorPackages = true;
    Params->TryGetBoolField(TEXT("include_external_actor_packages"), bIncludeExternalActorPackages);

    FString WorldError;
    UWorld* World = ResolveEditorWorld(WorldError);
    if (!World)
    {
        return ErrorResponse(WorldError);
    }

    const FString CurrentMap = CurrentMapFromWorld(World);
    if (!PackagePath.IsEmpty() && CurrentMap != PackagePath)
    {
        return ErrorResponse(FString::Printf(
            TEXT("Requested package_path does not match current editor map. Current: %s Requested: %s"),
            *CurrentMap,
            *PackagePath
        ));
    }

    TArray<FString> DirtyBefore;
    TArray<FString> SavedPackages;
    TArray<FString> DirtyAfter;
    const bool bSaved = SaveWorldPackages(
        World,
        bOnlyIfDirty,
        bIncludeExternalActorPackages,
        DirtyBefore,
        SavedPackages,
        DirtyAfter
    );

    if (!bSaved)
    {
        return ErrorResponse(FString::Printf(TEXT("Failed to save level packages for current map: %s"), *CurrentMap));
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetBoolField(TEXT("saved"), !SavedPackages.IsEmpty());
    ResultObj->SetStringField(TEXT("current_map"), CurrentMap);
    ResultObj->SetStringField(TEXT("package_path"), PackagePath);
    ResultObj->SetBoolField(TEXT("only_if_dirty"), bOnlyIfDirty);
    ResultObj->SetBoolField(TEXT("include_external_actor_packages"), bIncludeExternalActorPackages);
    ResultObj->SetArrayField(TEXT("dirty_packages_before"), StringsToJsonArray(DirtyBefore));
    ResultObj->SetArrayField(TEXT("saved_packages"), StringsToJsonArray(SavedPackages));
    ResultObj->SetArrayField(TEXT("dirty_packages_after"), StringsToJsonArray(DirtyAfter));
    return ResultObj;
}

TSharedPtr<FJsonObject> FUnrealMCPLevelWorkflowCommands::HandleLevelApplyConstructionPlan(
    const TSharedPtr<FJsonObject>& Params
)
{
    FString TargetMap;
    Params->TryGetStringField(TEXT("target_map"), TargetMap);
    TargetMap = NormalizePackagePath(TargetMap);
    if (!TargetMap.IsEmpty() && !IsExactGamePackagePath(TargetMap))
    {
        return ErrorResponse(TEXT("target_map must be an exact /Game map package path"));
    }

    const TArray<TSharedPtr<FJsonValue>>* OperationValues = nullptr;
    if (!Params->TryGetArrayField(TEXT("operations"), OperationValues) || !OperationValues || OperationValues->IsEmpty())
    {
        return ErrorResponse(TEXT("level_apply_construction_plan requires operations"));
    }

    bool bOpenLevel = true;
    Params->TryGetBoolField(TEXT("open_level"), bOpenLevel);
    bool bCreateIfMissing = false;
    Params->TryGetBoolField(TEXT("create_if_missing"), bCreateIfMissing);
    bool bSaveLevel = false;
    Params->TryGetBoolField(TEXT("save_level"), bSaveLevel);
    bool bDryRun = true;
    Params->TryGetBoolField(TEXT("dry_run"), bDryRun);

    FString WorldError;
    UWorld* World = ResolveEditorWorld(WorldError);
    if (!World)
    {
        return ErrorResponse(WorldError);
    }

    IAssetRegistry& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(
        TEXT("AssetRegistry")
    ).Get();

    const FString CurrentMapBefore = CurrentMapFromWorld(World);
    bool bOpenedMap = false;
    bool bCreatedMap = false;
    if (!TargetMap.IsEmpty() && CurrentMapBefore != TargetMap && bOpenLevel && !bDryRun)
    {
        if (!MapPackageExists(AssetRegistry, TargetMap))
        {
            if (!bCreateIfMissing)
            {
                return ErrorResponse(FString::Printf(TEXT("Target map does not exist: %s"), *TargetMap));
            }
            UWorld* NewWorld = UEditorLoadingAndSavingUtils::NewBlankMap(false);
            if (!NewWorld || !UEditorLoadingAndSavingUtils::SaveMap(NewWorld, TargetMap))
            {
                return ErrorResponse(FString::Printf(TEXT("Failed to create target map: %s"), *TargetMap));
            }
            bCreatedMap = true;
        }

        World = UEditorLoadingAndSavingUtils::LoadMap(MapFilenameFromPackagePath(TargetMap));
        if (!World)
        {
            return ErrorResponse(FString::Printf(TEXT("Failed to open target map: %s"), *TargetMap));
        }
        bOpenedMap = true;
    }

    TArray<TSharedPtr<FJsonValue>> CreatedActors;
    TArray<TSharedPtr<FJsonValue>> UpdatedActors;
    TArray<TSharedPtr<FJsonValue>> DeletedActors;
    TArray<FString> WouldCreate;
    TArray<FString> WouldUpdate;
    TArray<FString> WouldDelete;
    TArray<FString> MissingSources;

    const int32 OperationCount = FMath::Min(OperationValues->Num(), MaxLevelOperationLimit);
    if (!bDryRun)
    {
        FScopedTransaction Transaction(NSLOCTEXT("UnrealMCP", "ApplyLevelConstructionPlan", "Apply Level Construction Plan"));
        World->Modify();
        if (World->PersistentLevel)
        {
            World->PersistentLevel->Modify();
        }
    }

    for (int32 Index = 0; Index < OperationCount; ++Index)
    {
        const TSharedPtr<FJsonObject> Operation = (*OperationValues)[Index]->AsObject();
        if (!Operation.IsValid())
        {
            return ErrorResponse(TEXT("All level construction operations must be objects"));
        }

        FString Op;
        Operation->TryGetStringField(TEXT("op"), Op);
        FString ActorName;
        Operation->TryGetStringField(TEXT("actor_name"), ActorName);
        if (ActorName.IsEmpty() || HasWildcard(ActorName))
        {
            return ErrorResponse(TEXT("Every operation requires an exact actor_name"));
        }

        AActor* ExistingActor = FindActorByExactName(World, ActorName);
        if (bDryRun)
        {
            if (Op == TEXT("delete_actor"))
            {
                WouldDelete.Add(ActorName);
            }
            else if (ExistingActor)
            {
                WouldUpdate.Add(ActorName);
            }
            else
            {
                WouldCreate.Add(ActorName);
            }
            continue;
        }

        if (Op == TEXT("delete_actor"))
        {
            if (!ExistingActor)
            {
                continue;
            }
            TSharedPtr<FJsonObject> ActorInfo = ActorEvidence(ExistingActor);
            UEditorActorSubsystem* ActorSubsystem = GetEditorActorSubsystem();
            const bool bDeleted = ActorSubsystem
                ? ActorSubsystem->DestroyActor(ExistingActor)
                : World->EditorDestroyActor(ExistingActor, true);
            if (!bDeleted)
            {
                return ErrorResponse(FString::Printf(TEXT("Failed to delete actor: %s"), *ActorName));
            }
            DeletedActors.Add(MakeShared<FJsonValueObject>(ActorInfo));
            continue;
        }

        AActor* TargetActor = ExistingActor;
        if (!TargetActor && Op == TEXT("ensure_actor"))
        {
            FString AssetPath;
            Operation->TryGetStringField(TEXT("asset_path"), AssetPath);
            FString ActorClassPath;
            Operation->TryGetStringField(TEXT("actor_class"), ActorClassPath);
            const FVector Location = GetVectorField(Operation, TEXT("location"), FVector::ZeroVector);
            const FRotator Rotation = GetRotatorField(Operation, TEXT("rotation"), FRotator::ZeroRotator);

            if (!AssetPath.IsEmpty())
            {
                UObject* Asset = nullptr;
                if (UEditorAssetSubsystem* AssetSubsystem = GetEditorAssetSubsystem())
                {
                    Asset = AssetSubsystem->LoadAsset(AssetPath);
                }
                if (!Asset)
                {
                    MissingSources.Add(AssetPath);
                    continue;
                }
                if (UEditorActorSubsystem* ActorSubsystem = GetEditorActorSubsystem())
                {
                    TargetActor = ActorSubsystem->SpawnActorFromObject(Asset, Location, Rotation);
                }
            }
            else
            {
                UClass* ActorClass = ResolveActorClass(ActorClassPath);
                if (!ActorClass)
                {
                    MissingSources.Add(ActorClassPath);
                    continue;
                }
                if (UEditorActorSubsystem* ActorSubsystem = GetEditorActorSubsystem())
                {
                    TargetActor = ActorSubsystem->SpawnActorFromClass(ActorClass, Location, Rotation);
                }
                else
                {
                    FActorSpawnParameters SpawnParams;
                    SpawnParams.Name = *ActorName;
                    TargetActor = World->SpawnActor<AActor>(ActorClass, Location, Rotation, SpawnParams);
                }
            }

            if (!TargetActor)
            {
                return ErrorResponse(FString::Printf(TEXT("Failed to create actor: %s"), *ActorName));
            }
            CreatedActors.Add(MakeShared<FJsonValueObject>(ActorEvidence(TargetActor)));
        }

        if (!TargetActor)
        {
            return ErrorResponse(FString::Printf(TEXT("Actor not found for operation %s: %s"), *Op, *ActorName));
        }

        TargetActor->Modify();
        ApplyActorIdentity(TargetActor, Operation);
        if (Op == TEXT("ensure_actor") || Op == TEXT("set_actor_transform"))
        {
            ApplyActorTransform(
                TargetActor,
                GetVectorField(Operation, TEXT("location"), TargetActor->GetActorLocation()),
                GetRotatorField(Operation, TEXT("rotation"), TargetActor->GetActorRotation()),
                GetVectorField(Operation, TEXT("scale"), TargetActor->GetActorScale3D())
            );
        }
        if (Op == TEXT("set_actor_folder") || Op == TEXT("set_actor_label") || Op == TEXT("set_actor_tags"))
        {
            ApplyActorTransform(
                TargetActor,
                TargetActor->GetActorLocation(),
                TargetActor->GetActorRotation(),
                TargetActor->GetActorScale3D()
            );
        }
        if (ExistingActor)
        {
            UpdatedActors.Add(MakeShared<FJsonValueObject>(ActorEvidence(TargetActor)));
        }
    }

    TArray<FString> DirtyBeforeSave;
    TArray<FString> SavedPackages;
    TArray<FString> DirtyAfterSave;
    bool bSaved = false;
    if (bSaveLevel && !bDryRun)
    {
        bSaved = SaveWorldPackages(World, true, true, DirtyBeforeSave, SavedPackages, DirtyAfterSave);
        if (!bSaved)
        {
            return ErrorResponse(TEXT("Failed to save level after applying construction plan"));
        }
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetBoolField(TEXT("dry_run"), bDryRun);
    ResultObj->SetStringField(TEXT("target_map"), TargetMap);
    ResultObj->SetStringField(TEXT("current_map_before"), CurrentMapBefore);
    ResultObj->SetStringField(TEXT("current_map_after"), CurrentMapFromWorld(World));
    ResultObj->SetBoolField(TEXT("opened_map"), bOpenedMap);
    ResultObj->SetBoolField(TEXT("created_map"), bCreatedMap);
    ResultObj->SetNumberField(TEXT("operation_count"), OperationCount);
    ResultObj->SetArrayField(TEXT("would_create_actors"), StringsToJsonArray(WouldCreate));
    ResultObj->SetArrayField(TEXT("would_update_actors"), StringsToJsonArray(WouldUpdate));
    ResultObj->SetArrayField(TEXT("would_delete_actors"), StringsToJsonArray(WouldDelete));
    ResultObj->SetArrayField(TEXT("missing_sources"), StringsToJsonArray(MissingSources));
    ResultObj->SetArrayField(TEXT("created_actors"), CreatedActors);
    ResultObj->SetArrayField(TEXT("updated_actors"), UpdatedActors);
    ResultObj->SetArrayField(TEXT("deleted_actors"), DeletedActors);
    ResultObj->SetArrayField(TEXT("changed_packages"), StringsToJsonArray(PackageNames(CollectWorldPackages(World, true), true)));
    ResultObj->SetArrayField(TEXT("saved_packages"), StringsToJsonArray(SavedPackages));
    ResultObj->SetArrayField(TEXT("dirty_packages_after_save"), StringsToJsonArray(DirtyAfterSave));
    return ResultObj;
}

TSharedPtr<FJsonObject> FUnrealMCPLevelWorkflowCommands::HandleLevelValidateConstruction(
    const TSharedPtr<FJsonObject>& Params
)
{
    FString TargetMap;
    Params->TryGetStringField(TEXT("target_map"), TargetMap);
    TargetMap = NormalizePackagePath(TargetMap);
    if (!TargetMap.IsEmpty() && !IsExactGamePackagePath(TargetMap))
    {
        return ErrorResponse(TEXT("target_map must be an exact /Game map package path"));
    }

    const TArray<TSharedPtr<FJsonValue>>* ExpectedValues = nullptr;
    if (!Params->TryGetArrayField(TEXT("expected_actors"), ExpectedValues) || !ExpectedValues || ExpectedValues->IsEmpty())
    {
        return ErrorResponse(TEXT("level_validate_construction requires expected_actors"));
    }

    double LocationTolerance = 1.0;
    Params->TryGetNumberField(TEXT("location_tolerance"), LocationTolerance);

    FString WorldError;
    UWorld* World = ResolveEditorWorld(WorldError);
    if (!World)
    {
        return ErrorResponse(WorldError);
    }

    const FString CurrentMap = CurrentMapFromWorld(World);
    TArray<TSharedPtr<FJsonValue>> Missing;
    TArray<TSharedPtr<FJsonValue>> Mismatched;
    TArray<TSharedPtr<FJsonValue>> Found;
    if (!TargetMap.IsEmpty() && CurrentMap != TargetMap)
    {
        TSharedPtr<FJsonObject> Mismatch = MakeShared<FJsonObject>();
        Mismatch->SetStringField(TEXT("reason"), TEXT("current_map_mismatch"));
        Mismatch->SetStringField(TEXT("current_map"), CurrentMap);
        Mismatch->SetStringField(TEXT("expected_map"), TargetMap);
        Mismatched.Add(MakeShared<FJsonValueObject>(Mismatch));
    }

    for (const TSharedPtr<FJsonValue>& ExpectedValue : *ExpectedValues)
    {
        const TSharedPtr<FJsonObject> Expected = ExpectedValue->AsObject();
        if (!Expected.IsValid())
        {
            return ErrorResponse(TEXT("expected_actors entries must be objects"));
        }

        FString ActorName;
        Expected->TryGetStringField(TEXT("actor_name"), ActorName);
        if (ActorName.IsEmpty())
        {
            return ErrorResponse(TEXT("expected actor entries require actor_name"));
        }

        AActor* Actor = FindActorByExactName(World, ActorName);
        if (!Actor)
        {
            TSharedPtr<FJsonObject> MissingObj = MakeShared<FJsonObject>();
            MissingObj->SetStringField(TEXT("actor_name"), ActorName);
            Missing.Add(MakeShared<FJsonValueObject>(MissingObj));
            continue;
        }

        TArray<FString> ActorMismatches;
        FString ExpectedClass;
        Expected->TryGetStringField(TEXT("class"), ExpectedClass);
        if (!ExpectedClass.IsEmpty())
        {
            const FString ActualClass = Actor->GetClass() ? Actor->GetClass()->GetName() : FString();
            const FString ActualClassPath = Actor->GetClass() ? Actor->GetClass()->GetPathName() : FString();
            if (ActualClass != ExpectedClass && ActualClassPath != ExpectedClass)
            {
                ActorMismatches.Add(FString::Printf(TEXT("class expected=%s actual=%s"), *ExpectedClass, *ActualClass));
            }
        }

        FString ExpectedLabel;
        Expected->TryGetStringField(TEXT("label"), ExpectedLabel);
        if (!ExpectedLabel.IsEmpty() && Actor->GetActorLabel() != ExpectedLabel)
        {
            ActorMismatches.Add(FString::Printf(TEXT("label expected=%s actual=%s"), *ExpectedLabel, *Actor->GetActorLabel()));
        }

        FString ExpectedFolder;
        Expected->TryGetStringField(TEXT("folder_path"), ExpectedFolder);
        if (!ExpectedFolder.IsEmpty() && Actor->GetFolderPath().ToString() != ExpectedFolder)
        {
            ActorMismatches.Add(FString::Printf(TEXT("folder_path expected=%s actual=%s"), *ExpectedFolder, *Actor->GetFolderPath().ToString()));
        }

        for (const FString& ExpectedTag : GetStringArrayField(Expected, TEXT("tags")))
        {
            if (!Actor->Tags.Contains(FName(*ExpectedTag)))
            {
                ActorMismatches.Add(FString::Printf(TEXT("missing tag=%s"), *ExpectedTag));
            }
        }

        if (Expected->HasField(TEXT("location")))
        {
            const FVector ExpectedLocation = FUnrealMCPCommonUtils::GetVectorFromJson(Expected, TEXT("location"));
            const double Distance = FVector::Dist(ExpectedLocation, Actor->GetActorLocation());
            if (Distance > LocationTolerance)
            {
                ActorMismatches.Add(FString::Printf(TEXT("location distance=%f tolerance=%f"), Distance, LocationTolerance));
            }
        }

        TSharedPtr<FJsonObject> Evidence = ActorEvidence(Actor);
        if (ActorMismatches.IsEmpty())
        {
            Found.Add(MakeShared<FJsonValueObject>(Evidence));
        }
        else
        {
            Evidence->SetArrayField(TEXT("mismatches"), StringsToJsonArray(ActorMismatches));
            Mismatched.Add(MakeShared<FJsonValueObject>(Evidence));
        }
    }

    const bool bPassed = Missing.IsEmpty() && Mismatched.IsEmpty();
    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetBoolField(TEXT("passed"), bPassed);
    ResultObj->SetStringField(TEXT("target_map"), TargetMap);
    ResultObj->SetStringField(TEXT("current_map"), CurrentMap);
    ResultObj->SetNumberField(TEXT("expected_actor_count"), ExpectedValues->Num());
    ResultObj->SetArrayField(TEXT("found"), Found);
    ResultObj->SetArrayField(TEXT("missing"), Missing);
    ResultObj->SetArrayField(TEXT("mismatched"), Mismatched);
    return ResultObj;
}
