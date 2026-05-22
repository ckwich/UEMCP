#include "Commands/UnrealMCPAssetWorkflowCommands.h"
#include "Commands/UnrealMCPCommonUtils.h"

#include "AssetRegistry/ARFilter.h"
#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/AssetDataTagMap.h"
#include "AssetRegistry/AssetIdentifier.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Dom/JsonValue.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Modules/ModuleManager.h"

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
