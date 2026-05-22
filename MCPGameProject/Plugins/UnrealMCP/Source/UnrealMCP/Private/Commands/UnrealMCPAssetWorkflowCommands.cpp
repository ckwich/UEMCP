#include "Commands/UnrealMCPAssetWorkflowCommands.h"
#include "Commands/UnrealMCPCommonUtils.h"

#include "AssetRegistry/ARFilter.h"
#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/AssetDataTagMap.h"
#include "AssetRegistry/AssetIdentifier.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "AssetImportTask.h"
#include "AssetToolsModule.h"
#include "Components/StaticMeshComponent.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Engine/Blueprint.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "Engine/Level.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Factories/BlueprintFactory.h"
#include "FileHelpers.h"
#include "GameFramework/Actor.h"
#include "GameFramework/Pawn.h"
#include "IAssetTools.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "PhysicsEngine/AggregateGeom.h"
#include "PhysicsEngine/BodySetup.h"
#include "ScopedTransaction.h"
#include "Subsystems/EditorActorSubsystem.h"
#include "Subsystems/EditorAssetSubsystem.h"
#include "UObject/ObjectRedirector.h"
#include "UObject/SoftObjectPath.h"

namespace
{
    constexpr int32 MaxAssetIntakeSnapshotLimit = 10000;

    FString TrimTrailingSlashes(FString Value)
    {
        while (Value.Len() > 1 && Value.EndsWith(TEXT("/")))
        {
            Value.LeftChopInline(1);
        }
        return Value;
    }

    FString NormalizeAssetRoot(const FString& RawRoot)
    {
        FString Root = RawRoot.TrimStartAndEnd();
        Root.ReplaceInline(TEXT("\\"), TEXT("/"));
        Root = TrimTrailingSlashes(Root);

        if (Root.IsEmpty())
        {
            return FString();
        }
        if (Root.Equals(TEXT("Content"), ESearchCase::IgnoreCase) ||
            Root.Equals(TEXT("/Content"), ESearchCase::IgnoreCase))
        {
            return TEXT("/Game");
        }
        if (Root.StartsWith(TEXT("/Game"), ESearchCase::IgnoreCase) ||
            Root.StartsWith(TEXT("/Engine"), ESearchCase::IgnoreCase) ||
            Root.StartsWith(TEXT("/Plugin"), ESearchCase::IgnoreCase))
        {
            return Root;
        }
        if (Root.StartsWith(TEXT("Content/"), ESearchCase::IgnoreCase))
        {
            return TEXT("/Game/") + Root.RightChop(8);
        }
        if (Root.StartsWith(TEXT("/Content/"), ESearchCase::IgnoreCase))
        {
            return TEXT("/Game/") + Root.RightChop(9);
        }

        const FString ContentToken = TEXT("/Content/");
        const int32 AbsoluteContentIndex = Root.Find(
            ContentToken,
            ESearchCase::IgnoreCase,
            ESearchDir::FromEnd
        );
        if (AbsoluteContentIndex != INDEX_NONE)
        {
            return TEXT("/Game/") + Root.Mid(AbsoluteContentIndex + ContentToken.Len());
        }

        if (Root.StartsWith(TEXT("/"), ESearchCase::IgnoreCase))
        {
            return Root;
        }

        return TEXT("/Game/") + Root;
    }

    FString AssetClassShortName(const FString& AssetClassPath)
    {
        FString ShortName = AssetClassPath;

        int32 DotIndex = INDEX_NONE;
        if (ShortName.FindLastChar(TEXT('.'), DotIndex))
        {
            ShortName = ShortName.Mid(DotIndex + 1);
        }

        int32 SlashIndex = INDEX_NONE;
        if (ShortName.FindLastChar(TEXT('/'), SlashIndex))
        {
            ShortName = ShortName.Mid(SlashIndex + 1);
        }

        return ShortName;
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

    FString PackageParentPath(const FString& PackagePath)
    {
        int32 SlashIndex = INDEX_NONE;
        if (PackagePath.FindLastChar(TEXT('/'), SlashIndex))
        {
            return PackagePath.Left(SlashIndex);
        }
        return FString();
    }

    bool IsExactGamePackagePath(const FString& Path)
    {
        return Path.StartsWith(TEXT("/Game/")) &&
            !Path.Contains(TEXT("*")) &&
            !Path.Contains(TEXT("?"));
    }

    bool IsGameRootPath(const FString& Path)
    {
        return (Path == TEXT("/Game") || Path.StartsWith(TEXT("/Game/"))) &&
            !Path.Contains(TEXT("*")) &&
            !Path.Contains(TEXT("?"));
    }

    FString NormalizePackagePath(FString Path)
    {
        Path = Path.TrimStartAndEnd();
        Path.ReplaceInline(TEXT("\\"), TEXT("/"));
        return TrimTrailingSlashes(Path);
    }

    FString JoinPackagePath(const FString& ParentPath, const FString& LeafName)
    {
        const FString CleanParent = NormalizePackagePath(ParentPath);
        return CleanParent / LeafName;
    }

    FString ObjectPathFromPackagePath(const FString& PackagePath)
    {
        const FString LeafName = PackageLeafName(PackagePath);
        return FString::Printf(TEXT("%s.%s"), *PackagePath, *LeafName);
    }

    bool AssetExists(IAssetRegistry& AssetRegistry, const FString& PackagePath)
    {
        return AssetRegistry.GetAssetByObjectPath(FSoftObjectPath(ObjectPathFromPackagePath(PackagePath))).IsValid();
    }

    TSharedPtr<FJsonObject> ErrorResponse(const FString& Message)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(Message);
    }

    UEditorAssetSubsystem* GetEditorAssetSubsystem()
    {
        return GEditor ? GEditor->GetEditorSubsystem<UEditorAssetSubsystem>() : nullptr;
    }

    UEditorActorSubsystem* GetEditorActorSubsystem()
    {
        return GEditor ? GEditor->GetEditorSubsystem<UEditorActorSubsystem>() : nullptr;
    }

    UObject* LoadAssetObject(const FString& AssetPath)
    {
        if (UEditorAssetSubsystem* AssetSubsystem = GetEditorAssetSubsystem())
        {
            if (UObject* Asset = AssetSubsystem->LoadAsset(AssetPath))
            {
                return Asset;
            }
        }
        return StaticLoadObject(UObject::StaticClass(), nullptr, *AssetPath);
    }

    TArray<TSharedPtr<FJsonValue>> StringsToJsonArray(const TArray<FString>& Values);

    TArray<TSharedPtr<FJsonValue>> VectorToJsonArray(const FVector& Vector)
    {
        TArray<TSharedPtr<FJsonValue>> Values;
        Values.Add(MakeShared<FJsonValueNumber>(Vector.X));
        Values.Add(MakeShared<FJsonValueNumber>(Vector.Y));
        Values.Add(MakeShared<FJsonValueNumber>(Vector.Z));
        return Values;
    }

    FVector JsonArrayToVector(
        const TSharedPtr<FJsonObject>& JsonObject,
        const TCHAR* FieldName,
        const FVector& DefaultValue
    )
    {
        const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
        if (!JsonObject.IsValid() || !JsonObject->TryGetArrayField(FieldName, Values) || !Values || Values->Num() < 3)
        {
            return DefaultValue;
        }
        return FVector(
            (*Values)[0]->AsNumber(),
            (*Values)[1]->AsNumber(),
            (*Values)[2]->AsNumber()
        );
    }

    FRotator JsonArrayToRotator(
        const TSharedPtr<FJsonObject>& JsonObject,
        const TCHAR* FieldName,
        const FRotator& DefaultValue
    )
    {
        const FVector Vector = JsonArrayToVector(
            JsonObject,
            FieldName,
            FVector(DefaultValue.Pitch, DefaultValue.Yaw, DefaultValue.Roll)
        );
        return FRotator(Vector.X, Vector.Y, Vector.Z);
    }

    TSharedPtr<FJsonObject> BoolResult(
        const FString& Operation,
        bool bSuccess,
        bool bDryRun,
        const TArray<FString>& ChangedPackages,
        const TArray<FString>& Warnings = {}
    )
    {
        TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
        ResultObj->SetStringField(TEXT("operation"), Operation);
        ResultObj->SetBoolField(TEXT("success"), bSuccess);
        ResultObj->SetBoolField(TEXT("dry_run"), bDryRun);
        ResultObj->SetArrayField(TEXT("changed_packages"), StringsToJsonArray(ChangedPackages));
        ResultObj->SetArrayField(TEXT("warnings"), StringsToJsonArray(Warnings));
        return ResultObj;
    }

    bool AssetClassMatches(
        const TArray<FString>& ClassFilters,
        const FString& AssetClass,
        const FString& AssetClassPath
    )
    {
        if (ClassFilters.IsEmpty())
        {
            return true;
        }

        for (const FString& ClassFilter : ClassFilters)
        {
            if (AssetClass.Equals(ClassFilter, ESearchCase::IgnoreCase) ||
                AssetClassPath.Equals(ClassFilter, ESearchCase::IgnoreCase) ||
                AssetClassPath.Contains(ClassFilter, ESearchCase::IgnoreCase))
            {
                return true;
            }
        }
        return false;
    }

    TArray<FString> GetStringArrayField(
        const TSharedPtr<FJsonObject>& Params,
        const TCHAR* FieldName
    )
    {
        TArray<FString> Values;
        const TArray<TSharedPtr<FJsonValue>>* JsonArray = nullptr;
        if (!Params.IsValid() || !Params->TryGetArrayField(FieldName, JsonArray) || !JsonArray)
        {
            return Values;
        }

        for (const TSharedPtr<FJsonValue>& JsonValue : *JsonArray)
        {
            if (!JsonValue.IsValid() || JsonValue->Type != EJson::String)
            {
                continue;
            }
            const FString Text = JsonValue->AsString().TrimStartAndEnd();
            if (!Text.IsEmpty())
            {
                Values.Add(Text);
            }
        }
        return Values;
    }

    TArray<TSharedPtr<FJsonValue>> ObjectArrayToJsonArray(const TArray<TSharedPtr<FJsonObject>>& Objects)
    {
        TArray<TSharedPtr<FJsonValue>> JsonValues;
        JsonValues.Reserve(Objects.Num());
        for (const TSharedPtr<FJsonObject>& Object : Objects)
        {
            JsonValues.Add(MakeShared<FJsonValueObject>(Object));
        }
        return JsonValues;
    }

    TArray<TSharedPtr<FJsonObject>> GetObjectArrayField(
        const TSharedPtr<FJsonObject>& Params,
        const TCHAR* FieldName
    )
    {
        TArray<TSharedPtr<FJsonObject>> Values;
        const TArray<TSharedPtr<FJsonValue>>* JsonArray = nullptr;
        if (!Params.IsValid() || !Params->TryGetArrayField(FieldName, JsonArray) || !JsonArray)
        {
            return Values;
        }

        for (const TSharedPtr<FJsonValue>& JsonValue : *JsonArray)
        {
            if (!JsonValue.IsValid() || JsonValue->Type != EJson::Object)
            {
                continue;
            }
            Values.Add(JsonValue->AsObject());
        }
        return Values;
    }

    TArray<TSharedPtr<FJsonValue>> StringsToJsonArray(const TArray<FString>& Values)
    {
        TArray<TSharedPtr<FJsonValue>> JsonValues;
        JsonValues.Reserve(Values.Num());
        for (const FString& Value : Values)
        {
            JsonValues.Add(MakeShared<FJsonValueString>(Value));
        }
        return JsonValues;
    }

    TArray<TSharedPtr<FJsonValue>> AssetRelationshipsToJsonArray(
        IAssetRegistry& AssetRegistry,
        FName PackageName,
        bool bReferencers
    )
    {
        TArray<FAssetDependency> Relationships;
        UE::AssetRegistry::FDependencyQuery Query;
        const FAssetIdentifier AssetIdentifier(PackageName);
        if (bReferencers)
        {
            AssetRegistry.GetReferencers(
                AssetIdentifier,
                Relationships,
                UE::AssetRegistry::EDependencyCategory::Package,
                Query
            );
        }
        else
        {
            AssetRegistry.GetDependencies(
                AssetIdentifier,
                Relationships,
                UE::AssetRegistry::EDependencyCategory::Package,
                Query
            );
        }

        Relationships.Sort([](const FAssetDependency& Left, const FAssetDependency& Right)
        {
            return Left.LexicalLess(Right);
        });

        TArray<TSharedPtr<FJsonValue>> RelationshipValues;
        RelationshipValues.Reserve(Relationships.Num());
        TSet<FString> SeenIdentifiers;
        for (const FAssetDependency& Relationship : Relationships)
        {
            const FAssetIdentifier& RelationshipId = Relationship.AssetId;
            const FString Identifier = RelationshipId.PackageName != NAME_None
                ? RelationshipId.PackageName.ToString()
                : RelationshipId.ToString();
            if (Identifier.IsEmpty() || SeenIdentifiers.Contains(Identifier))
            {
                continue;
            }
            SeenIdentifiers.Add(Identifier);
            RelationshipValues.Add(MakeShared<FJsonValueString>(Identifier));
        }
        return RelationshipValues;
    }

    TSharedPtr<FJsonObject> TagsToJsonObject(const FAssetData& AssetData)
    {
        TSharedPtr<FJsonObject> TagsObj = MakeShared<FJsonObject>();
        AssetData.EnumerateTags([TagsObj](TPair<FName, FAssetTagValueRef> TagAndValue)
        {
            TagsObj->SetStringField(TagAndValue.Key.ToString(), TagAndValue.Value.AsString());
        });
        return TagsObj;
    }

    TSharedPtr<FJsonObject> AssetDataToJson(
        const FAssetData& AssetData,
        IAssetRegistry& AssetRegistry,
        bool bIncludeDependencies,
        bool bIncludeReferencers,
        bool bIncludeTags
    )
    {
        const FString AssetClassPath = AssetData.AssetClassPath.ToString();
        const FString AssetClass = AssetClassShortName(AssetClassPath);

        TSharedPtr<FJsonObject> AssetObj = MakeShared<FJsonObject>();
        AssetObj->SetStringField(TEXT("asset_name"), AssetData.AssetName.ToString());
        AssetObj->SetStringField(TEXT("object_path"), AssetData.GetObjectPathString());
        AssetObj->SetStringField(TEXT("package_name"), AssetData.PackageName.ToString());
        AssetObj->SetStringField(TEXT("package_path"), AssetData.PackagePath.ToString());
        AssetObj->SetStringField(TEXT("asset_class"), AssetClass);
        AssetObj->SetStringField(TEXT("asset_class_path"), AssetClassPath);

        AssetObj->SetArrayField(
            TEXT("dependencies"),
            bIncludeDependencies
                ? AssetRelationshipsToJsonArray(AssetRegistry, AssetData.PackageName, false)
                : TArray<TSharedPtr<FJsonValue>>()
        );
        AssetObj->SetArrayField(
            TEXT("referencers"),
            bIncludeReferencers
                ? AssetRelationshipsToJsonArray(AssetRegistry, AssetData.PackageName, true)
                : TArray<TSharedPtr<FJsonValue>>()
        );
        AssetObj->SetObjectField(
            TEXT("tags"),
            bIncludeTags ? TagsToJsonObject(AssetData) : MakeShared<FJsonObject>()
        );
        return AssetObj;
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

    FString CurrentMapFromWorld(UWorld* World)
    {
        if (!World)
        {
            return FString();
        }
        return World->GetOutermost() ? World->GetOutermost()->GetName() : World->GetMapName();
    }

    bool TargetMapMatches(UWorld* World, const FString& TargetMap)
    {
        return TargetMap.IsEmpty() || CurrentMapFromWorld(World).Equals(TargetMap, ESearchCase::IgnoreCase);
    }

    AActor* FindActorByName(UWorld* World, const FString& ActorName)
    {
        if (!World || ActorName.IsEmpty())
        {
            return nullptr;
        }

        for (TActorIterator<AActor> ActorIt(World); ActorIt; ++ActorIt)
        {
            AActor* Actor = *ActorIt;
            if (Actor && (Actor->GetName() == ActorName || Actor->GetActorLabel() == ActorName))
            {
                return Actor;
            }
        }

        return nullptr;
    }

    void MarkActorLevelDirty(AActor* Actor)
    {
        if (!Actor)
        {
            return;
        }
        if (ULevel* Level = Actor->GetLevel())
        {
            Level->Modify();
            if (UPackage* Package = Level->GetOutermost())
            {
                Package->SetDirtyFlag(true);
            }
        }
    }

    UClass* ResolveActorParentClass(const FString& ParentClass)
    {
        if (ParentClass.IsEmpty() ||
            ParentClass.Equals(TEXT("Actor"), ESearchCase::IgnoreCase) ||
            ParentClass.Equals(TEXT("AActor"), ESearchCase::IgnoreCase))
        {
            return AActor::StaticClass();
        }
        if (ParentClass.Equals(TEXT("Pawn"), ESearchCase::IgnoreCase) ||
            ParentClass.Equals(TEXT("APawn"), ESearchCase::IgnoreCase))
        {
            return APawn::StaticClass();
        }
        if (UClass* LoadedClass = LoadClass<AActor>(nullptr, *ParentClass))
        {
            return LoadedClass;
        }

        FString EngineClassName = ParentClass;
        if (!EngineClassName.StartsWith(TEXT("A")))
        {
            EngineClassName = TEXT("A") + EngineClassName;
        }
        return LoadClass<AActor>(
            nullptr,
            *FString::Printf(TEXT("/Script/Engine.%s"), *EngineClassName)
        );
    }

    TSharedPtr<FJsonObject> PlacementPlanToJson(
        const FString& AssetPath,
        const FString& ActorName,
        const FVector& Location,
        const FRotator& Rotation,
        const FVector& Scale
    )
    {
        TSharedPtr<FJsonObject> PlacementObj = MakeShared<FJsonObject>();
        PlacementObj->SetStringField(TEXT("asset_path"), AssetPath);
        PlacementObj->SetStringField(TEXT("actor_name"), ActorName);
        PlacementObj->SetArrayField(TEXT("location"), VectorToJsonArray(Location));
        PlacementObj->SetArrayField(TEXT("rotation"), VectorToJsonArray(FVector(Rotation.Pitch, Rotation.Yaw, Rotation.Roll)));
        PlacementObj->SetArrayField(TEXT("scale"), VectorToJsonArray(Scale));
        return PlacementObj;
    }
}

FUnrealMCPAssetWorkflowCommands::FUnrealMCPAssetWorkflowCommands()
{
}

TSharedPtr<FJsonObject> FUnrealMCPAssetWorkflowCommands::HandleCommand(
    const FString& CommandType,
    const TSharedPtr<FJsonObject>& Params
)
{
    if (CommandType == TEXT("asset_intake_snapshot"))
    {
        return HandleAssetIntakeSnapshot(Params);
    }
    if (CommandType == TEXT("asset_import_from_disk"))
    {
        return HandleAssetImportFromDisk(Params);
    }
    if (CommandType == TEXT("asset_rename"))
    {
        return HandleAssetRename(Params);
    }
    if (CommandType == TEXT("asset_move"))
    {
        return HandleAssetMove(Params);
    }
    if (CommandType == TEXT("asset_duplicate"))
    {
        return HandleAssetDuplicate(Params);
    }
    if (CommandType == TEXT("asset_delete"))
    {
        return HandleAssetDelete(Params);
    }
    if (CommandType == TEXT("asset_save_packages"))
    {
        return HandleAssetSavePackages(Params);
    }
    if (CommandType == TEXT("asset_fixup_redirectors"))
    {
        return HandleAssetFixupRedirectors(Params);
    }
    if (CommandType == TEXT("asset_prepare_for_level"))
    {
        return HandleAssetPrepareForLevel(Params);
    }
    if (CommandType == TEXT("asset_create_blueprint_wrapper"))
    {
        return HandleAssetCreateBlueprintWrapper(Params);
    }
    if (CommandType == TEXT("asset_place_in_level"))
    {
        return HandleAssetPlaceInLevel(Params);
    }
    if (CommandType == TEXT("asset_validate_level_placements"))
    {
        return HandleAssetValidateLevelPlacements(Params);
    }

    return FUnrealMCPCommonUtils::CreateErrorResponse(
        FString::Printf(TEXT("Unknown asset workflow command: %s"), *CommandType)
    );
}

TSharedPtr<FJsonObject> FUnrealMCPAssetWorkflowCommands::HandleAssetIntakeSnapshot(
    const TSharedPtr<FJsonObject>& Params
)
{
    const TArray<FString> RawRoots = GetStringArrayField(Params, TEXT("roots"));
    TArray<FString> PackageRoots;
    TSet<FString> SeenRoots;
    for (const FString& RawRoot : RawRoots)
    {
        const FString PackageRoot = NormalizeAssetRoot(RawRoot);
        if (!PackageRoot.IsEmpty() && !SeenRoots.Contains(PackageRoot))
        {
            SeenRoots.Add(PackageRoot);
            PackageRoots.Add(PackageRoot);
        }
    }

    if (PackageRoots.IsEmpty())
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(
            TEXT("asset_intake_snapshot requires at least one non-empty root")
        );
    }

    const TArray<FString> ClassFilters = GetStringArrayField(Params, TEXT("classes"));

    bool bIncludeDependencies = true;
    if (Params->HasField(TEXT("include_dependencies")))
    {
        Params->TryGetBoolField(TEXT("include_dependencies"), bIncludeDependencies);
    }
    bool bIncludeReferencers = false;
    if (Params->HasField(TEXT("include_referencers")))
    {
        Params->TryGetBoolField(TEXT("include_referencers"), bIncludeReferencers);
    }
    bool bIncludeTags = true;
    if (Params->HasField(TEXT("include_tags")))
    {
        Params->TryGetBoolField(TEXT("include_tags"), bIncludeTags);
    }

    double RequestedLimit = 5000.0;
    if (Params->HasField(TEXT("limit")))
    {
        Params->TryGetNumberField(TEXT("limit"), RequestedLimit);
    }
    const int32 Limit = FMath::Clamp(
        static_cast<int32>(RequestedLimit),
        1,
        MaxAssetIntakeSnapshotLimit
    );

    FARFilter Filter;
    Filter.bRecursivePaths = true;
    Filter.bIncludeOnlyOnDiskAssets = false;
    for (const FString& PackageRoot : PackageRoots)
    {
        Filter.PackagePaths.Add(*PackageRoot);
    }

    IAssetRegistry& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(
        TEXT("AssetRegistry")
    ).Get();

    TArray<FAssetData> AssetDataList;
    AssetRegistry.GetAssets(Filter, AssetDataList);
    AssetDataList.Sort([](const FAssetData& Left, const FAssetData& Right)
    {
        return Left.GetObjectPathString() < Right.GetObjectPathString();
    });

    TArray<TSharedPtr<FJsonValue>> Assets;
    Assets.Reserve(FMath::Min(Limit, AssetDataList.Num()));
    TSet<FString> SeenObjectPaths;
    int32 MatchedAssetCount = 0;
    for (const FAssetData& AssetData : AssetDataList)
    {
        const FString ObjectPath = AssetData.GetObjectPathString();
        if (SeenObjectPaths.Contains(ObjectPath))
        {
            continue;
        }
        SeenObjectPaths.Add(ObjectPath);

        const FString AssetClassPath = AssetData.AssetClassPath.ToString();
        const FString AssetClass = AssetClassShortName(AssetClassPath);
        if (!AssetClassMatches(ClassFilters, AssetClass, AssetClassPath))
        {
            continue;
        }

        ++MatchedAssetCount;
        if (Assets.Num() < Limit)
        {
            Assets.Add(MakeShared<FJsonValueObject>(AssetDataToJson(
                AssetData,
                AssetRegistry,
                bIncludeDependencies,
                bIncludeReferencers,
                bIncludeTags
            )));
        }
    }

    TArray<TSharedPtr<FJsonValue>> Warnings;
    if (AssetRegistry.IsLoadingAssets())
    {
        Warnings.Add(MakeShared<FJsonValueString>(
            TEXT("Asset Registry is still loading assets; intake snapshot may be incomplete")
        ));
    }

    TSharedPtr<FJsonObject> FiltersObj = MakeShared<FJsonObject>();
    FiltersObj->SetArrayField(TEXT("roots"), StringsToJsonArray(PackageRoots));
    FiltersObj->SetArrayField(TEXT("classes"), StringsToJsonArray(ClassFilters));
    FiltersObj->SetBoolField(TEXT("include_dependencies"), bIncludeDependencies);
    FiltersObj->SetBoolField(TEXT("include_referencers"), bIncludeReferencers);
    FiltersObj->SetBoolField(TEXT("include_tags"), bIncludeTags);
    FiltersObj->SetNumberField(TEXT("limit"), Limit);

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(
        TEXT("snapshot_id"),
        FString::Printf(
            TEXT("asset-snapshot-%s"),
            *FGuid::NewGuid().ToString(EGuidFormats::Digits)
        )
    );
    ResultObj->SetArrayField(TEXT("roots"), StringsToJsonArray(PackageRoots));
    ResultObj->SetObjectField(TEXT("filters"), FiltersObj);
    ResultObj->SetNumberField(TEXT("asset_count"), Assets.Num());
    ResultObj->SetNumberField(TEXT("matched_asset_count"), MatchedAssetCount);
    ResultObj->SetNumberField(TEXT("total_asset_count"), AssetDataList.Num());
    ResultObj->SetBoolField(TEXT("truncated"), MatchedAssetCount > Assets.Num());
    ResultObj->SetBoolField(TEXT("asset_registry_loading"), AssetRegistry.IsLoadingAssets());
    ResultObj->SetArrayField(TEXT("assets"), Assets);
    ResultObj->SetArrayField(TEXT("warnings"), Warnings);
    return ResultObj;
}

TSharedPtr<FJsonObject> FUnrealMCPAssetWorkflowCommands::HandleAssetImportFromDisk(
    const TSharedPtr<FJsonObject>& Params
)
{
    TArray<FString> SourceFiles = GetStringArrayField(Params, TEXT("source_files"));
    if (SourceFiles.IsEmpty())
    {
        return ErrorResponse(TEXT("asset_import_from_disk requires at least one source file"));
    }

    FString DestinationPath;
    if (!Params->TryGetStringField(TEXT("destination_path"), DestinationPath))
    {
        return ErrorResponse(TEXT("Missing destination_path"));
    }
    DestinationPath = NormalizePackagePath(DestinationPath);
    if (!IsGameRootPath(DestinationPath))
    {
        return ErrorResponse(TEXT("destination_path must be an exact /Game package path"));
    }

    bool bReplaceExisting = false;
    Params->TryGetBoolField(TEXT("replace_existing"), bReplaceExisting);
    bool bSaveImportedAssets = false;
    Params->TryGetBoolField(TEXT("save_imported_assets"), bSaveImportedAssets);
    bool bDryRun = true;
    Params->TryGetBoolField(TEXT("dry_run"), bDryRun);

    IAssetRegistry& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(
        TEXT("AssetRegistry")
    ).Get();

    TArray<FString> NormalizedSources;
    TArray<FString> PlannedPackages;
    TArray<FString> MissingFiles;
    TArray<FString> Conflicts;
    TArray<TSharedPtr<FJsonObject>> PlannedImports;

    for (const FString& SourceFile : SourceFiles)
    {
        FString NormalizedSource = FPaths::ConvertRelativePathToFull(SourceFile);
        FPaths::NormalizeFilename(NormalizedSource);
        const FString AssetName = FPaths::GetBaseFilename(NormalizedSource);
        const FString PlannedPackage = JoinPackagePath(DestinationPath, AssetName);

        const bool bMissing = !FPaths::FileExists(NormalizedSource);
        const bool bConflict = AssetExists(AssetRegistry, PlannedPackage);

        NormalizedSources.Add(NormalizedSource);
        PlannedPackages.Add(PlannedPackage);
        if (bMissing)
        {
            MissingFiles.Add(NormalizedSource);
        }
        if (bConflict)
        {
            Conflicts.Add(PlannedPackage);
        }

        TSharedPtr<FJsonObject> PlannedImport = MakeShared<FJsonObject>();
        PlannedImport->SetStringField(TEXT("source_file"), NormalizedSource);
        PlannedImport->SetStringField(TEXT("destination_path"), DestinationPath);
        PlannedImport->SetStringField(TEXT("planned_package"), PlannedPackage);
        PlannedImport->SetStringField(TEXT("planned_object_path"), ObjectPathFromPackagePath(PlannedPackage));
        PlannedImport->SetBoolField(TEXT("missing"), bMissing);
        PlannedImport->SetBoolField(TEXT("conflict"), bConflict);
        PlannedImports.Add(PlannedImport);
    }

    if (bDryRun)
    {
        TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
        ResultObj->SetBoolField(TEXT("dry_run"), true);
        ResultObj->SetArrayField(TEXT("source_files"), StringsToJsonArray(NormalizedSources));
        ResultObj->SetStringField(TEXT("destination_path"), DestinationPath);
        ResultObj->SetArrayField(TEXT("planned_packages"), StringsToJsonArray(PlannedPackages));
        ResultObj->SetArrayField(TEXT("missing_files"), StringsToJsonArray(MissingFiles));
        ResultObj->SetArrayField(TEXT("conflicts"), StringsToJsonArray(Conflicts));
        ResultObj->SetArrayField(TEXT("planned_imports"), ObjectArrayToJsonArray(PlannedImports));
        ResultObj->SetBoolField(TEXT("replace_existing"), bReplaceExisting);
        ResultObj->SetBoolField(TEXT("save_imported_assets"), bSaveImportedAssets);
        return ResultObj;
    }

    if (!MissingFiles.IsEmpty())
    {
        return ErrorResponse(FString::Printf(
            TEXT("Cannot import because one or more source files are missing: %s"),
            *FString::Join(MissingFiles, TEXT(", "))
        ));
    }
    if (!bReplaceExisting && !Conflicts.IsEmpty())
    {
        return ErrorResponse(FString::Printf(
            TEXT("Cannot import because destination assets already exist and replace_existing is false: %s"),
            *FString::Join(Conflicts, TEXT(", "))
        ));
    }

    TArray<UAssetImportTask*> ImportTasks;
    for (const FString& SourceFile : NormalizedSources)
    {
        UAssetImportTask* ImportTask = NewObject<UAssetImportTask>();
        ImportTask->Filename = SourceFile;
        ImportTask->DestinationPath = DestinationPath;
        ImportTask->DestinationName = FPaths::GetBaseFilename(SourceFile);
        ImportTask->bReplaceExisting = bReplaceExisting;
        ImportTask->bReplaceExistingSettings = bReplaceExisting;
        ImportTask->bAutomated = true;
        ImportTask->bSave = bSaveImportedAssets;
        ImportTask->bAsync = false;
        ImportTasks.Add(ImportTask);
    }

    FAssetToolsModule& AssetToolsModule = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools"));
    AssetToolsModule.Get().ImportAssetTasks(ImportTasks);

    TArray<FString> ImportedAssets;
    TArray<FString> DirtyPackages;
    TArray<FString> FailedFiles;
    for (UAssetImportTask* ImportTask : ImportTasks)
    {
        bool bImportedAny = false;
        for (UObject* ImportedObject : ImportTask->GetObjects())
        {
            if (!ImportedObject)
            {
                continue;
            }
            bImportedAny = true;
            ImportedAssets.Add(ImportedObject->GetPathName());
            if (UPackage* Package = ImportedObject->GetOutermost())
            {
                if (Package->IsDirty())
                {
                    DirtyPackages.AddUnique(Package->GetName());
                }
            }
        }
        for (const FString& ImportedObjectPath : ImportTask->ImportedObjectPaths)
        {
            if (!ImportedObjectPath.IsEmpty())
            {
                ImportedAssets.AddUnique(ImportedObjectPath);
                bImportedAny = true;
            }
        }
        if (!bImportedAny)
        {
            FailedFiles.Add(ImportTask->Filename);
        }
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetBoolField(TEXT("dry_run"), false);
    ResultObj->SetStringField(TEXT("destination_path"), DestinationPath);
    ResultObj->SetArrayField(TEXT("source_files"), StringsToJsonArray(NormalizedSources));
    ResultObj->SetArrayField(TEXT("planned_packages"), StringsToJsonArray(PlannedPackages));
    ResultObj->SetArrayField(TEXT("imported_assets"), StringsToJsonArray(ImportedAssets));
    ResultObj->SetArrayField(TEXT("failed_files"), StringsToJsonArray(FailedFiles));
    ResultObj->SetArrayField(TEXT("dirty_packages"), StringsToJsonArray(DirtyPackages));
    ResultObj->SetBoolField(TEXT("save_requested"), bSaveImportedAssets);
    ResultObj->SetBoolField(TEXT("replace_existing"), bReplaceExisting);
    return ResultObj;
}

TSharedPtr<FJsonObject> FUnrealMCPAssetWorkflowCommands::HandleAssetRename(
    const TSharedPtr<FJsonObject>& Params
)
{
    FString AssetPath;
    FString NewName;
    if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) ||
        !Params->TryGetStringField(TEXT("new_name"), NewName))
    {
        return ErrorResponse(TEXT("asset_rename requires asset_path and new_name"));
    }
    AssetPath = NormalizePackagePath(AssetPath);
    NewName = NewName.TrimStartAndEnd();
    if (!IsExactGamePackagePath(AssetPath) || NewName.IsEmpty() || NewName.Contains(TEXT("/")) || NewName.Contains(TEXT("\\")))
    {
        return ErrorResponse(TEXT("asset_rename requires an exact /Game asset_path and a leaf new_name"));
    }

    bool bDryRun = true;
    Params->TryGetBoolField(TEXT("dry_run"), bDryRun);
    const FString DestinationPath = JoinPackagePath(PackageParentPath(AssetPath), NewName);
    TArray<FString> ChangedPackages = {AssetPath, DestinationPath};

    if (bDryRun)
    {
        TSharedPtr<FJsonObject> ResultObj = BoolResult(TEXT("rename"), true, true, ChangedPackages);
        ResultObj->SetStringField(TEXT("source_path"), AssetPath);
        ResultObj->SetStringField(TEXT("destination_path"), DestinationPath);
        return ResultObj;
    }

    UEditorAssetSubsystem* AssetSubsystem = GetEditorAssetSubsystem();
    if (!AssetSubsystem)
    {
        return ErrorResponse(TEXT("Editor asset subsystem is not available"));
    }

    IAssetRegistry& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
    if (AssetExists(AssetRegistry, DestinationPath))
    {
        return ErrorResponse(FString::Printf(TEXT("Destination asset already exists: %s"), *DestinationPath));
    }

    const bool bRenamed = AssetSubsystem->RenameAsset(AssetPath, DestinationPath);
    if (!bRenamed)
    {
        return ErrorResponse(FString::Printf(TEXT("Failed to rename asset: %s"), *AssetPath));
    }
    TSharedPtr<FJsonObject> ResultObj = BoolResult(TEXT("rename"), true, false, ChangedPackages);
    ResultObj->SetStringField(TEXT("source_path"), AssetPath);
    ResultObj->SetStringField(TEXT("destination_path"), DestinationPath);
    return ResultObj;
}

TSharedPtr<FJsonObject> FUnrealMCPAssetWorkflowCommands::HandleAssetMove(
    const TSharedPtr<FJsonObject>& Params
)
{
    FString AssetPath;
    FString DestinationPath;
    if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) ||
        !Params->TryGetStringField(TEXT("destination_path"), DestinationPath))
    {
        return ErrorResponse(TEXT("asset_move requires asset_path and destination_path"));
    }
    AssetPath = NormalizePackagePath(AssetPath);
    DestinationPath = NormalizePackagePath(DestinationPath);
    if (!IsExactGamePackagePath(AssetPath) || !IsExactGamePackagePath(DestinationPath))
    {
        return ErrorResponse(TEXT("asset_move requires exact /Game package paths"));
    }

    bool bDryRun = true;
    Params->TryGetBoolField(TEXT("dry_run"), bDryRun);
    TArray<FString> ChangedPackages = {AssetPath, DestinationPath};
    if (bDryRun)
    {
        TSharedPtr<FJsonObject> ResultObj = BoolResult(TEXT("move"), true, true, ChangedPackages);
        ResultObj->SetStringField(TEXT("source_path"), AssetPath);
        ResultObj->SetStringField(TEXT("destination_path"), DestinationPath);
        return ResultObj;
    }

    UEditorAssetSubsystem* AssetSubsystem = GetEditorAssetSubsystem();
    if (!AssetSubsystem)
    {
        return ErrorResponse(TEXT("Editor asset subsystem is not available"));
    }
    IAssetRegistry& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
    if (AssetExists(AssetRegistry, DestinationPath))
    {
        return ErrorResponse(FString::Printf(TEXT("Destination asset already exists: %s"), *DestinationPath));
    }

    const bool bMoved = AssetSubsystem->RenameAsset(AssetPath, DestinationPath);
    if (!bMoved)
    {
        return ErrorResponse(FString::Printf(TEXT("Failed to move asset: %s"), *AssetPath));
    }
    TSharedPtr<FJsonObject> ResultObj = BoolResult(TEXT("move"), true, false, ChangedPackages);
    ResultObj->SetStringField(TEXT("source_path"), AssetPath);
    ResultObj->SetStringField(TEXT("destination_path"), DestinationPath);
    return ResultObj;
}

TSharedPtr<FJsonObject> FUnrealMCPAssetWorkflowCommands::HandleAssetDuplicate(
    const TSharedPtr<FJsonObject>& Params
)
{
    FString AssetPath;
    FString DestinationPath;
    if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) ||
        !Params->TryGetStringField(TEXT("destination_path"), DestinationPath))
    {
        return ErrorResponse(TEXT("asset_duplicate requires asset_path and destination_path"));
    }
    AssetPath = NormalizePackagePath(AssetPath);
    DestinationPath = NormalizePackagePath(DestinationPath);
    if (!IsExactGamePackagePath(AssetPath) || !IsExactGamePackagePath(DestinationPath))
    {
        return ErrorResponse(TEXT("asset_duplicate requires exact /Game package paths"));
    }

    bool bDryRun = true;
    Params->TryGetBoolField(TEXT("dry_run"), bDryRun);
    TArray<FString> ChangedPackages = {DestinationPath};
    if (bDryRun)
    {
        TSharedPtr<FJsonObject> ResultObj = BoolResult(TEXT("duplicate"), true, true, ChangedPackages);
        ResultObj->SetStringField(TEXT("source_path"), AssetPath);
        ResultObj->SetStringField(TEXT("destination_path"), DestinationPath);
        return ResultObj;
    }

    UEditorAssetSubsystem* AssetSubsystem = GetEditorAssetSubsystem();
    if (!AssetSubsystem)
    {
        return ErrorResponse(TEXT("Editor asset subsystem is not available"));
    }
    IAssetRegistry& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
    if (AssetExists(AssetRegistry, DestinationPath))
    {
        return ErrorResponse(FString::Printf(TEXT("Destination asset already exists: %s"), *DestinationPath));
    }

    UObject* DuplicatedAsset = AssetSubsystem->DuplicateAsset(AssetPath, DestinationPath);
    if (!DuplicatedAsset)
    {
        return ErrorResponse(FString::Printf(TEXT("Failed to duplicate asset: %s"), *AssetPath));
    }
    TSharedPtr<FJsonObject> ResultObj = BoolResult(TEXT("duplicate"), true, false, ChangedPackages);
    ResultObj->SetStringField(TEXT("source_path"), AssetPath);
    ResultObj->SetStringField(TEXT("destination_path"), DestinationPath);
    ResultObj->SetStringField(TEXT("duplicated_object_path"), DuplicatedAsset->GetPathName());
    return ResultObj;
}

TSharedPtr<FJsonObject> FUnrealMCPAssetWorkflowCommands::HandleAssetDelete(
    const TSharedPtr<FJsonObject>& Params
)
{
    FString AssetPath;
    if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath))
    {
        return ErrorResponse(TEXT("asset_delete requires asset_path"));
    }
    AssetPath = NormalizePackagePath(AssetPath);
    if (!IsExactGamePackagePath(AssetPath))
    {
        return ErrorResponse(TEXT("asset_delete requires an exact /Game package path"));
    }

    bool bDryRun = true;
    Params->TryGetBoolField(TEXT("dry_run"), bDryRun);
    TArray<FString> ChangedPackages = {AssetPath};
    if (bDryRun)
    {
        TSharedPtr<FJsonObject> ResultObj = BoolResult(TEXT("delete"), true, true, ChangedPackages);
        ResultObj->SetStringField(TEXT("asset_path"), AssetPath);
        return ResultObj;
    }

    UEditorAssetSubsystem* AssetSubsystem = GetEditorAssetSubsystem();
    if (!AssetSubsystem)
    {
        return ErrorResponse(TEXT("Editor asset subsystem is not available"));
    }
    const bool bDeleted = AssetSubsystem->DeleteAsset(AssetPath);
    if (!bDeleted)
    {
        return ErrorResponse(FString::Printf(TEXT("Failed to delete asset: %s"), *AssetPath));
    }
    TSharedPtr<FJsonObject> ResultObj = BoolResult(TEXT("delete"), true, false, ChangedPackages);
    ResultObj->SetStringField(TEXT("asset_path"), AssetPath);
    return ResultObj;
}

TSharedPtr<FJsonObject> FUnrealMCPAssetWorkflowCommands::HandleAssetSavePackages(
    const TSharedPtr<FJsonObject>& Params
)
{
    TArray<FString> PackagePaths = GetStringArrayField(Params, TEXT("package_paths"));
    if (PackagePaths.IsEmpty())
    {
        return ErrorResponse(TEXT("asset_save_packages requires package_paths"));
    }
    bool bOnlyIfDirty = true;
    Params->TryGetBoolField(TEXT("only_if_dirty"), bOnlyIfDirty);

    UEditorAssetSubsystem* AssetSubsystem = GetEditorAssetSubsystem();
    if (!AssetSubsystem)
    {
        return ErrorResponse(TEXT("Editor asset subsystem is not available"));
    }

    TArray<FString> SavedPackages;
    TArray<FString> FailedPackages;
    for (FString PackagePath : PackagePaths)
    {
        PackagePath = NormalizePackagePath(PackagePath);
        if (!IsExactGamePackagePath(PackagePath))
        {
            FailedPackages.Add(PackagePath);
            continue;
        }
        if (AssetSubsystem->SaveAsset(PackagePath, bOnlyIfDirty))
        {
            SavedPackages.Add(PackagePath);
        }
        else
        {
            FailedPackages.Add(PackagePath);
        }
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetArrayField(TEXT("saved_packages"), StringsToJsonArray(SavedPackages));
    ResultObj->SetArrayField(TEXT("failed_packages"), StringsToJsonArray(FailedPackages));
    ResultObj->SetBoolField(TEXT("only_if_dirty"), bOnlyIfDirty);
    return ResultObj;
}

TSharedPtr<FJsonObject> FUnrealMCPAssetWorkflowCommands::HandleAssetFixupRedirectors(
    const TSharedPtr<FJsonObject>& Params
)
{
    TArray<FString> Roots = GetStringArrayField(Params, TEXT("roots"));
    if (Roots.IsEmpty())
    {
        return ErrorResponse(TEXT("asset_fixup_redirectors requires roots"));
    }
    bool bDryRun = true;
    Params->TryGetBoolField(TEXT("dry_run"), bDryRun);

    FARFilter Filter;
    Filter.bRecursivePaths = true;
    Filter.bIncludeOnlyOnDiskAssets = false;
    Filter.ClassPaths.Add(UObjectRedirector::StaticClass()->GetClassPathName());
    for (FString Root : Roots)
    {
        Root = NormalizePackagePath(Root);
        if (!IsGameRootPath(Root))
        {
            return ErrorResponse(TEXT("asset_fixup_redirectors roots must be /Game package paths"));
        }
        Filter.PackagePaths.Add(*Root);
    }

    IAssetRegistry& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
    TArray<FAssetData> RedirectorData;
    AssetRegistry.GetAssets(Filter, RedirectorData);

    TArray<FString> RedirectorPaths;
    TArray<UObjectRedirector*> Redirectors;
    for (const FAssetData& AssetData : RedirectorData)
    {
        RedirectorPaths.Add(AssetData.GetObjectPathString());
        if (!bDryRun)
        {
            if (UObjectRedirector* Redirector = Cast<UObjectRedirector>(AssetData.GetAsset()))
            {
                Redirectors.Add(Redirector);
            }
        }
    }

    if (!bDryRun && !Redirectors.IsEmpty())
    {
        FAssetToolsModule& AssetToolsModule = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools"));
        AssetToolsModule.Get().FixupReferencers(
            Redirectors,
            false,
            ERedirectFixupMode::DeleteFixedUpRedirectors
        );
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetBoolField(TEXT("dry_run"), bDryRun);
    ResultObj->SetArrayField(TEXT("roots"), StringsToJsonArray(Roots));
    ResultObj->SetArrayField(TEXT("redirectors"), StringsToJsonArray(RedirectorPaths));
    ResultObj->SetNumberField(TEXT("redirector_count"), RedirectorPaths.Num());
    ResultObj->SetBoolField(TEXT("fixup_requested"), !bDryRun);
    return ResultObj;
}

TSharedPtr<FJsonObject> FUnrealMCPAssetWorkflowCommands::HandleAssetPrepareForLevel(
    const TSharedPtr<FJsonObject>& Params
)
{
    FString AssetPath;
    if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath))
    {
        return ErrorResponse(TEXT("asset_prepare_for_level requires asset_path"));
    }
    AssetPath = NormalizePackagePath(AssetPath);
    if (!IsExactGamePackagePath(AssetPath))
    {
        return ErrorResponse(TEXT("asset_prepare_for_level requires an exact /Game package path"));
    }

    UObject* Asset = LoadAssetObject(AssetPath);
    if (!Asset)
    {
        return ErrorResponse(FString::Printf(TEXT("Asset could not be loaded: %s"), *AssetPath));
    }

    TArray<FString> Warnings;
    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("asset_path"), AssetPath);
    ResultObj->SetStringField(TEXT("object_path"), Asset->GetPathName());
    ResultObj->SetStringField(TEXT("asset_class"), Asset->GetClass()->GetName());
    ResultObj->SetBoolField(TEXT("loadable"), true);
    ResultObj->SetArrayField(TEXT("recommended_scale"), VectorToJsonArray(FVector(1.0f, 1.0f, 1.0f)));

    if (UStaticMesh* StaticMesh = Cast<UStaticMesh>(Asset))
    {
        const FBoxSphereBounds Bounds = StaticMesh->GetBounds();
        UBodySetup* BodySetup = StaticMesh->GetBodySetup();
        const bool bHasSimpleCollision = BodySetup && BodySetup->AggGeom.GetElementCount() > 0;
        const bool bUsesComplexCollision = BodySetup && BodySetup->CollisionTraceFlag == CTF_UseComplexAsSimple;
        const bool bHasCollision = bHasSimpleCollision || bUsesComplexCollision;
        if (!bHasCollision)
        {
            Warnings.Add(TEXT("Static mesh has no simple collision and is not configured to use complex collision as simple"));
        }
        if (StaticMesh->GetStaticMaterials().IsEmpty())
        {
            Warnings.Add(TEXT("Static mesh has no material slots"));
        }

        TSharedPtr<FJsonObject> BoundsObj = MakeShared<FJsonObject>();
        BoundsObj->SetArrayField(TEXT("origin"), VectorToJsonArray(Bounds.Origin));
        BoundsObj->SetArrayField(TEXT("box_extent"), VectorToJsonArray(Bounds.BoxExtent));
        BoundsObj->SetNumberField(TEXT("sphere_radius"), Bounds.SphereRadius);

        ResultObj->SetStringField(TEXT("placement_kind"), TEXT("static_mesh_actor"));
        ResultObj->SetObjectField(TEXT("bounds"), BoundsObj);
        ResultObj->SetBoolField(TEXT("collision_present"), bHasCollision);
        ResultObj->SetNumberField(TEXT("material_slot_count"), StaticMesh->GetStaticMaterials().Num());
        ResultObj->SetNumberField(TEXT("section_count_lod0"), StaticMesh->GetNumSections(0));
        ResultObj->SetBoolField(TEXT("nanite_enabled"), StaticMesh->GetNaniteSettings().bEnabled);
        ResultObj->SetBoolField(TEXT("placement_ready"), true);
    }
    else if (UBlueprint* Blueprint = Cast<UBlueprint>(Asset))
    {
        ResultObj->SetStringField(TEXT("placement_kind"), TEXT("blueprint_actor"));
        ResultObj->SetBoolField(TEXT("placement_ready"), Blueprint->GeneratedClass && Blueprint->GeneratedClass->IsChildOf(AActor::StaticClass()));
        ResultObj->SetStringField(
            TEXT("generated_class"),
            Blueprint->GeneratedClass ? Blueprint->GeneratedClass->GetPathName() : FString()
        );
        if (!Blueprint->GeneratedClass)
        {
            Warnings.Add(TEXT("Blueprint has no generated class; compile it before placement"));
        }
        else if (!Blueprint->GeneratedClass->IsChildOf(AActor::StaticClass()))
        {
            Warnings.Add(TEXT("Blueprint generated class is not an Actor"));
        }
    }
    else
    {
        ResultObj->SetStringField(TEXT("placement_kind"), TEXT("unsupported"));
        ResultObj->SetBoolField(TEXT("placement_ready"), false);
        Warnings.Add(TEXT("Only StaticMesh and Actor Blueprint assets are directly placement-ready"));
    }

    ResultObj->SetArrayField(TEXT("warnings"), StringsToJsonArray(Warnings));
    return ResultObj;
}

TSharedPtr<FJsonObject> FUnrealMCPAssetWorkflowCommands::HandleAssetCreateBlueprintWrapper(
    const TSharedPtr<FJsonObject>& Params
)
{
    FString AssetPath;
    FString TargetPackagePath;
    if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) ||
        !Params->TryGetStringField(TEXT("target_package_path"), TargetPackagePath))
    {
        return ErrorResponse(TEXT("asset_create_blueprint_wrapper requires asset_path and target_package_path"));
    }
    AssetPath = NormalizePackagePath(AssetPath);
    TargetPackagePath = NormalizePackagePath(TargetPackagePath);
    if (!IsExactGamePackagePath(AssetPath) || !IsExactGamePackagePath(TargetPackagePath))
    {
        return ErrorResponse(TEXT("asset_create_blueprint_wrapper requires exact /Game package paths"));
    }

    FString ComponentName = TEXT("AssetMesh");
    Params->TryGetStringField(TEXT("component_name"), ComponentName);
    ComponentName = ComponentName.TrimStartAndEnd();
    if (ComponentName.IsEmpty())
    {
        ComponentName = TEXT("AssetMesh");
    }

    FString ParentClassName = TEXT("Actor");
    Params->TryGetStringField(TEXT("parent_class"), ParentClassName);
    bool bDryRun = true;
    Params->TryGetBoolField(TEXT("dry_run"), bDryRun);
    bool bSaveAsset = false;
    Params->TryGetBoolField(TEXT("save_asset"), bSaveAsset);

    UObject* Asset = LoadAssetObject(AssetPath);
    UStaticMesh* StaticMesh = Cast<UStaticMesh>(Asset);
    if (!StaticMesh)
    {
        return ErrorResponse(TEXT("asset_create_blueprint_wrapper currently requires a StaticMesh asset"));
    }

    IAssetRegistry& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
    const bool bTargetExists = AssetExists(AssetRegistry, TargetPackagePath);

    TSharedPtr<FJsonObject> PlanObj = MakeShared<FJsonObject>();
    PlanObj->SetStringField(TEXT("source_asset"), AssetPath);
    PlanObj->SetStringField(TEXT("target_package_path"), TargetPackagePath);
    PlanObj->SetStringField(TEXT("blueprint_name"), PackageLeafName(TargetPackagePath));
    PlanObj->SetStringField(TEXT("component_name"), ComponentName);
    PlanObj->SetStringField(TEXT("parent_class"), ParentClassName);
    PlanObj->SetBoolField(TEXT("target_exists"), bTargetExists);

    if (bDryRun)
    {
        PlanObj->SetBoolField(TEXT("dry_run"), true);
        return PlanObj;
    }
    if (bTargetExists)
    {
        return ErrorResponse(FString::Printf(TEXT("Target Blueprint already exists: %s"), *TargetPackagePath));
    }

    UClass* ParentClass = ResolveActorParentClass(ParentClassName);
    if (!ParentClass || !ParentClass->IsChildOf(AActor::StaticClass()))
    {
        return ErrorResponse(FString::Printf(TEXT("Invalid Actor parent class: %s"), *ParentClassName));
    }

    UPackage* Package = CreatePackage(*TargetPackagePath);
    if (!Package)
    {
        return ErrorResponse(FString::Printf(TEXT("Failed to create package: %s"), *TargetPackagePath));
    }

    UBlueprintFactory* Factory = NewObject<UBlueprintFactory>();
    Factory->ParentClass = ParentClass;
    UBlueprint* NewBlueprint = Cast<UBlueprint>(Factory->FactoryCreateNew(
        UBlueprint::StaticClass(),
        Package,
        *PackageLeafName(TargetPackagePath),
        RF_Public | RF_Standalone,
        nullptr,
        GWarn
    ));
    if (!NewBlueprint || !NewBlueprint->SimpleConstructionScript)
    {
        return ErrorResponse(TEXT("Failed to create Blueprint wrapper"));
    }

    USCS_Node* MeshNode = NewBlueprint->SimpleConstructionScript->CreateNode(
        UStaticMeshComponent::StaticClass(),
        *ComponentName
    );
    if (!MeshNode)
    {
        return ErrorResponse(TEXT("Failed to create StaticMeshComponent node for Blueprint wrapper"));
    }

    if (UStaticMeshComponent* MeshComponent = Cast<UStaticMeshComponent>(MeshNode->ComponentTemplate))
    {
        MeshComponent->SetStaticMesh(StaticMesh);
    }
    NewBlueprint->SimpleConstructionScript->AddNode(MeshNode);
    FAssetRegistryModule::AssetCreated(NewBlueprint);
    FKismetEditorUtilities::CompileBlueprint(NewBlueprint);
    Package->MarkPackageDirty();

    bool bSaved = false;
    if (bSaveAsset)
    {
        if (UEditorAssetSubsystem* AssetSubsystem = GetEditorAssetSubsystem())
        {
            bSaved = AssetSubsystem->SaveAsset(TargetPackagePath, false);
        }
    }

    TSharedPtr<FJsonObject> ResultObj = BoolResult(TEXT("create_blueprint_wrapper"), true, false, {TargetPackagePath});
    ResultObj->SetStringField(TEXT("blueprint_path"), NewBlueprint->GetPathName());
    ResultObj->SetStringField(TEXT("target_package_path"), TargetPackagePath);
    ResultObj->SetStringField(TEXT("source_asset"), AssetPath);
    ResultObj->SetBoolField(TEXT("save_requested"), bSaveAsset);
    ResultObj->SetBoolField(TEXT("saved"), bSaved);
    return ResultObj;
}

TSharedPtr<FJsonObject> FUnrealMCPAssetWorkflowCommands::HandleAssetPlaceInLevel(
    const TSharedPtr<FJsonObject>& Params
)
{
    TArray<TSharedPtr<FJsonObject>> PlacementInputs = GetObjectArrayField(Params, TEXT("placements"));
    if (PlacementInputs.IsEmpty())
    {
        return ErrorResponse(TEXT("asset_place_in_level requires placements"));
    }

    FString TargetMap;
    Params->TryGetStringField(TEXT("target_map"), TargetMap);
    bool bDryRun = true;
    Params->TryGetBoolField(TEXT("dry_run"), bDryRun);
    bool bSaveLevel = false;
    Params->TryGetBoolField(TEXT("save_level"), bSaveLevel);

    FString WorldError;
    UWorld* World = ResolveEditorWorld(WorldError);
    if (!World)
    {
        return ErrorResponse(WorldError);
    }
    if (!TargetMapMatches(World, TargetMap))
    {
        return ErrorResponse(FString::Printf(
            TEXT("Current map '%s' does not match requested target_map '%s'"),
            *CurrentMapFromWorld(World),
            *TargetMap
        ));
    }

    TArray<TSharedPtr<FJsonObject>> PlannedPlacements;
    TArray<TSharedPtr<FJsonObject>> CreatedActors;
    TArray<FString> ChangedPackages;
    TArray<FString> Warnings;

    if (!bDryRun)
    {
        FScopedTransaction Transaction(NSLOCTEXT("UnrealMCP", "AssetPlaceInLevel", "Place Assets In Level"));
        World->Modify();
        if (World->PersistentLevel)
        {
            World->PersistentLevel->Modify();
        }
    }

    int32 PlacementIndex = 0;
    for (const TSharedPtr<FJsonObject>& PlacementInput : PlacementInputs)
    {
        ++PlacementIndex;
        FString AssetPath;
        if (!PlacementInput->TryGetStringField(TEXT("asset_path"), AssetPath))
        {
            return ErrorResponse(TEXT("Each placement requires asset_path"));
        }
        AssetPath = NormalizePackagePath(AssetPath);
        if (!IsExactGamePackagePath(AssetPath))
        {
            return ErrorResponse(TEXT("Each placement requires an exact /Game asset_path"));
        }

        FString ActorName = FString::Printf(TEXT("PlacedAsset_%d"), PlacementIndex);
        PlacementInput->TryGetStringField(TEXT("actor_name"), ActorName);
        ActorName = ActorName.TrimStartAndEnd();
        if (ActorName.IsEmpty())
        {
            ActorName = FString::Printf(TEXT("PlacedAsset_%d"), PlacementIndex);
        }

        const FVector Location = JsonArrayToVector(PlacementInput, TEXT("location"), FVector::ZeroVector);
        const FRotator Rotation = JsonArrayToRotator(PlacementInput, TEXT("rotation"), FRotator::ZeroRotator);
        const FVector Scale = JsonArrayToVector(PlacementInput, TEXT("scale"), FVector(1.0f, 1.0f, 1.0f));

        PlannedPlacements.Add(PlacementPlanToJson(AssetPath, ActorName, Location, Rotation, Scale));
        if (bDryRun)
        {
            continue;
        }

        if (FindActorByName(World, ActorName))
        {
            return ErrorResponse(FString::Printf(TEXT("Actor already exists in level: %s"), *ActorName));
        }

        UObject* Asset = LoadAssetObject(AssetPath);
        if (!Asset)
        {
            return ErrorResponse(FString::Printf(TEXT("Asset could not be loaded: %s"), *AssetPath));
        }

        AActor* NewActor = nullptr;
        if (UStaticMesh* StaticMesh = Cast<UStaticMesh>(Asset))
        {
            if (UEditorActorSubsystem* ActorSubsystem = GetEditorActorSubsystem())
            {
                NewActor = ActorSubsystem->SpawnActorFromClass(AStaticMeshActor::StaticClass(), Location, Rotation);
            }
            else
            {
                NewActor = World->SpawnActor<AStaticMeshActor>(AStaticMeshActor::StaticClass(), Location, Rotation);
            }
            if (AStaticMeshActor* StaticMeshActor = Cast<AStaticMeshActor>(NewActor))
            {
                StaticMeshActor->GetStaticMeshComponent()->SetStaticMesh(StaticMesh);
            }
        }
        else if (UBlueprint* Blueprint = Cast<UBlueprint>(Asset))
        {
            if (!Blueprint->GeneratedClass || !Blueprint->GeneratedClass->IsChildOf(AActor::StaticClass()))
            {
                return ErrorResponse(FString::Printf(TEXT("Blueprint is not an Actor Blueprint: %s"), *AssetPath));
            }
            if (UEditorActorSubsystem* ActorSubsystem = GetEditorActorSubsystem())
            {
                NewActor = ActorSubsystem->SpawnActorFromClass(TSubclassOf<AActor>(Blueprint->GeneratedClass), Location, Rotation);
            }
            else
            {
                FTransform SpawnTransform(Rotation, Location, Scale);
                NewActor = World->SpawnActor<AActor>(Blueprint->GeneratedClass, SpawnTransform);
            }
        }
        else
        {
            return ErrorResponse(FString::Printf(TEXT("Unsupported placement asset type: %s"), *Asset->GetClass()->GetName()));
        }

        if (!NewActor)
        {
            return ErrorResponse(FString::Printf(TEXT("Failed to place asset in level: %s"), *AssetPath));
        }

        NewActor->Modify();
        NewActor->SetActorLabel(ActorName);
        FTransform Transform(Rotation, Location, Scale);
        NewActor->SetActorTransform(Transform);
        NewActor->PostEditMove(true);
        NewActor->PostEditChange();
        MarkActorLevelDirty(NewActor);
        CreatedActors.Add(FUnrealMCPCommonUtils::ActorToJsonObject(NewActor, true));
    }

    bool bSaved = false;
    if (!bDryRun && bSaveLevel)
    {
        if (World->PersistentLevel)
        {
            bSaved = FEditorFileUtils::SaveLevel(World->PersistentLevel);
            ChangedPackages.AddUnique(World->PersistentLevel->GetOutermost()->GetName());
        }
        else
        {
            Warnings.Add(TEXT("Cannot save level because the editor world has no persistent level"));
        }
    }
    else if (!bDryRun && World->PersistentLevel)
    {
        ChangedPackages.AddUnique(World->PersistentLevel->GetOutermost()->GetName());
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetBoolField(TEXT("dry_run"), bDryRun);
    ResultObj->SetStringField(TEXT("current_map"), CurrentMapFromWorld(World));
    ResultObj->SetStringField(TEXT("target_map"), TargetMap);
    ResultObj->SetArrayField(TEXT("placements"), ObjectArrayToJsonArray(PlannedPlacements));
    ResultObj->SetArrayField(TEXT("created_actors"), ObjectArrayToJsonArray(CreatedActors));
    ResultObj->SetArrayField(TEXT("changed_packages"), StringsToJsonArray(ChangedPackages));
    ResultObj->SetArrayField(TEXT("warnings"), StringsToJsonArray(Warnings));
    ResultObj->SetBoolField(TEXT("save_requested"), bSaveLevel);
    ResultObj->SetBoolField(TEXT("saved"), bSaved);
    return ResultObj;
}

TSharedPtr<FJsonObject> FUnrealMCPAssetWorkflowCommands::HandleAssetValidateLevelPlacements(
    const TSharedPtr<FJsonObject>& Params
)
{
    TArray<FString> ExpectedActors = GetStringArrayField(Params, TEXT("expected_actors"));
    if (ExpectedActors.IsEmpty())
    {
        return ErrorResponse(TEXT("asset_validate_level_placements requires expected_actors"));
    }

    FString TargetMap;
    Params->TryGetStringField(TEXT("target_map"), TargetMap);
    FString WorldError;
    UWorld* World = ResolveEditorWorld(WorldError);
    if (!World)
    {
        return ErrorResponse(WorldError);
    }
    if (!TargetMapMatches(World, TargetMap))
    {
        return ErrorResponse(FString::Printf(
            TEXT("Current map '%s' does not match requested target_map '%s'"),
            *CurrentMapFromWorld(World),
            *TargetMap
        ));
    }

    TArray<FString> FoundActors;
    TArray<FString> MissingActors;
    TArray<TSharedPtr<FJsonObject>> FoundActorObjects;
    for (const FString& ExpectedActor : ExpectedActors)
    {
        if (AActor* Actor = FindActorByName(World, ExpectedActor))
        {
            FoundActors.Add(ExpectedActor);
            FoundActorObjects.Add(FUnrealMCPCommonUtils::ActorToJsonObject(Actor, false));
        }
        else
        {
            MissingActors.Add(ExpectedActor);
        }
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("current_map"), CurrentMapFromWorld(World));
    ResultObj->SetStringField(TEXT("target_map"), TargetMap);
    ResultObj->SetArrayField(TEXT("expected_actors"), StringsToJsonArray(ExpectedActors));
    ResultObj->SetArrayField(TEXT("found_actors"), StringsToJsonArray(FoundActors));
    ResultObj->SetArrayField(TEXT("missing_actors"), StringsToJsonArray(MissingActors));
    ResultObj->SetArrayField(TEXT("found_actor_details"), ObjectArrayToJsonArray(FoundActorObjects));
    ResultObj->SetBoolField(TEXT("all_present"), MissingActors.IsEmpty());
    return ResultObj;
}
