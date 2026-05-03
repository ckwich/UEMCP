#include "Commands/UnrealMCPEditorCommands.h"
#include "Commands/UnrealMCPCommonUtils.h"
#include "Observability/UEMCPOutputLogCapture.h"
#include "AssetRegistry/ARFilter.h"
#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/AssetIdentifier.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Editor.h"
#include "EditorViewportClient.h"
#include "LevelEditorViewport.h"
#include "ImageUtils.h"
#include "HighResScreenshot.h"
#include "CoreGlobals.h"
#include "Engine/GameViewportClient.h"
#include "Misc/FileHelper.h"
#include "GameFramework/Actor.h"
#include "Engine/Selection.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Misc/App.h"
#include "Misc/AutomationEvent.h"
#include "Misc/AutomationTest.h"
#include "Misc/EngineVersion.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "Kismet/GameplayStatics.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/DirectionalLight.h"
#include "Engine/PointLight.h"
#include "Engine/SpotLight.h"
#include "Camera/CameraActor.h"
#include "Components/StaticMeshComponent.h"
#include "EditorSubsystem.h"
#include "Subsystems/EditorActorSubsystem.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "UObject/UObjectIterator.h"

namespace
{
    constexpr const TCHAR* UEMCPPluginVersion = TEXT("0.1.0");
    constexpr int32 MaxAutomationEventEntries = 50;

    FString AutomationEventTypeToString(EAutomationEventType EventType)
    {
        switch (EventType)
        {
            case EAutomationEventType::Info:
                return TEXT("info");
            case EAutomationEventType::Warning:
                return TEXT("warning");
            case EAutomationEventType::Error:
                return TEXT("error");
            default:
                return TEXT("unknown");
        }
    }

    TSharedPtr<FJsonObject> AutomationTestInfoToJson(const FAutomationTestInfo& TestInfo)
    {
        TSharedPtr<FJsonObject> TestObj = MakeShared<FJsonObject>();
        TestObj->SetStringField(TEXT("display_name"), TestInfo.GetDisplayName());
        TestObj->SetStringField(TEXT("full_test_path"), TestInfo.GetFullTestPath());
        TestObj->SetStringField(TEXT("test_name"), TestInfo.GetTestName());
        TestObj->SetStringField(TEXT("tags"), TestInfo.GetTestTags());
        TestObj->SetStringField(TEXT("test_parameter"), TestInfo.GetTestParameter());
        TestObj->SetStringField(TEXT("source_file"), TestInfo.GetSourceFile());
        TestObj->SetNumberField(TEXT("source_line"), TestInfo.GetSourceFileLine());
        TestObj->SetStringField(TEXT("asset_path"), TestInfo.GetAssetPath());
        TestObj->SetStringField(TEXT("open_command"), TestInfo.GetOpenCommand());
        TestObj->SetNumberField(TEXT("flags"), static_cast<double>(static_cast<uint32>(TestInfo.GetTestFlags())));
        TestObj->SetNumberField(TEXT("participants_required"), TestInfo.GetNumParticipantsRequired());
        return TestObj;
    }

    TSharedPtr<FJsonObject> AutomationExecutionEntryToJson(const FAutomationExecutionEntry& Entry)
    {
        TSharedPtr<FJsonObject> EntryObj = MakeShared<FJsonObject>();
        EntryObj->SetStringField(TEXT("type"), AutomationEventTypeToString(Entry.Event.Type));
        EntryObj->SetStringField(TEXT("message"), Entry.Event.Message);
        EntryObj->SetStringField(TEXT("context"), Entry.Event.Context);
        EntryObj->SetStringField(TEXT("filename"), Entry.Filename);
        EntryObj->SetNumberField(TEXT("line_number"), Entry.LineNumber);
        EntryObj->SetStringField(TEXT("timestamp_utc"), Entry.Timestamp.ToIso8601());
        EntryObj->SetStringField(TEXT("formatted"), Entry.ToString());
        return EntryObj;
    }

    bool AutomationTestMatchesPrefix(const FAutomationTestInfo& TestInfo, const FString& Prefix)
    {
        return Prefix.IsEmpty() ||
            TestInfo.GetFullTestPath().StartsWith(Prefix, ESearchCase::IgnoreCase) ||
            TestInfo.GetTestName().StartsWith(Prefix, ESearchCase::IgnoreCase);
    }

    TArray<FAutomationTestInfo> GetCurrentAutomationTests()
    {
        FAutomationTestFramework& Framework = FAutomationTestFramework::Get();
        Framework.LoadTestModules();

        TArray<FAutomationTestInfo> TestInfos;
        Framework.GetValidTestNames(TestInfos);
        TestInfos.Sort([](const FAutomationTestInfo& Left, const FAutomationTestInfo& Right)
        {
            return Left.GetFullTestPath() < Right.GetFullTestPath();
        });
        return TestInfos;
    }

    const FAutomationTestInfo* FindAutomationTestByName(
        const TArray<FAutomationTestInfo>& TestInfos,
        const FString& RequestedTestName
    )
    {
        for (const FAutomationTestInfo& TestInfo : TestInfos)
        {
            if (TestInfo.GetFullTestPath().Equals(RequestedTestName, ESearchCase::IgnoreCase) ||
                TestInfo.GetTestName().Equals(RequestedTestName, ESearchCase::IgnoreCase))
            {
                return &TestInfo;
            }
        }

        return nullptr;
    }

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
            return TEXT("/Game");
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

    FString NormalizeAssetPackageName(const FString& RawAssetPath)
    {
        FString AssetPath = RawAssetPath.TrimStartAndEnd();
        AssetPath.ReplaceInline(TEXT("\\"), TEXT("/"));
        AssetPath = TrimTrailingSlashes(AssetPath);

        if (AssetPath.IsEmpty())
        {
            return FString();
        }

        FString LongPackageName;
        if ((AssetPath.EndsWith(TEXT(".uasset"), ESearchCase::IgnoreCase) ||
             AssetPath.EndsWith(TEXT(".umap"), ESearchCase::IgnoreCase)) &&
            FPackageName::TryConvertFilenameToLongPackageName(AssetPath, LongPackageName))
        {
            return LongPackageName;
        }

        const int32 ValueIndex = AssetPath.Find(
            TEXT("::"),
            ESearchCase::CaseSensitive,
            ESearchDir::FromStart
        );
        if (ValueIndex != INDEX_NONE)
        {
            AssetPath.LeftInline(ValueIndex);
        }

        int32 ObjectDelimiterIndex = INDEX_NONE;
        if (AssetPath.FindChar(TEXT('.'), ObjectDelimiterIndex))
        {
            AssetPath.LeftInline(ObjectDelimiterIndex);
        }

        return TrimTrailingSlashes(NormalizeAssetRoot(AssetPath));
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
        const FString& ClassFilter,
        const FString& AssetClass,
        const FString& AssetClassPath
    )
    {
        return ClassFilter.IsEmpty() ||
            AssetClass.Equals(ClassFilter, ESearchCase::IgnoreCase) ||
            AssetClassPath.Equals(ClassFilter, ESearchCase::IgnoreCase) ||
            AssetClassPath.Contains(ClassFilter, ESearchCase::IgnoreCase);
    }

    bool StringFilterMatches(const FString& Value, const FString& Filter)
    {
        return Filter.IsEmpty() || Value.Contains(Filter, ESearchCase::IgnoreCase);
    }

    TSharedPtr<FJsonObject> AssetDataToJson(const FAssetData& AssetData)
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
        return AssetObj;
    }

    FString DependencyCategoryToString(UE::AssetRegistry::EDependencyCategory Category)
    {
        if (Category == UE::AssetRegistry::EDependencyCategory::Package)
        {
            return TEXT("Package");
        }
        if (Category == UE::AssetRegistry::EDependencyCategory::SearchableName)
        {
            return TEXT("SearchableName");
        }
        if (Category == UE::AssetRegistry::EDependencyCategory::Manage)
        {
            return TEXT("Manage");
        }
        if (Category == UE::AssetRegistry::EDependencyCategory::None)
        {
            return TEXT("None");
        }
        return TEXT("Mixed");
    }

    void AddDependencyPropertyName(
        TArray<TSharedPtr<FJsonValue>>& Properties,
        UE::AssetRegistry::EDependencyProperty AllProperties,
        UE::AssetRegistry::EDependencyProperty Property,
        const TCHAR* PropertyName
    )
    {
        if (EnumHasAnyFlags(AllProperties, Property))
        {
            Properties.Add(MakeShared<FJsonValueString>(PropertyName));
        }
    }

    TArray<TSharedPtr<FJsonValue>> DependencyPropertiesToJson(
        UE::AssetRegistry::EDependencyProperty Properties
    )
    {
        TArray<TSharedPtr<FJsonValue>> PropertyNames;
        AddDependencyPropertyName(
            PropertyNames,
            Properties,
            UE::AssetRegistry::EDependencyProperty::Hard,
            TEXT("Hard")
        );
        AddDependencyPropertyName(
            PropertyNames,
            Properties,
            UE::AssetRegistry::EDependencyProperty::Game,
            TEXT("Game")
        );
        AddDependencyPropertyName(
            PropertyNames,
            Properties,
            UE::AssetRegistry::EDependencyProperty::Build,
            TEXT("Build")
        );
        AddDependencyPropertyName(
            PropertyNames,
            Properties,
            UE::AssetRegistry::EDependencyProperty::Direct,
            TEXT("Direct")
        );
        AddDependencyPropertyName(
            PropertyNames,
            Properties,
            UE::AssetRegistry::EDependencyProperty::CookRule,
            TEXT("CookRule")
        );
        return PropertyNames;
    }

    UE::AssetRegistry::FDependencyQuery BuildPackageDependencyQuery(
        bool bIncludeHard,
        bool bIncludeSoft
    )
    {
        UE::AssetRegistry::FDependencyQuery Query;
        if (bIncludeHard && !bIncludeSoft)
        {
            Query.Required |= UE::AssetRegistry::EDependencyProperty::Hard;
        }
        else if (!bIncludeHard && bIncludeSoft)
        {
            Query.Excluded |= UE::AssetRegistry::EDependencyProperty::Hard;
        }
        return Query;
    }

    bool TryGetFirstAssetDataForPackage(
        IAssetRegistry& AssetRegistry,
        FName PackageName,
        FAssetData& OutAssetData,
        int32& OutAssetCount
    )
    {
        TArray<FAssetData> AssetDataList;
        AssetRegistry.GetAssetsByPackageName(PackageName, AssetDataList, false, false);
        AssetDataList.Sort([](const FAssetData& Left, const FAssetData& Right)
        {
            return Left.GetObjectPathString() < Right.GetObjectPathString();
        });

        OutAssetCount = AssetDataList.Num();
        if (AssetDataList.IsEmpty())
        {
            return false;
        }

        OutAssetData = AssetDataList[0];
        return true;
    }

    TSharedPtr<FJsonObject> AssetDependencyToJson(
        const FAssetDependency& Dependency,
        IAssetRegistry& AssetRegistry
    )
    {
        const FAssetIdentifier& AssetId = Dependency.AssetId;
        const bool bHard = EnumHasAnyFlags(
            Dependency.Properties,
            UE::AssetRegistry::EDependencyProperty::Hard
        );
        const bool bGame = EnumHasAnyFlags(
            Dependency.Properties,
            UE::AssetRegistry::EDependencyProperty::Game
        );

        TSharedPtr<FJsonObject> DependencyObj = MakeShared<FJsonObject>();
        DependencyObj->SetStringField(TEXT("identifier"), AssetId.ToString());
        DependencyObj->SetStringField(TEXT("category"), DependencyCategoryToString(Dependency.Category));
        DependencyObj->SetArrayField(
            TEXT("properties"),
            DependencyPropertiesToJson(Dependency.Properties)
        );
        DependencyObj->SetBoolField(TEXT("hard"), bHard);
        DependencyObj->SetBoolField(TEXT("soft"), !bHard);
        DependencyObj->SetBoolField(TEXT("game"), bGame);
        DependencyObj->SetBoolField(TEXT("editor_only"), !bGame);

        if (AssetId.PackageName != NAME_None)
        {
            const FString PackageName = AssetId.PackageName.ToString();
            DependencyObj->SetStringField(TEXT("package_name"), PackageName);
            DependencyObj->SetStringField(TEXT("package_path"), FPackageName::GetLongPackagePath(PackageName));
        }
        if (AssetId.ObjectName != NAME_None)
        {
            DependencyObj->SetStringField(TEXT("object_name"), AssetId.ObjectName.ToString());
        }
        if (AssetId.ValueName != NAME_None)
        {
            DependencyObj->SetStringField(TEXT("value_name"), AssetId.ValueName.ToString());
        }
        if (AssetId.GetPrimaryAssetId().IsValid())
        {
            DependencyObj->SetStringField(TEXT("primary_asset_id"), AssetId.GetPrimaryAssetId().ToString());
        }

        if (AssetId.PackageName != NAME_None)
        {
            FAssetData RelatedAssetData;
            int32 RelatedAssetCount = 0;
            const bool bFoundAssetData = TryGetFirstAssetDataForPackage(
                AssetRegistry,
                AssetId.PackageName,
                RelatedAssetData,
                RelatedAssetCount
            );
            DependencyObj->SetBoolField(TEXT("asset_found"), bFoundAssetData);
            DependencyObj->SetNumberField(TEXT("asset_count"), RelatedAssetCount);
            if (bFoundAssetData)
            {
                DependencyObj->SetObjectField(TEXT("asset"), AssetDataToJson(RelatedAssetData));
                DependencyObj->SetStringField(TEXT("asset_name"), RelatedAssetData.AssetName.ToString());
                DependencyObj->SetStringField(TEXT("object_path"), RelatedAssetData.GetObjectPathString());
                DependencyObj->SetStringField(
                    TEXT("asset_class"),
                    AssetClassShortName(RelatedAssetData.AssetClassPath.ToString())
                );
                DependencyObj->SetStringField(
                    TEXT("asset_class_path"),
                    RelatedAssetData.AssetClassPath.ToString()
                );
            }
        }

        return DependencyObj;
    }

    TSharedPtr<FJsonObject> HandleAssetRelationshipQuery(
        const TSharedPtr<FJsonObject>& Params,
        bool bReferencers
    )
    {
        double RequestedLimit = 100.0;
        if (Params->HasField(TEXT("limit")))
        {
            Params->TryGetNumberField(TEXT("limit"), RequestedLimit);
        }
        const int32 Limit = FMath::Clamp(static_cast<int32>(RequestedLimit), 1, 1000);

        FString AssetPath;
        Params->TryGetStringField(TEXT("asset_path"), AssetPath);
        const FString PackageNameString = NormalizeAssetPackageName(AssetPath);
        if (PackageNameString.IsEmpty())
        {
            return FUnrealMCPCommonUtils::CreateErrorResponse(
                TEXT("Missing required asset_path for Asset Registry relationship query")
            );
        }

        bool bIncludeHard = true;
        if (Params->HasField(TEXT("include_hard")))
        {
            Params->TryGetBoolField(TEXT("include_hard"), bIncludeHard);
        }
        bool bIncludeSoft = true;
        if (Params->HasField(TEXT("include_soft")))
        {
            Params->TryGetBoolField(TEXT("include_soft"), bIncludeSoft);
        }

        IAssetRegistry& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(
            TEXT("AssetRegistry")
        ).Get();

        const FName PackageName(*PackageNameString);
        FAssetData SourceAssetData;
        int32 SourceAssetCount = 0;
        const bool bFoundSourceAsset = TryGetFirstAssetDataForPackage(
            AssetRegistry,
            PackageName,
            SourceAssetData,
            SourceAssetCount
        );

        TArray<FAssetDependency> Relationships;
        bool bQuerySucceeded = true;
        if (bIncludeHard || bIncludeSoft)
        {
            const UE::AssetRegistry::FDependencyQuery Query = BuildPackageDependencyQuery(
                bIncludeHard,
                bIncludeSoft
            );
            if (bReferencers)
            {
                bQuerySucceeded = AssetRegistry.GetReferencers(
                    FAssetIdentifier(PackageName),
                    Relationships,
                    UE::AssetRegistry::EDependencyCategory::Package,
                    Query
                );
            }
            else
            {
                bQuerySucceeded = AssetRegistry.GetDependencies(
                    FAssetIdentifier(PackageName),
                    Relationships,
                    UE::AssetRegistry::EDependencyCategory::Package,
                    Query
                );
            }
        }

        Relationships.Sort([](const FAssetDependency& Left, const FAssetDependency& Right)
        {
            return Left.LexicalLess(Right);
        });

        TArray<TSharedPtr<FJsonValue>> RelationshipValues;
        RelationshipValues.Reserve(FMath::Min(Limit, Relationships.Num()));
        for (const FAssetDependency& Relationship : Relationships)
        {
            if (RelationshipValues.Num() >= Limit)
            {
                break;
            }
            RelationshipValues.Add(MakeShared<FJsonValueObject>(
                AssetDependencyToJson(Relationship, AssetRegistry)
            ));
        }

        TSharedPtr<FJsonObject> FiltersObj = MakeShared<FJsonObject>();
        FiltersObj->SetNumberField(TEXT("limit"), Limit);
        FiltersObj->SetStringField(TEXT("asset_path"), AssetPath);
        FiltersObj->SetStringField(TEXT("package_name"), PackageNameString);
        FiltersObj->SetBoolField(TEXT("include_hard"), bIncludeHard);
        FiltersObj->SetBoolField(TEXT("include_soft"), bIncludeSoft);

        TArray<TSharedPtr<FJsonValue>> Warnings;
        if (AssetRegistry.IsLoadingAssets())
        {
            Warnings.Add(MakeShared<FJsonValueString>(
                TEXT("Asset Registry is still loading assets; relationship results may be incomplete")
            ));
        }
        if (!bFoundSourceAsset)
        {
            Warnings.Add(MakeShared<FJsonValueString>(
                TEXT("No Asset Registry asset data was found for the requested package")
            ));
        }

        const TCHAR* RelationshipArrayName = bReferencers ? TEXT("referencers") : TEXT("dependencies");
        const TCHAR* MatchedCountName = bReferencers
            ? TEXT("matched_referencer_count")
            : TEXT("matched_dependency_count");
        const TCHAR* ReturnedCountName = bReferencers
            ? TEXT("returned_referencer_count")
            : TEXT("returned_dependency_count");

        TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
        ResultObj->SetStringField(TEXT("asset_path"), AssetPath);
        ResultObj->SetStringField(TEXT("package_name"), PackageNameString);
        ResultObj->SetBoolField(TEXT("asset_found"), bFoundSourceAsset);
        ResultObj->SetNumberField(TEXT("source_asset_count"), SourceAssetCount);
        if (bFoundSourceAsset)
        {
            ResultObj->SetObjectField(TEXT("source_asset"), AssetDataToJson(SourceAssetData));
        }
        ResultObj->SetArrayField(RelationshipArrayName, RelationshipValues);
        ResultObj->SetNumberField(MatchedCountName, Relationships.Num());
        ResultObj->SetNumberField(ReturnedCountName, RelationshipValues.Num());
        ResultObj->SetBoolField(TEXT("truncated"), Relationships.Num() > RelationshipValues.Num());
        ResultObj->SetBoolField(TEXT("query_succeeded"), bQuerySucceeded);
        ResultObj->SetBoolField(TEXT("asset_registry_loading"), AssetRegistry.IsLoadingAssets());
        ResultObj->SetObjectField(TEXT("filters"), FiltersObj);
        ResultObj->SetArrayField(TEXT("warnings"), Warnings);

        return ResultObj;
    }
}

FUnrealMCPEditorCommands::FUnrealMCPEditorCommands()
{
}

TSharedPtr<FJsonObject> FUnrealMCPEditorCommands::HandleCommand(const FString& CommandType, const TSharedPtr<FJsonObject>& Params)
{
    // Read-only observability commands
    if (CommandType == TEXT("get_editor_status"))
    {
        return HandleGetEditorStatus(Params);
    }
    else if (CommandType == TEXT("get_output_log"))
    {
        return HandleGetOutputLog(Params);
    }
    else if (CommandType == TEXT("asset_search"))
    {
        return HandleAssetSearch(Params);
    }
    else if (CommandType == TEXT("asset_dependencies"))
    {
        return HandleAssetDependencies(Params);
    }
    else if (CommandType == TEXT("asset_referencers"))
    {
        return HandleAssetReferencers(Params);
    }
    else if (CommandType == TEXT("list_automation_tests"))
    {
        return HandleListAutomationTests(Params);
    }
    else if (CommandType == TEXT("run_automation_test"))
    {
        return HandleRunAutomationTest(Params);
    }
    // Actor manipulation commands
    else if (CommandType == TEXT("get_actors_in_level"))
    {
        return HandleGetActorsInLevel(Params);
    }
    else if (CommandType == TEXT("find_actors_by_name"))
    {
        return HandleFindActorsByName(Params);
    }
    else if (CommandType == TEXT("spawn_actor") || CommandType == TEXT("create_actor"))
    {
        if (CommandType == TEXT("create_actor"))
        {
            UE_LOG(LogTemp, Warning, TEXT("'create_actor' command is deprecated and will be removed in a future version. Please use 'spawn_actor' instead."));
        }
        return HandleSpawnActor(Params);
    }
    else if (CommandType == TEXT("delete_actor"))
    {
        return HandleDeleteActor(Params);
    }
    else if (CommandType == TEXT("set_actor_transform"))
    {
        return HandleSetActorTransform(Params);
    }
    else if (CommandType == TEXT("get_actor_properties"))
    {
        return HandleGetActorProperties(Params);
    }
    else if (CommandType == TEXT("set_actor_property"))
    {
        return HandleSetActorProperty(Params);
    }
    // Blueprint actor spawning
    else if (CommandType == TEXT("spawn_blueprint_actor"))
    {
        return HandleSpawnBlueprintActor(Params);
    }
    // Editor viewport commands
    else if (CommandType == TEXT("focus_viewport"))
    {
        return HandleFocusViewport(Params);
    }
    else if (CommandType == TEXT("take_screenshot"))
    {
        return HandleTakeScreenshot(Params);
    }
    
    return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Unknown editor command: %s"), *CommandType));
}

TSharedPtr<FJsonObject> FUnrealMCPEditorCommands::HandleGetEditorStatus(const TSharedPtr<FJsonObject>& Params)
{
    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();

    ResultObj->SetStringField(TEXT("plugin_version"), UEMCPPluginVersion);
    ResultObj->SetStringField(TEXT("engine_version"), FEngineVersion::Current().ToString());
    ResultObj->SetStringField(TEXT("project_name"), FApp::GetProjectName());
    ResultObj->SetStringField(TEXT("project_path"), FPaths::ConvertRelativePathToFull(FPaths::GetProjectFilePath()));

    UWorld* EditorWorld = nullptr;
    if (GEditor)
    {
        EditorWorld = GEditor->GetEditorWorldContext().World();
    }

    FString CurrentMap;
    if (EditorWorld)
    {
        CurrentMap = EditorWorld->GetOutermost() ? EditorWorld->GetOutermost()->GetName() : EditorWorld->GetMapName();
    }
    ResultObj->SetStringField(TEXT("current_map"), CurrentMap);

    ResultObj->SetBoolField(TEXT("is_pie_running"), GEditor && GEditor->PlayWorld != nullptr);
    ResultObj->SetBoolField(TEXT("is_slow_task_active"), GIsSlowTask);
    ResultObj->SetNumberField(TEXT("selected_actor_count"), GEditor ? GEditor->GetSelectedActorCount() : 0);

    int32 DirtyPackageCount = 0;
    for (TObjectIterator<UPackage> PackageIt; PackageIt; ++PackageIt)
    {
        if (PackageIt->IsDirty())
        {
            ++DirtyPackageCount;
        }
    }
    ResultObj->SetNumberField(TEXT("dirty_package_count"), DirtyPackageCount);

    return ResultObj;
}

TSharedPtr<FJsonObject> FUnrealMCPEditorCommands::HandleGetOutputLog(const TSharedPtr<FJsonObject>& Params)
{
    double RequestedLimit = 200.0;
    if (Params->HasField(TEXT("limit")))
    {
        Params->TryGetNumberField(TEXT("limit"), RequestedLimit);
    }
    const int32 Limit = FMath::Clamp(static_cast<int32>(RequestedLimit), 1, 1000);

    FString Category;
    Params->TryGetStringField(TEXT("category"), Category);

    FString Verbosity;
    Params->TryGetStringField(TEXT("verbosity"), Verbosity);

    FString Contains;
    Params->TryGetStringField(TEXT("contains"), Contains);

    TSharedPtr<FJsonObject> FiltersObj = MakeShared<FJsonObject>();
    FiltersObj->SetNumberField(TEXT("limit"), Limit);
    if (!Category.IsEmpty())
    {
        FiltersObj->SetStringField(TEXT("category"), Category);
    }
    if (!Verbosity.IsEmpty())
    {
        FiltersObj->SetStringField(TEXT("verbosity"), Verbosity);
    }
    if (!Contains.IsEmpty())
    {
        FiltersObj->SetStringField(TEXT("contains"), Contains);
    }

    FUEMCPOutputLogCapture& OutputLogCapture = FUEMCPOutputLogCapture::Get();
    OutputLogCapture.Register();

    TArray<FUEMCPOutputLogEntry> CapturedEntries;
    const int32 MatchedEntryCount = OutputLogCapture.Query(Limit, Category, Verbosity, Contains, CapturedEntries);

    TArray<TSharedPtr<FJsonValue>> Entries;
    Entries.Reserve(CapturedEntries.Num());
    for (const FUEMCPOutputLogEntry& Entry : CapturedEntries)
    {
        TSharedPtr<FJsonObject> EntryObj = MakeShared<FJsonObject>();
        EntryObj->SetNumberField(TEXT("sequence"), static_cast<double>(Entry.Sequence));
        EntryObj->SetNumberField(TEXT("time_seconds"), Entry.TimeSeconds);
        EntryObj->SetStringField(TEXT("timestamp_utc"), Entry.TimestampUtc);
        EntryObj->SetStringField(TEXT("category"), Entry.Category);
        EntryObj->SetStringField(TEXT("verbosity"), Entry.Verbosity);
        EntryObj->SetStringField(TEXT("message"), Entry.Message);
        Entries.Add(MakeShared<FJsonValueObject>(EntryObj));
    }

    TArray<TSharedPtr<FJsonValue>> Warnings;

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetArrayField(TEXT("entries"), Entries);
    ResultObj->SetBoolField(TEXT("truncated"), MatchedEntryCount > CapturedEntries.Num());
    ResultObj->SetNumberField(TEXT("buffer_capacity"), OutputLogCapture.GetCapacity());
    ResultObj->SetNumberField(TEXT("captured_entry_count"), OutputLogCapture.GetCapturedEntryCount());
    ResultObj->SetNumberField(TEXT("matched_entry_count"), MatchedEntryCount);
    ResultObj->SetObjectField(TEXT("filters"), FiltersObj);
    ResultObj->SetArrayField(TEXT("warnings"), Warnings);

    return ResultObj;
}

TSharedPtr<FJsonObject> FUnrealMCPEditorCommands::HandleAssetSearch(const TSharedPtr<FJsonObject>& Params)
{
    double RequestedLimit = 100.0;
    if (Params->HasField(TEXT("limit")))
    {
        Params->TryGetNumberField(TEXT("limit"), RequestedLimit);
    }
    const int32 Limit = FMath::Clamp(static_cast<int32>(RequestedLimit), 1, 1000);

    FString Root;
    Params->TryGetStringField(TEXT("root"), Root);
    const FString PackagePath = NormalizeAssetRoot(Root);

    FString ClassName;
    Params->TryGetStringField(TEXT("class_name"), ClassName);
    ClassName = ClassName.TrimStartAndEnd();

    FString NameContains;
    Params->TryGetStringField(TEXT("name_contains"), NameContains);
    NameContains = NameContains.TrimStartAndEnd();

    FString PathContains;
    Params->TryGetStringField(TEXT("path_contains"), PathContains);
    PathContains = PathContains.TrimStartAndEnd();

    FARFilter Filter;
    Filter.PackagePaths.Add(*PackagePath);
    Filter.bRecursivePaths = true;
    Filter.bIncludeOnlyOnDiskAssets = false;

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

    int32 MatchedAssetCount = 0;
    for (const FAssetData& AssetData : AssetDataList)
    {
        const FString AssetName = AssetData.AssetName.ToString();
        const FString ObjectPath = AssetData.GetObjectPathString();
        const FString PackageName = AssetData.PackageName.ToString();
        const FString AssetClassPath = AssetData.AssetClassPath.ToString();
        const FString AssetClass = AssetClassShortName(AssetClassPath);

        if (!AssetClassMatches(ClassName, AssetClass, AssetClassPath))
        {
            continue;
        }
        if (!StringFilterMatches(AssetName, NameContains))
        {
            continue;
        }
        if (!StringFilterMatches(ObjectPath, PathContains) &&
            !StringFilterMatches(PackageName, PathContains))
        {
            continue;
        }

        ++MatchedAssetCount;
        if (Assets.Num() < Limit)
        {
            Assets.Add(MakeShared<FJsonValueObject>(AssetDataToJson(AssetData)));
        }
    }

    TSharedPtr<FJsonObject> FiltersObj = MakeShared<FJsonObject>();
    FiltersObj->SetNumberField(TEXT("limit"), Limit);
    if (!Root.IsEmpty())
    {
        FiltersObj->SetStringField(TEXT("root"), Root);
    }
    FiltersObj->SetStringField(TEXT("package_path"), PackagePath);
    if (!ClassName.IsEmpty())
    {
        FiltersObj->SetStringField(TEXT("class_name"), ClassName);
    }
    if (!NameContains.IsEmpty())
    {
        FiltersObj->SetStringField(TEXT("name_contains"), NameContains);
    }
    if (!PathContains.IsEmpty())
    {
        FiltersObj->SetStringField(TEXT("path_contains"), PathContains);
    }

    TArray<TSharedPtr<FJsonValue>> Warnings;
    if (AssetRegistry.IsLoadingAssets())
    {
        Warnings.Add(MakeShared<FJsonValueString>(
            TEXT("Asset Registry is still loading assets; results may be incomplete")
        ));
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetArrayField(TEXT("assets"), Assets);
    ResultObj->SetNumberField(TEXT("total_asset_count"), AssetDataList.Num());
    ResultObj->SetNumberField(TEXT("matched_asset_count"), MatchedAssetCount);
    ResultObj->SetNumberField(TEXT("returned_asset_count"), Assets.Num());
    ResultObj->SetBoolField(TEXT("truncated"), MatchedAssetCount > Assets.Num());
    ResultObj->SetBoolField(TEXT("asset_registry_loading"), AssetRegistry.IsLoadingAssets());
    ResultObj->SetObjectField(TEXT("filters"), FiltersObj);
    ResultObj->SetArrayField(TEXT("warnings"), Warnings);

    return ResultObj;
}

TSharedPtr<FJsonObject> FUnrealMCPEditorCommands::HandleAssetDependencies(const TSharedPtr<FJsonObject>& Params)
{
    return HandleAssetRelationshipQuery(Params, false);
}

TSharedPtr<FJsonObject> FUnrealMCPEditorCommands::HandleAssetReferencers(const TSharedPtr<FJsonObject>& Params)
{
    return HandleAssetRelationshipQuery(Params, true);
}

TSharedPtr<FJsonObject> FUnrealMCPEditorCommands::HandleListAutomationTests(const TSharedPtr<FJsonObject>& Params)
{
    double RequestedLimit = 200.0;
    if (Params->HasField(TEXT("limit")))
    {
        Params->TryGetNumberField(TEXT("limit"), RequestedLimit);
    }
    const int32 Limit = FMath::Clamp(static_cast<int32>(RequestedLimit), 1, 1000);

    FString Prefix;
    Params->TryGetStringField(TEXT("prefix"), Prefix);

    const TArray<FAutomationTestInfo> TestInfos = GetCurrentAutomationTests();

    TArray<TSharedPtr<FJsonValue>> Tests;
    Tests.Reserve(FMath::Min(Limit, TestInfos.Num()));

    int32 MatchedTestCount = 0;
    for (const FAutomationTestInfo& TestInfo : TestInfos)
    {
        if (!AutomationTestMatchesPrefix(TestInfo, Prefix))
        {
            continue;
        }

        ++MatchedTestCount;
        if (Tests.Num() < Limit)
        {
            Tests.Add(MakeShared<FJsonValueObject>(AutomationTestInfoToJson(TestInfo)));
        }
    }

    TSharedPtr<FJsonObject> FiltersObj = MakeShared<FJsonObject>();
    FiltersObj->SetNumberField(TEXT("limit"), Limit);
    if (!Prefix.IsEmpty())
    {
        FiltersObj->SetStringField(TEXT("prefix"), Prefix);
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetArrayField(TEXT("tests"), Tests);
    ResultObj->SetNumberField(TEXT("total_valid_test_count"), TestInfos.Num());
    ResultObj->SetNumberField(TEXT("matched_test_count"), MatchedTestCount);
    ResultObj->SetNumberField(TEXT("returned_test_count"), Tests.Num());
    ResultObj->SetBoolField(TEXT("truncated"), MatchedTestCount > Tests.Num());
    ResultObj->SetObjectField(TEXT("filters"), FiltersObj);

    return ResultObj;
}

TSharedPtr<FJsonObject> FUnrealMCPEditorCommands::HandleRunAutomationTest(const TSharedPtr<FJsonObject>& Params)
{
    FString RequestedTestName;
    if (!Params->TryGetStringField(TEXT("test_name"), RequestedTestName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'test_name' parameter"));
    }

    RequestedTestName = RequestedTestName.TrimStartAndEnd();
    if (RequestedTestName.IsEmpty())
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Invalid 'test_name' parameter: value cannot be empty"));
    }

    double RequestedTimeoutSeconds = 30.0;
    if (Params->HasField(TEXT("timeout_seconds")))
    {
        Params->TryGetNumberField(TEXT("timeout_seconds"), RequestedTimeoutSeconds);
    }
    const double TimeoutSeconds = FMath::Clamp(RequestedTimeoutSeconds, 1.0, 120.0);

    if (GIsSlowTask)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Automation tests cannot run while an editor slow task is active"));
    }

    if (GIsPlayInEditorWorld || (GEditor && GEditor->PlayWorld != nullptr))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Automation tests cannot run while Play In Editor is active"));
    }

    const TArray<FAutomationTestInfo> TestInfos = GetCurrentAutomationTests();
    const FAutomationTestInfo* MatchedTestInfo = FindAutomationTestByName(TestInfos, RequestedTestName);
    if (!MatchedTestInfo)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Automation test not found: %s"), *RequestedTestName));
    }

    FAutomationTestFramework& Framework = FAutomationTestFramework::Get();
    const FString TestCommand = MatchedTestInfo->GetTestName();
    const FString FullTestPath = MatchedTestInfo->GetFullTestPath();

    if (!Framework.ContainsTest(TestCommand))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Automation test is not runnable by command name: %s"), *TestCommand));
    }

    const double StartedAtSeconds = FPlatformTime::Seconds();
    Framework.DequeueAllCommands();
    Framework.StartTestByName(TestCommand, 0, FullTestPath);

    if (!GIsAutomationTesting)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Automation test did not start: %s"), *FullTestPath));
    }

    bool bTimedOut = false;
    while (!Framework.IsLatentCommandQueueEmpty())
    {
        Framework.ExecuteNetworkCommands();
        Framework.ExecuteLatentCommands();

        if (Framework.IsLatentCommandQueueEmpty())
        {
            break;
        }

        if ((FPlatformTime::Seconds() - StartedAtSeconds) > TimeoutSeconds)
        {
            bTimedOut = true;
            Framework.DequeueAllCommands();
            break;
        }

        FPlatformProcess::Sleep(0.01f);
    }

    FAutomationTestExecutionInfo ExecutionInfo;
    const bool bFrameworkSuccess = Framework.StopTest(ExecutionInfo);
    const bool bSuccessful = bFrameworkSuccess && !bTimedOut;

    TArray<TSharedPtr<FJsonValue>> Events;
    const TArray<FAutomationExecutionEntry>& Entries = ExecutionInfo.GetEntries();
    Events.Reserve(FMath::Min(MaxAutomationEventEntries, Entries.Num() + (bTimedOut ? 1 : 0)));

    if (bTimedOut)
    {
        TSharedPtr<FJsonObject> TimeoutEventObj = MakeShared<FJsonObject>();
        TimeoutEventObj->SetStringField(TEXT("type"), TEXT("error"));
        TimeoutEventObj->SetStringField(TEXT("message"), FString::Printf(TEXT("Automation test timed out after %.2f seconds"), TimeoutSeconds));
        TimeoutEventObj->SetStringField(TEXT("context"), TEXT("UEMCP"));
        TimeoutEventObj->SetStringField(TEXT("filename"), TEXT(""));
        TimeoutEventObj->SetNumberField(TEXT("line_number"), -1);
        TimeoutEventObj->SetStringField(TEXT("timestamp_utc"), FDateTime::UtcNow().ToIso8601());
        TimeoutEventObj->SetStringField(TEXT("formatted"), TimeoutEventObj->GetStringField(TEXT("message")));
        Events.Add(MakeShared<FJsonValueObject>(TimeoutEventObj));
    }

    for (const FAutomationExecutionEntry& Entry : Entries)
    {
        if (Events.Num() >= MaxAutomationEventEntries)
        {
            break;
        }
        Events.Add(MakeShared<FJsonValueObject>(AutomationExecutionEntryToJson(Entry)));
    }

    const double DurationSeconds = FPlatformTime::Seconds() - StartedAtSeconds;

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetObjectField(TEXT("test"), AutomationTestInfoToJson(*MatchedTestInfo));
    ResultObj->SetStringField(TEXT("status"), bTimedOut ? TEXT("timed_out") : (bSuccessful ? TEXT("passed") : TEXT("failed")));
    ResultObj->SetBoolField(TEXT("successful"), bSuccessful);
    ResultObj->SetBoolField(TEXT("timed_out"), bTimedOut);
    ResultObj->SetNumberField(TEXT("duration_seconds"), DurationSeconds);
    ResultObj->SetNumberField(TEXT("reported_duration_seconds"), ExecutionInfo.Duration);
    ResultObj->SetNumberField(TEXT("error_count"), ExecutionInfo.GetErrorTotal() + (bTimedOut ? 1 : 0));
    ResultObj->SetNumberField(TEXT("warning_count"), ExecutionInfo.GetWarningTotal());
    ResultObj->SetNumberField(TEXT("event_count"), Entries.Num() + (bTimedOut ? 1 : 0));
    ResultObj->SetBoolField(TEXT("events_truncated"), (Entries.Num() + (bTimedOut ? 1 : 0)) > Events.Num());
    ResultObj->SetArrayField(TEXT("events"), Events);

    return ResultObj;
}

TSharedPtr<FJsonObject> FUnrealMCPEditorCommands::HandleGetActorsInLevel(const TSharedPtr<FJsonObject>& Params)
{
    TArray<AActor*> AllActors;
    UGameplayStatics::GetAllActorsOfClass(GWorld, AActor::StaticClass(), AllActors);
    
    TArray<TSharedPtr<FJsonValue>> ActorArray;
    for (AActor* Actor : AllActors)
    {
        if (Actor)
        {
            ActorArray.Add(FUnrealMCPCommonUtils::ActorToJson(Actor));
        }
    }
    
    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetArrayField(TEXT("actors"), ActorArray);
    
    return ResultObj;
}

TSharedPtr<FJsonObject> FUnrealMCPEditorCommands::HandleFindActorsByName(const TSharedPtr<FJsonObject>& Params)
{
    FString Pattern;
    if (!Params->TryGetStringField(TEXT("pattern"), Pattern))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'pattern' parameter"));
    }
    
    TArray<AActor*> AllActors;
    UGameplayStatics::GetAllActorsOfClass(GWorld, AActor::StaticClass(), AllActors);
    
    TArray<TSharedPtr<FJsonValue>> MatchingActors;
    for (AActor* Actor : AllActors)
    {
        if (Actor && Actor->GetName().Contains(Pattern))
        {
            MatchingActors.Add(FUnrealMCPCommonUtils::ActorToJson(Actor));
        }
    }
    
    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetArrayField(TEXT("actors"), MatchingActors);
    
    return ResultObj;
}

TSharedPtr<FJsonObject> FUnrealMCPEditorCommands::HandleSpawnActor(const TSharedPtr<FJsonObject>& Params)
{
    // Get required parameters
    FString ActorType;
    if (!Params->TryGetStringField(TEXT("type"), ActorType))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'type' parameter"));
    }

    // Get actor name (required parameter)
    FString ActorName;
    if (!Params->TryGetStringField(TEXT("name"), ActorName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'name' parameter"));
    }

    // Get optional transform parameters
    FVector Location(0.0f, 0.0f, 0.0f);
    FRotator Rotation(0.0f, 0.0f, 0.0f);
    FVector Scale(1.0f, 1.0f, 1.0f);

    if (Params->HasField(TEXT("location")))
    {
        Location = FUnrealMCPCommonUtils::GetVectorFromJson(Params, TEXT("location"));
    }
    if (Params->HasField(TEXT("rotation")))
    {
        Rotation = FUnrealMCPCommonUtils::GetRotatorFromJson(Params, TEXT("rotation"));
    }
    if (Params->HasField(TEXT("scale")))
    {
        Scale = FUnrealMCPCommonUtils::GetVectorFromJson(Params, TEXT("scale"));
    }

    // Create the actor based on type
    AActor* NewActor = nullptr;
    UWorld* World = GEditor->GetEditorWorldContext().World();

    if (!World)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Failed to get editor world"));
    }

    // Check if an actor with this name already exists
    TArray<AActor*> AllActors;
    UGameplayStatics::GetAllActorsOfClass(World, AActor::StaticClass(), AllActors);
    for (AActor* Actor : AllActors)
    {
        if (Actor && Actor->GetName() == ActorName)
        {
            return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Actor with name '%s' already exists"), *ActorName));
        }
    }

    FActorSpawnParameters SpawnParams;
    SpawnParams.Name = *ActorName;

    if (ActorType == TEXT("StaticMeshActor"))
    {
        NewActor = World->SpawnActor<AStaticMeshActor>(AStaticMeshActor::StaticClass(), Location, Rotation, SpawnParams);
    }
    else if (ActorType == TEXT("PointLight"))
    {
        NewActor = World->SpawnActor<APointLight>(APointLight::StaticClass(), Location, Rotation, SpawnParams);
    }
    else if (ActorType == TEXT("SpotLight"))
    {
        NewActor = World->SpawnActor<ASpotLight>(ASpotLight::StaticClass(), Location, Rotation, SpawnParams);
    }
    else if (ActorType == TEXT("DirectionalLight"))
    {
        NewActor = World->SpawnActor<ADirectionalLight>(ADirectionalLight::StaticClass(), Location, Rotation, SpawnParams);
    }
    else if (ActorType == TEXT("CameraActor"))
    {
        NewActor = World->SpawnActor<ACameraActor>(ACameraActor::StaticClass(), Location, Rotation, SpawnParams);
    }
    else
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Unknown actor type: %s"), *ActorType));
    }

    if (NewActor)
    {
        // Set scale (since SpawnActor only takes location and rotation)
        FTransform Transform = NewActor->GetTransform();
        Transform.SetScale3D(Scale);
        NewActor->SetActorTransform(Transform);

        // Return the created actor's details
        return FUnrealMCPCommonUtils::ActorToJsonObject(NewActor, true);
    }

    return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Failed to create actor"));
}

TSharedPtr<FJsonObject> FUnrealMCPEditorCommands::HandleDeleteActor(const TSharedPtr<FJsonObject>& Params)
{
    FString ActorName;
    if (!Params->TryGetStringField(TEXT("name"), ActorName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'name' parameter"));
    }

    TArray<AActor*> AllActors;
    UGameplayStatics::GetAllActorsOfClass(GWorld, AActor::StaticClass(), AllActors);
    
    for (AActor* Actor : AllActors)
    {
        if (Actor && Actor->GetName() == ActorName)
        {
            // Store actor info before deletion for the response
            TSharedPtr<FJsonObject> ActorInfo = FUnrealMCPCommonUtils::ActorToJsonObject(Actor);
            
            // Delete the actor
            Actor->Destroy();
            
            TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
            ResultObj->SetObjectField(TEXT("deleted_actor"), ActorInfo);
            return ResultObj;
        }
    }
    
    return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Actor not found: %s"), *ActorName));
}

TSharedPtr<FJsonObject> FUnrealMCPEditorCommands::HandleSetActorTransform(const TSharedPtr<FJsonObject>& Params)
{
    // Get actor name
    FString ActorName;
    if (!Params->TryGetStringField(TEXT("name"), ActorName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'name' parameter"));
    }

    // Find the actor
    AActor* TargetActor = nullptr;
    TArray<AActor*> AllActors;
    UGameplayStatics::GetAllActorsOfClass(GWorld, AActor::StaticClass(), AllActors);
    
    for (AActor* Actor : AllActors)
    {
        if (Actor && Actor->GetName() == ActorName)
        {
            TargetActor = Actor;
            break;
        }
    }

    if (!TargetActor)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Actor not found: %s"), *ActorName));
    }

    // Get transform parameters
    FTransform NewTransform = TargetActor->GetTransform();

    if (Params->HasField(TEXT("location")))
    {
        NewTransform.SetLocation(FUnrealMCPCommonUtils::GetVectorFromJson(Params, TEXT("location")));
    }
    if (Params->HasField(TEXT("rotation")))
    {
        NewTransform.SetRotation(FQuat(FUnrealMCPCommonUtils::GetRotatorFromJson(Params, TEXT("rotation"))));
    }
    if (Params->HasField(TEXT("scale")))
    {
        NewTransform.SetScale3D(FUnrealMCPCommonUtils::GetVectorFromJson(Params, TEXT("scale")));
    }

    // Set the new transform
    TargetActor->SetActorTransform(NewTransform);

    // Return updated actor info
    return FUnrealMCPCommonUtils::ActorToJsonObject(TargetActor, true);
}

TSharedPtr<FJsonObject> FUnrealMCPEditorCommands::HandleGetActorProperties(const TSharedPtr<FJsonObject>& Params)
{
    // Get actor name
    FString ActorName;
    if (!Params->TryGetStringField(TEXT("name"), ActorName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'name' parameter"));
    }

    // Find the actor
    AActor* TargetActor = nullptr;
    TArray<AActor*> AllActors;
    UGameplayStatics::GetAllActorsOfClass(GWorld, AActor::StaticClass(), AllActors);
    
    for (AActor* Actor : AllActors)
    {
        if (Actor && Actor->GetName() == ActorName)
        {
            TargetActor = Actor;
            break;
        }
    }

    if (!TargetActor)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Actor not found: %s"), *ActorName));
    }

    // Always return detailed properties for this command
    return FUnrealMCPCommonUtils::ActorToJsonObject(TargetActor, true);
}

TSharedPtr<FJsonObject> FUnrealMCPEditorCommands::HandleSetActorProperty(const TSharedPtr<FJsonObject>& Params)
{
    // Get actor name
    FString ActorName;
    if (!Params->TryGetStringField(TEXT("name"), ActorName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'name' parameter"));
    }

    // Find the actor
    AActor* TargetActor = nullptr;
    TArray<AActor*> AllActors;
    UGameplayStatics::GetAllActorsOfClass(GWorld, AActor::StaticClass(), AllActors);
    
    for (AActor* Actor : AllActors)
    {
        if (Actor && Actor->GetName() == ActorName)
        {
            TargetActor = Actor;
            break;
        }
    }

    if (!TargetActor)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Actor not found: %s"), *ActorName));
    }

    // Get property name
    FString PropertyName;
    if (!Params->TryGetStringField(TEXT("property_name"), PropertyName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'property_name' parameter"));
    }

    // Get property value
    if (!Params->HasField(TEXT("property_value")))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'property_value' parameter"));
    }
    
    TSharedPtr<FJsonValue> PropertyValue = Params->Values.FindRef(TEXT("property_value"));
    
    // Set the property using our utility function
    FString ErrorMessage;
    if (FUnrealMCPCommonUtils::SetObjectProperty(TargetActor, PropertyName, PropertyValue, ErrorMessage))
    {
        // Property set successfully
        TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
        ResultObj->SetStringField(TEXT("actor"), ActorName);
        ResultObj->SetStringField(TEXT("property"), PropertyName);
        ResultObj->SetBoolField(TEXT("success"), true);
        
        // Also include the full actor details
        ResultObj->SetObjectField(TEXT("actor_details"), FUnrealMCPCommonUtils::ActorToJsonObject(TargetActor, true));
        return ResultObj;
    }
    else
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(ErrorMessage);
    }
}

TSharedPtr<FJsonObject> FUnrealMCPEditorCommands::HandleSpawnBlueprintActor(const TSharedPtr<FJsonObject>& Params)
{
    // Get required parameters
    FString BlueprintName;
    if (!Params->TryGetStringField(TEXT("blueprint_name"), BlueprintName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'blueprint_name' parameter"));
    }

    FString ActorName;
    if (!Params->TryGetStringField(TEXT("actor_name"), ActorName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'actor_name' parameter"));
    }

    // Find the blueprint
    if (BlueprintName.IsEmpty())
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Blueprint name is empty"));
    }

    FString Root      = TEXT("/Game/Blueprints/");
    FString AssetPath = Root + BlueprintName;

    if (!FPackageName::DoesPackageExist(AssetPath))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Blueprint '%s' not found – it must reside under /Game/Blueprints"), *BlueprintName));
    }

    UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *AssetPath);
    if (!Blueprint)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintName));
    }

    // Get transform parameters
    FVector Location(0.0f, 0.0f, 0.0f);
    FRotator Rotation(0.0f, 0.0f, 0.0f);
    FVector Scale(1.0f, 1.0f, 1.0f);

    if (Params->HasField(TEXT("location")))
    {
        Location = FUnrealMCPCommonUtils::GetVectorFromJson(Params, TEXT("location"));
    }
    if (Params->HasField(TEXT("rotation")))
    {
        Rotation = FUnrealMCPCommonUtils::GetRotatorFromJson(Params, TEXT("rotation"));
    }
    if (Params->HasField(TEXT("scale")))
    {
        Scale = FUnrealMCPCommonUtils::GetVectorFromJson(Params, TEXT("scale"));
    }

    // Spawn the actor
    UWorld* World = GEditor->GetEditorWorldContext().World();
    if (!World)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Failed to get editor world"));
    }

    FTransform SpawnTransform;
    SpawnTransform.SetLocation(Location);
    SpawnTransform.SetRotation(FQuat(Rotation));
    SpawnTransform.SetScale3D(Scale);

    FActorSpawnParameters SpawnParams;
    SpawnParams.Name = *ActorName;

    AActor* NewActor = World->SpawnActor<AActor>(Blueprint->GeneratedClass, SpawnTransform, SpawnParams);
    if (NewActor)
    {
        return FUnrealMCPCommonUtils::ActorToJsonObject(NewActor, true);
    }

    return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Failed to spawn blueprint actor"));
}

TSharedPtr<FJsonObject> FUnrealMCPEditorCommands::HandleFocusViewport(const TSharedPtr<FJsonObject>& Params)
{
    // Get target actor name if provided
    FString TargetActorName;
    bool HasTargetActor = Params->TryGetStringField(TEXT("target"), TargetActorName);

    // Get location if provided
    FVector Location(0.0f, 0.0f, 0.0f);
    bool HasLocation = false;
    if (Params->HasField(TEXT("location")))
    {
        Location = FUnrealMCPCommonUtils::GetVectorFromJson(Params, TEXT("location"));
        HasLocation = true;
    }

    // Get distance
    float Distance = 1000.0f;
    if (Params->HasField(TEXT("distance")))
    {
        Distance = Params->GetNumberField(TEXT("distance"));
    }

    // Get orientation if provided
    FRotator Orientation(0.0f, 0.0f, 0.0f);
    bool HasOrientation = false;
    if (Params->HasField(TEXT("orientation")))
    {
        Orientation = FUnrealMCPCommonUtils::GetRotatorFromJson(Params, TEXT("orientation"));
        HasOrientation = true;
    }

    // Get the active viewport
    FLevelEditorViewportClient* ViewportClient = (FLevelEditorViewportClient*)GEditor->GetActiveViewport()->GetClient();
    if (!ViewportClient)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Failed to get active viewport"));
    }

    // If we have a target actor, focus on it
    if (HasTargetActor)
    {
        // Find the actor
        AActor* TargetActor = nullptr;
        TArray<AActor*> AllActors;
        UGameplayStatics::GetAllActorsOfClass(GWorld, AActor::StaticClass(), AllActors);
        
        for (AActor* Actor : AllActors)
        {
            if (Actor && Actor->GetName() == TargetActorName)
            {
                TargetActor = Actor;
                break;
            }
        }

        if (!TargetActor)
        {
            return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Actor not found: %s"), *TargetActorName));
        }

        // Focus on the actor
        ViewportClient->SetViewLocation(TargetActor->GetActorLocation() - FVector(Distance, 0.0f, 0.0f));
    }
    // Otherwise use the provided location
    else if (HasLocation)
    {
        ViewportClient->SetViewLocation(Location - FVector(Distance, 0.0f, 0.0f));
    }
    else
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Either 'target' or 'location' must be provided"));
    }

    // Set orientation if provided
    if (HasOrientation)
    {
        ViewportClient->SetViewRotation(Orientation);
    }

    // Force viewport to redraw
    ViewportClient->Invalidate();

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetBoolField(TEXT("success"), true);
    return ResultObj;
}

TSharedPtr<FJsonObject> FUnrealMCPEditorCommands::HandleTakeScreenshot(const TSharedPtr<FJsonObject>& Params)
{
    // Get file path parameter
    FString FilePath;
    if (!Params->TryGetStringField(TEXT("filepath"), FilePath))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'filepath' parameter"));
    }
    
    // Ensure the file path has a proper extension
    if (!FilePath.EndsWith(TEXT(".png")))
    {
        FilePath += TEXT(".png");
    }

    // Get the active viewport
    if (GEditor && GEditor->GetActiveViewport())
    {
        FViewport* Viewport = GEditor->GetActiveViewport();
        TArray<FColor> Bitmap;
        FIntRect ViewportRect(0, 0, Viewport->GetSizeXY().X, Viewport->GetSizeXY().Y);
        
        if (Viewport->ReadPixels(Bitmap, FReadSurfaceDataFlags(), ViewportRect))
        {
            TArray<uint8> CompressedBitmap;
            FImageUtils::CompressImageArray(Viewport->GetSizeXY().X, Viewport->GetSizeXY().Y, Bitmap, CompressedBitmap);
            
            if (FFileHelper::SaveArrayToFile(CompressedBitmap, *FilePath))
            {
                TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
                ResultObj->SetStringField(TEXT("filepath"), FilePath);
                return ResultObj;
            }
        }
    }
    
    return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Failed to take screenshot"));
}
