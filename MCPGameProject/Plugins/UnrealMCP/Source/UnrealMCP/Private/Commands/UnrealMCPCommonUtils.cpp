#include "Commands/UnrealMCPCommonUtils.h"
#include "GameFramework/Actor.h"
#include "Engine/Blueprint.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "K2Node_Event.h"
#include "K2Node_CallFunction.h"
#include "K2Node_VariableGet.h"
#include "K2Node_VariableSet.h"
#include "K2Node_InputAction.h"
#include "K2Node_Self.h"
#include "EdGraphSchema_K2.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Components/StaticMeshComponent.h"
#include "Components/LightComponent.h"
#include "Components/PrimitiveComponent.h"
#include "Components/SceneComponent.h"
#include "UObject/UObjectIterator.h"
#include "Engine/Selection.h"
#include "EditorAssetLibrary.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "BlueprintNodeSpawner.h"
#include "BlueprintActionDatabase.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "UObject/UnrealType.h"

namespace
{
    constexpr int32 MaxActorPropertyEntries = 64;

    TSharedPtr<FJsonValue> PropertyValueToJson(
        FProperty* Property,
        const void* ValuePtr,
        UObject* Owner,
        const FActorPropertySerializationOptions& Options,
        int32 Depth
    );
    TSharedPtr<FJsonObject> ReflectedPropertyToJson(
        FProperty* Property,
        const void* ContainerPtr,
        UObject* Owner,
        const FActorPropertySerializationOptions& Options,
        int32 Depth
    );

    FString ExportPropertyValue(FProperty* Property, const void* ValuePtr, UObject* Owner)
    {
        FString ExportedValue;
        if (Property && ValuePtr)
        {
            Property->ExportTextItem_Direct(ExportedValue, ValuePtr, nullptr, Owner, PPF_None);
        }
        return ExportedValue;
    }

    bool IsPrivateOrProtectedProperty(FProperty* Property)
    {
        return Property && Property->HasAnyPropertyFlags(
            CPF_NativeAccessSpecifierPrivate | CPF_NativeAccessSpecifierProtected
        );
    }

    bool ShouldIncludeReflectedProperty(
        FProperty* Property,
        const FActorPropertySerializationOptions& Options,
        bool bNestedStructField
    )
    {
        if (!Property)
        {
            return false;
        }

        if (!bNestedStructField && !Options.NameContains.IsEmpty() && !Property->GetName().Contains(Options.NameContains))
        {
            return false;
        }

        if (!Options.bIncludePrivate && IsPrivateOrProtectedProperty(Property))
        {
            return false;
        }

        if (!Options.bIncludeTransient && Property->HasAnyPropertyFlags(CPF_Transient))
        {
            return false;
        }

        if (!Options.bIncludeConfig && Property->HasAnyPropertyFlags(CPF_Config))
        {
            return false;
        }

        const bool bEditableOrBlueprintVisible = Property->HasAnyPropertyFlags(CPF_Edit | CPF_BlueprintVisible);
        if (!Options.bIncludeNonEditable && !bNestedStructField && !bEditableOrBlueprintVisible)
        {
            return false;
        }

        return true;
    }

    TSharedPtr<FJsonObject> PropertyFlagsToJson(FProperty* Property)
    {
        TSharedPtr<FJsonObject> FlagsObject = MakeShared<FJsonObject>();
        if (!Property)
        {
            return FlagsObject;
        }

        FlagsObject->SetBoolField(TEXT("editable"), Property->HasAnyPropertyFlags(CPF_Edit));
        FlagsObject->SetBoolField(TEXT("blueprint_visible"), Property->HasAnyPropertyFlags(CPF_BlueprintVisible));
        FlagsObject->SetBoolField(TEXT("blueprint_read_only"), Property->HasAnyPropertyFlags(CPF_BlueprintReadOnly));
        FlagsObject->SetBoolField(TEXT("transient"), Property->HasAnyPropertyFlags(CPF_Transient));
        FlagsObject->SetBoolField(TEXT("config"), Property->HasAnyPropertyFlags(CPF_Config));
        FlagsObject->SetBoolField(TEXT("save_game"), Property->HasAnyPropertyFlags(CPF_SaveGame));
        FlagsObject->SetBoolField(TEXT("replicated"), Property->HasAnyPropertyFlags(CPF_Net));
        FlagsObject->SetBoolField(TEXT("private"), Property->HasAnyPropertyFlags(CPF_NativeAccessSpecifierPrivate));
        FlagsObject->SetBoolField(TEXT("protected"), Property->HasAnyPropertyFlags(CPF_NativeAccessSpecifierProtected));
        FlagsObject->SetBoolField(TEXT("public"), Property->HasAnyPropertyFlags(CPF_NativeAccessSpecifierPublic));
        FlagsObject->SetStringField(
            TEXT("flags_hex"),
            FString::Printf(TEXT("0x%016llx"), static_cast<unsigned long long>(Property->GetPropertyFlags()))
        );

        return FlagsObject;
    }

    TSharedPtr<FJsonObject> StructFieldsToJson(
        FStructProperty* StructProperty,
        const void* StructValuePtr,
        UObject* Owner,
        const FActorPropertySerializationOptions& Options,
        int32 Depth
    )
    {
        TSharedPtr<FJsonObject> FieldsObject = MakeShared<FJsonObject>();
        if (!StructProperty || !StructProperty->Struct || !StructValuePtr)
        {
            return FieldsObject;
        }

        if (Depth >= Options.MaxNestedStructDepth)
        {
            FieldsObject->SetBoolField(TEXT("_truncated"), true);
            FieldsObject->SetStringField(TEXT("_reason"), TEXT("max_struct_depth"));
            return FieldsObject;
        }

        for (TFieldIterator<FProperty> FieldIt(StructProperty->Struct, EFieldIteratorFlags::IncludeSuper); FieldIt; ++FieldIt)
        {
            FProperty* Field = *FieldIt;
            if (!ShouldIncludeReflectedProperty(Field, Options, true))
            {
                continue;
            }

            FieldsObject->SetObjectField(Field->GetName(), ReflectedPropertyToJson(Field, StructValuePtr, Owner, Options, Depth + 1));
        }

        return FieldsObject;
    }

    TSharedPtr<FJsonValue> ArrayValueToJson(
        FArrayProperty* ArrayProperty,
        const void* ValuePtr,
        UObject* Owner,
        const FActorPropertySerializationOptions& Options,
        int32 Depth
    )
    {
        if (!ArrayProperty || !ValuePtr)
        {
            return MakeShared<FJsonValueNull>();
        }

        FScriptArrayHelper ArrayHelper(ArrayProperty, ValuePtr);
        TArray<TSharedPtr<FJsonValue>> JsonValues;
        const int32 ItemCount = FMath::Min(ArrayHelper.Num(), Options.MaxCollectionItems);
        for (int32 Index = 0; Index < ItemCount; ++Index)
        {
            JsonValues.Add(PropertyValueToJson(ArrayProperty->Inner, ArrayHelper.GetRawPtr(Index), Owner, Options, Depth + 1));
        }

        return MakeShared<FJsonValueArray>(JsonValues);
    }

    TSharedPtr<FJsonValue> PropertyValueToJson(
        FProperty* Property,
        const void* ValuePtr,
        UObject* Owner,
        const FActorPropertySerializationOptions& Options,
        int32 Depth
    )
    {
        if (!Property || !ValuePtr)
        {
            return MakeShared<FJsonValueNull>();
        }

        if (FBoolProperty* BoolProperty = CastField<FBoolProperty>(Property))
        {
            return MakeShared<FJsonValueBoolean>(BoolProperty->GetPropertyValue(ValuePtr));
        }

        if (FEnumProperty* EnumProperty = CastField<FEnumProperty>(Property))
        {
            const int64 RawValue = EnumProperty->GetUnderlyingProperty()->GetSignedIntPropertyValue(ValuePtr);
            UEnum* Enum = EnumProperty->GetEnum();
            return MakeShared<FJsonValueString>(Enum ? Enum->GetNameStringByValue(RawValue) : FString::FromInt(RawValue));
        }

        if (FByteProperty* ByteProperty = CastField<FByteProperty>(Property))
        {
            const uint8 RawValue = ByteProperty->GetPropertyValue(ValuePtr);
            UEnum* Enum = ByteProperty->GetIntPropertyEnum();
            return MakeShared<FJsonValueString>(Enum ? Enum->GetNameStringByValue(RawValue) : FString::FromInt(RawValue));
        }

        if (FNumericProperty* NumericProperty = CastField<FNumericProperty>(Property))
        {
            if (NumericProperty->IsFloatingPoint())
            {
                return MakeShared<FJsonValueNumber>(NumericProperty->GetFloatingPointPropertyValue(ValuePtr));
            }

            if (NumericProperty->IsInteger())
            {
                return MakeShared<FJsonValueNumber>(static_cast<double>(NumericProperty->GetSignedIntPropertyValue(ValuePtr)));
            }
        }

        if (FStrProperty* StringProperty = CastField<FStrProperty>(Property))
        {
            return MakeShared<FJsonValueString>(StringProperty->GetPropertyValue(ValuePtr));
        }

        if (FNameProperty* NameProperty = CastField<FNameProperty>(Property))
        {
            return MakeShared<FJsonValueString>(NameProperty->GetPropertyValue(ValuePtr).ToString());
        }

        if (FTextProperty* TextProperty = CastField<FTextProperty>(Property))
        {
            return MakeShared<FJsonValueString>(TextProperty->GetPropertyValue(ValuePtr).ToString());
        }

        if (FObjectPropertyBase* ObjectProperty = CastField<FObjectPropertyBase>(Property))
        {
            UObject* ObjectValue = ObjectProperty->GetObjectPropertyValue(ValuePtr);
            if (ObjectValue)
            {
                TSharedPtr<FJsonObject> ObjectRef = MakeShared<FJsonObject>();
                ObjectRef->SetStringField(TEXT("name"), ObjectValue->GetName());
                ObjectRef->SetStringField(TEXT("class"), ObjectValue->GetClass() ? ObjectValue->GetClass()->GetName() : FString());
                if (Options.bIncludeObjectPaths)
                {
                    ObjectRef->SetStringField(TEXT("path"), ObjectValue->GetPathName());
                }
                return MakeShared<FJsonValueObject>(ObjectRef);
            }
            return MakeShared<FJsonValueNull>();
        }

        if (FArrayProperty* ArrayProperty = CastField<FArrayProperty>(Property))
        {
            return ArrayValueToJson(ArrayProperty, ValuePtr, Owner, Options, Depth);
        }

        return MakeShared<FJsonValueString>(ExportPropertyValue(Property, ValuePtr, Owner));
    }

    TSharedPtr<FJsonObject> ReflectedPropertyToJson(
        FProperty* Property,
        const void* ContainerPtr,
        UObject* Owner,
        const FActorPropertySerializationOptions& Options,
        int32 Depth
    )
    {
        TSharedPtr<FJsonObject> PropertyObject = MakeShared<FJsonObject>();
        if (!Property || !ContainerPtr)
        {
            return PropertyObject;
        }

        const void* ValuePtr = Property->ContainerPtrToValuePtr<const void>(ContainerPtr);

        PropertyObject->SetStringField(TEXT("type"), Property->GetCPPType());
        PropertyObject->SetStringField(TEXT("property_class"), Property->GetClass()->GetName());
        PropertyObject->SetObjectField(TEXT("flags"), PropertyFlagsToJson(Property));
        PropertyObject->SetField(TEXT("value"), PropertyValueToJson(Property, ValuePtr, Owner, Options, Depth));
        PropertyObject->SetStringField(TEXT("value_text"), ExportPropertyValue(Property, ValuePtr, Owner));

        if (FStructProperty* StructProperty = CastField<FStructProperty>(Property))
        {
            PropertyObject->SetStringField(
                TEXT("struct_type"),
                StructProperty->Struct ? StructProperty->Struct->GetName() : TEXT("")
            );
            PropertyObject->SetObjectField(TEXT("fields"), StructFieldsToJson(StructProperty, ValuePtr, Owner, Options, Depth));
        }
        else if (FArrayProperty* ArrayProperty = CastField<FArrayProperty>(Property))
        {
            FScriptArrayHelper ArrayHelper(ArrayProperty, ValuePtr);
            PropertyObject->SetNumberField(TEXT("count"), ArrayHelper.Num());
            PropertyObject->SetBoolField(TEXT("truncated"), ArrayHelper.Num() > Options.MaxCollectionItems);
        }

        return PropertyObject;
    }

    TSharedPtr<FJsonObject> ObjectPropertiesToJson(
        UObject* Object,
        const FActorPropertySerializationOptions& Options,
        TSharedPtr<FJsonObject>& OutMeta
    )
    {
        TSharedPtr<FJsonObject> PropertiesObject = MakeShared<FJsonObject>();
        OutMeta = MakeShared<FJsonObject>();
        if (!Object)
        {
            OutMeta->SetNumberField(TEXT("total_matching_properties"), 0);
            OutMeta->SetNumberField(TEXT("returned_properties"), 0);
            OutMeta->SetBoolField(TEXT("truncated"), false);
            return PropertiesObject;
        }

        int32 MatchingPropertyCount = 0;
        int32 ReturnedPropertyCount = 0;
        const int32 RequestedMaxProperties = Options.MaxProperties > 0 ? Options.MaxProperties : MaxActorPropertyEntries;
        const int32 MaxReturnedProperties = FMath::Clamp(RequestedMaxProperties, 1, 512);

        for (TFieldIterator<FProperty> PropertyIt(Object->GetClass(), EFieldIteratorFlags::IncludeSuper); PropertyIt; ++PropertyIt)
        {
            FProperty* Property = *PropertyIt;
            if (!ShouldIncludeReflectedProperty(Property, Options, false))
            {
                continue;
            }

            ++MatchingPropertyCount;
            if (ReturnedPropertyCount >= MaxReturnedProperties)
            {
                continue;
            }

            PropertiesObject->SetObjectField(Property->GetName(), ReflectedPropertyToJson(Property, Object, Object, Options, 0));
            ++ReturnedPropertyCount;
        }

        OutMeta->SetNumberField(TEXT("total_matching_properties"), MatchingPropertyCount);
        OutMeta->SetNumberField(TEXT("returned_properties"), ReturnedPropertyCount);
        OutMeta->SetBoolField(TEXT("truncated"), MatchingPropertyCount > ReturnedPropertyCount);
        OutMeta->SetNumberField(TEXT("property_limit"), MaxReturnedProperties);
        OutMeta->SetBoolField(TEXT("include_private"), Options.bIncludePrivate);
        OutMeta->SetBoolField(TEXT("include_transient"), Options.bIncludeTransient);
        OutMeta->SetBoolField(TEXT("include_config"), Options.bIncludeConfig);
        OutMeta->SetBoolField(TEXT("include_non_editable"), Options.bIncludeNonEditable);
        OutMeta->SetBoolField(TEXT("include_object_paths"), Options.bIncludeObjectPaths);
        if (!Options.NameContains.IsEmpty())
        {
            OutMeta->SetStringField(TEXT("name_contains"), Options.NameContains);
        }

        return PropertiesObject;
    }
}

// JSON Utilities
TSharedPtr<FJsonObject> FUnrealMCPCommonUtils::CreateErrorResponse(const FString& Message)
{
    TSharedPtr<FJsonObject> ResponseObject = MakeShared<FJsonObject>();
    ResponseObject->SetBoolField(TEXT("success"), false);
    ResponseObject->SetStringField(TEXT("error"), Message);
    return ResponseObject;
}

TSharedPtr<FJsonObject> FUnrealMCPCommonUtils::CreateSuccessResponse(const TSharedPtr<FJsonObject>& Data)
{
    TSharedPtr<FJsonObject> ResponseObject = MakeShared<FJsonObject>();
    ResponseObject->SetBoolField(TEXT("success"), true);
    
    if (Data.IsValid())
    {
        ResponseObject->SetObjectField(TEXT("data"), Data);
    }
    
    return ResponseObject;
}

void FUnrealMCPCommonUtils::GetIntArrayFromJson(const TSharedPtr<FJsonObject>& JsonObject, const FString& FieldName, TArray<int32>& OutArray)
{
    OutArray.Reset();
    
    if (!JsonObject->HasField(FieldName))
    {
        return;
    }
    
    const TArray<TSharedPtr<FJsonValue>>* JsonArray;
    if (JsonObject->TryGetArrayField(FieldName, JsonArray))
    {
        for (const TSharedPtr<FJsonValue>& Value : *JsonArray)
        {
            OutArray.Add((int32)Value->AsNumber());
        }
    }
}

void FUnrealMCPCommonUtils::GetFloatArrayFromJson(const TSharedPtr<FJsonObject>& JsonObject, const FString& FieldName, TArray<float>& OutArray)
{
    OutArray.Reset();
    
    if (!JsonObject->HasField(FieldName))
    {
        return;
    }
    
    const TArray<TSharedPtr<FJsonValue>>* JsonArray;
    if (JsonObject->TryGetArrayField(FieldName, JsonArray))
    {
        for (const TSharedPtr<FJsonValue>& Value : *JsonArray)
        {
            OutArray.Add((float)Value->AsNumber());
        }
    }
}

FVector2D FUnrealMCPCommonUtils::GetVector2DFromJson(const TSharedPtr<FJsonObject>& JsonObject, const FString& FieldName)
{
    FVector2D Result(0.0f, 0.0f);
    
    if (!JsonObject->HasField(FieldName))
    {
        return Result;
    }
    
    const TArray<TSharedPtr<FJsonValue>>* JsonArray;
    if (JsonObject->TryGetArrayField(FieldName, JsonArray) && JsonArray->Num() >= 2)
    {
        Result.X = (float)(*JsonArray)[0]->AsNumber();
        Result.Y = (float)(*JsonArray)[1]->AsNumber();
    }
    
    return Result;
}

FVector FUnrealMCPCommonUtils::GetVectorFromJson(const TSharedPtr<FJsonObject>& JsonObject, const FString& FieldName)
{
    FVector Result(0.0f, 0.0f, 0.0f);
    
    if (!JsonObject->HasField(FieldName))
    {
        return Result;
    }
    
    const TArray<TSharedPtr<FJsonValue>>* JsonArray;
    if (JsonObject->TryGetArrayField(FieldName, JsonArray) && JsonArray->Num() >= 3)
    {
        Result.X = (float)(*JsonArray)[0]->AsNumber();
        Result.Y = (float)(*JsonArray)[1]->AsNumber();
        Result.Z = (float)(*JsonArray)[2]->AsNumber();
    }
    
    return Result;
}

FRotator FUnrealMCPCommonUtils::GetRotatorFromJson(const TSharedPtr<FJsonObject>& JsonObject, const FString& FieldName)
{
    FRotator Result(0.0f, 0.0f, 0.0f);
    
    if (!JsonObject->HasField(FieldName))
    {
        return Result;
    }
    
    const TArray<TSharedPtr<FJsonValue>>* JsonArray;
    if (JsonObject->TryGetArrayField(FieldName, JsonArray) && JsonArray->Num() >= 3)
    {
        Result.Pitch = (float)(*JsonArray)[0]->AsNumber();
        Result.Yaw = (float)(*JsonArray)[1]->AsNumber();
        Result.Roll = (float)(*JsonArray)[2]->AsNumber();
    }
    
    return Result;
}

// Blueprint Utilities
UBlueprint* FUnrealMCPCommonUtils::FindBlueprint(const FString& BlueprintName)
{
    return FindBlueprintByName(BlueprintName);
}

UBlueprint* FUnrealMCPCommonUtils::FindBlueprintByName(const FString& BlueprintName)
{
    FString AssetPath = TEXT("/Game/Blueprints/") + BlueprintName;
    return LoadObject<UBlueprint>(nullptr, *AssetPath);
}

UEdGraph* FUnrealMCPCommonUtils::FindOrCreateEventGraph(UBlueprint* Blueprint)
{
    if (!Blueprint)
    {
        return nullptr;
    }
    
    // Try to find the event graph
    for (UEdGraph* Graph : Blueprint->UbergraphPages)
    {
        if (Graph->GetName().Contains(TEXT("EventGraph")))
        {
            return Graph;
        }
    }
    
    // Create a new event graph if none exists
    UEdGraph* NewGraph = FBlueprintEditorUtils::CreateNewGraph(Blueprint, FName(TEXT("EventGraph")), UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
    FBlueprintEditorUtils::AddUbergraphPage(Blueprint, NewGraph);
    return NewGraph;
}

// Blueprint node utilities
UK2Node_Event* FUnrealMCPCommonUtils::CreateEventNode(UEdGraph* Graph, const FString& EventName, const FVector2D& Position)
{
    if (!Graph)
    {
        return nullptr;
    }
    
    UBlueprint* Blueprint = FBlueprintEditorUtils::FindBlueprintForGraph(Graph);
    if (!Blueprint)
    {
        return nullptr;
    }
    
    // Check for existing event node with this exact name
    for (UEdGraphNode* Node : Graph->Nodes)
    {
        UK2Node_Event* EventNode = Cast<UK2Node_Event>(Node);
        if (EventNode && EventNode->EventReference.GetMemberName() == FName(*EventName))
        {
            UE_LOG(LogTemp, Display, TEXT("Using existing event node with name %s (ID: %s)"), 
                *EventName, *EventNode->NodeGuid.ToString());
            return EventNode;
        }
    }

    // No existing node found, create a new one
    UK2Node_Event* EventNode = nullptr;
    
    // Find the function to create the event
    UClass* BlueprintClass = Blueprint->GeneratedClass;
    UFunction* EventFunction = BlueprintClass->FindFunctionByName(FName(*EventName));
    
    if (EventFunction)
    {
        EventNode = NewObject<UK2Node_Event>(Graph);
        EventNode->EventReference.SetExternalMember(FName(*EventName), BlueprintClass);
        EventNode->NodePosX = Position.X;
        EventNode->NodePosY = Position.Y;
        Graph->AddNode(EventNode, true);
        EventNode->PostPlacedNewNode();
        EventNode->AllocateDefaultPins();
        UE_LOG(LogTemp, Display, TEXT("Created new event node with name %s (ID: %s)"), 
            *EventName, *EventNode->NodeGuid.ToString());
    }
    else
    {
        UE_LOG(LogTemp, Error, TEXT("Failed to find function for event name: %s"), *EventName);
    }
    
    return EventNode;
}

UK2Node_CallFunction* FUnrealMCPCommonUtils::CreateFunctionCallNode(UEdGraph* Graph, UFunction* Function, const FVector2D& Position)
{
    if (!Graph || !Function)
    {
        return nullptr;
    }
    
    UK2Node_CallFunction* FunctionNode = NewObject<UK2Node_CallFunction>(Graph);
    FunctionNode->SetFromFunction(Function);
    FunctionNode->NodePosX = Position.X;
    FunctionNode->NodePosY = Position.Y;
    Graph->AddNode(FunctionNode, true);
    FunctionNode->CreateNewGuid();
    FunctionNode->PostPlacedNewNode();
    FunctionNode->AllocateDefaultPins();
    
    return FunctionNode;
}

UK2Node_VariableGet* FUnrealMCPCommonUtils::CreateVariableGetNode(UEdGraph* Graph, UBlueprint* Blueprint, const FString& VariableName, const FVector2D& Position)
{
    if (!Graph || !Blueprint)
    {
        return nullptr;
    }
    
    UK2Node_VariableGet* VariableGetNode = NewObject<UK2Node_VariableGet>(Graph);
    
    FName VarName(*VariableName);
    FProperty* Property = FindFProperty<FProperty>(Blueprint->GeneratedClass, VarName);
    
    if (Property)
    {
        VariableGetNode->VariableReference.SetFromField<FProperty>(Property, false);
        VariableGetNode->NodePosX = Position.X;
        VariableGetNode->NodePosY = Position.Y;
        Graph->AddNode(VariableGetNode, true);
        VariableGetNode->PostPlacedNewNode();
        VariableGetNode->AllocateDefaultPins();
        
        return VariableGetNode;
    }
    
    return nullptr;
}

UK2Node_VariableSet* FUnrealMCPCommonUtils::CreateVariableSetNode(UEdGraph* Graph, UBlueprint* Blueprint, const FString& VariableName, const FVector2D& Position)
{
    if (!Graph || !Blueprint)
    {
        return nullptr;
    }
    
    UK2Node_VariableSet* VariableSetNode = NewObject<UK2Node_VariableSet>(Graph);
    
    FName VarName(*VariableName);
    FProperty* Property = FindFProperty<FProperty>(Blueprint->GeneratedClass, VarName);
    
    if (Property)
    {
        VariableSetNode->VariableReference.SetFromField<FProperty>(Property, false);
        VariableSetNode->NodePosX = Position.X;
        VariableSetNode->NodePosY = Position.Y;
        Graph->AddNode(VariableSetNode, true);
        VariableSetNode->PostPlacedNewNode();
        VariableSetNode->AllocateDefaultPins();
        
        return VariableSetNode;
    }
    
    return nullptr;
}

UK2Node_InputAction* FUnrealMCPCommonUtils::CreateInputActionNode(UEdGraph* Graph, const FString& ActionName, const FVector2D& Position)
{
    if (!Graph)
    {
        return nullptr;
    }
    
    UK2Node_InputAction* InputActionNode = NewObject<UK2Node_InputAction>(Graph);
    InputActionNode->InputActionName = FName(*ActionName);
    InputActionNode->NodePosX = Position.X;
    InputActionNode->NodePosY = Position.Y;
    Graph->AddNode(InputActionNode, true);
    InputActionNode->CreateNewGuid();
    InputActionNode->PostPlacedNewNode();
    InputActionNode->AllocateDefaultPins();
    
    return InputActionNode;
}

UK2Node_Self* FUnrealMCPCommonUtils::CreateSelfReferenceNode(UEdGraph* Graph, const FVector2D& Position)
{
    if (!Graph)
    {
        return nullptr;
    }
    
    UK2Node_Self* SelfNode = NewObject<UK2Node_Self>(Graph);
    SelfNode->NodePosX = Position.X;
    SelfNode->NodePosY = Position.Y;
    Graph->AddNode(SelfNode, true);
    SelfNode->CreateNewGuid();
    SelfNode->PostPlacedNewNode();
    SelfNode->AllocateDefaultPins();
    
    return SelfNode;
}

bool FUnrealMCPCommonUtils::ConnectGraphNodes(UEdGraph* Graph, UEdGraphNode* SourceNode, const FString& SourcePinName, 
                                           UEdGraphNode* TargetNode, const FString& TargetPinName)
{
    if (!Graph || !SourceNode || !TargetNode)
    {
        return false;
    }
    
    UEdGraphPin* SourcePin = FindPin(SourceNode, SourcePinName, EGPD_Output);
    UEdGraphPin* TargetPin = FindPin(TargetNode, TargetPinName, EGPD_Input);
    
    if (SourcePin && TargetPin)
    {
        SourcePin->MakeLinkTo(TargetPin);
        return true;
    }
    
    return false;
}

UEdGraphPin* FUnrealMCPCommonUtils::FindPin(UEdGraphNode* Node, const FString& PinName, EEdGraphPinDirection Direction)
{
    if (!Node)
    {
        return nullptr;
    }
    
    // Log all pins for debugging
    UE_LOG(LogTemp, Display, TEXT("FindPin: Looking for pin '%s' (Direction: %d) in node '%s'"), 
           *PinName, (int32)Direction, *Node->GetName());
    
    for (UEdGraphPin* Pin : Node->Pins)
    {
        UE_LOG(LogTemp, Display, TEXT("  - Available pin: '%s', Direction: %d, Category: %s"), 
               *Pin->PinName.ToString(), (int32)Pin->Direction, *Pin->PinType.PinCategory.ToString());
    }
    
    // First try exact match
    for (UEdGraphPin* Pin : Node->Pins)
    {
        if (Pin->PinName.ToString() == PinName && (Direction == EGPD_MAX || Pin->Direction == Direction))
        {
            UE_LOG(LogTemp, Display, TEXT("  - Found exact matching pin: '%s'"), *Pin->PinName.ToString());
            return Pin;
        }
    }
    
    // If no exact match and we're looking for a component reference, try case-insensitive match
    for (UEdGraphPin* Pin : Node->Pins)
    {
        if (Pin->PinName.ToString().Equals(PinName, ESearchCase::IgnoreCase) && 
            (Direction == EGPD_MAX || Pin->Direction == Direction))
        {
            UE_LOG(LogTemp, Display, TEXT("  - Found case-insensitive matching pin: '%s'"), *Pin->PinName.ToString());
            return Pin;
        }
    }
    
    // If we're looking for a component output and didn't find it by name, try to find the first data output pin
    if (Direction == EGPD_Output && Cast<UK2Node_VariableGet>(Node) != nullptr)
    {
        for (UEdGraphPin* Pin : Node->Pins)
        {
            if (Pin->Direction == EGPD_Output && Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec)
            {
                UE_LOG(LogTemp, Display, TEXT("  - Found fallback data output pin: '%s'"), *Pin->PinName.ToString());
                return Pin;
            }
        }
    }
    
    UE_LOG(LogTemp, Warning, TEXT("  - No matching pin found for '%s'"), *PinName);
    return nullptr;
}

// Actor utilities
TSharedPtr<FJsonValue> FUnrealMCPCommonUtils::ActorToJson(AActor* Actor)
{
    if (!Actor)
    {
        return MakeShared<FJsonValueNull>();
    }
    
    TSharedPtr<FJsonObject> ActorObject = MakeShared<FJsonObject>();
    ActorObject->SetStringField(TEXT("name"), Actor->GetName());
    ActorObject->SetStringField(TEXT("class"), Actor->GetClass()->GetName());
    
    FVector Location = Actor->GetActorLocation();
    TArray<TSharedPtr<FJsonValue>> LocationArray;
    LocationArray.Add(MakeShared<FJsonValueNumber>(Location.X));
    LocationArray.Add(MakeShared<FJsonValueNumber>(Location.Y));
    LocationArray.Add(MakeShared<FJsonValueNumber>(Location.Z));
    ActorObject->SetArrayField(TEXT("location"), LocationArray);
    
    FRotator Rotation = Actor->GetActorRotation();
    TArray<TSharedPtr<FJsonValue>> RotationArray;
    RotationArray.Add(MakeShared<FJsonValueNumber>(Rotation.Pitch));
    RotationArray.Add(MakeShared<FJsonValueNumber>(Rotation.Yaw));
    RotationArray.Add(MakeShared<FJsonValueNumber>(Rotation.Roll));
    ActorObject->SetArrayField(TEXT("rotation"), RotationArray);
    
    FVector Scale = Actor->GetActorScale3D();
    TArray<TSharedPtr<FJsonValue>> ScaleArray;
    ScaleArray.Add(MakeShared<FJsonValueNumber>(Scale.X));
    ScaleArray.Add(MakeShared<FJsonValueNumber>(Scale.Y));
    ScaleArray.Add(MakeShared<FJsonValueNumber>(Scale.Z));
    ActorObject->SetArrayField(TEXT("scale"), ScaleArray);
    
    return MakeShared<FJsonValueObject>(ActorObject);
}

TSharedPtr<FJsonObject> FUnrealMCPCommonUtils::ActorToJsonObject(
    AActor* Actor,
    bool bDetailed,
    const FActorPropertySerializationOptions& PropertyOptions
)
{
    if (!Actor)
    {
        return nullptr;
    }
    
    TSharedPtr<FJsonObject> ActorObject = MakeShared<FJsonObject>();
    ActorObject->SetStringField(TEXT("name"), Actor->GetName());
    ActorObject->SetStringField(TEXT("class"), Actor->GetClass()->GetName());
    
    FVector Location = Actor->GetActorLocation();
    TArray<TSharedPtr<FJsonValue>> LocationArray;
    LocationArray.Add(MakeShared<FJsonValueNumber>(Location.X));
    LocationArray.Add(MakeShared<FJsonValueNumber>(Location.Y));
    LocationArray.Add(MakeShared<FJsonValueNumber>(Location.Z));
    ActorObject->SetArrayField(TEXT("location"), LocationArray);
    
    FRotator Rotation = Actor->GetActorRotation();
    TArray<TSharedPtr<FJsonValue>> RotationArray;
    RotationArray.Add(MakeShared<FJsonValueNumber>(Rotation.Pitch));
    RotationArray.Add(MakeShared<FJsonValueNumber>(Rotation.Yaw));
    RotationArray.Add(MakeShared<FJsonValueNumber>(Rotation.Roll));
    ActorObject->SetArrayField(TEXT("rotation"), RotationArray);
    
    FVector Scale = Actor->GetActorScale3D();
    TArray<TSharedPtr<FJsonValue>> ScaleArray;
    ScaleArray.Add(MakeShared<FJsonValueNumber>(Scale.X));
    ScaleArray.Add(MakeShared<FJsonValueNumber>(Scale.Y));
    ScaleArray.Add(MakeShared<FJsonValueNumber>(Scale.Z));
    ActorObject->SetArrayField(TEXT("scale"), ScaleArray);

    if (bDetailed)
    {
        TSharedPtr<FJsonObject> PropertiesMeta;
        ActorObject->SetObjectField(TEXT("properties"), ObjectPropertiesToJson(Actor, PropertyOptions, PropertiesMeta));
        ActorObject->SetObjectField(TEXT("properties_meta"), PropertiesMeta);
    }
    
    return ActorObject;
}

UK2Node_Event* FUnrealMCPCommonUtils::FindExistingEventNode(UEdGraph* Graph, const FString& EventName)
{
    if (!Graph)
    {
        return nullptr;
    }

    // Look for existing event nodes
    for (UEdGraphNode* Node : Graph->Nodes)
    {
        UK2Node_Event* EventNode = Cast<UK2Node_Event>(Node);
        if (EventNode && EventNode->EventReference.GetMemberName() == FName(*EventName))
        {
            UE_LOG(LogTemp, Display, TEXT("Found existing event node with name: %s"), *EventName);
            return EventNode;
        }
    }

    return nullptr;
}

bool FUnrealMCPCommonUtils::SetObjectProperty(UObject* Object, const FString& PropertyName, 
                                     const TSharedPtr<FJsonValue>& Value, FString& OutErrorMessage)
{
    if (!Object)
    {
        OutErrorMessage = TEXT("Invalid object");
        return false;
    }

    FProperty* Property = Object->GetClass()->FindPropertyByName(*PropertyName);
    if (!Property)
    {
        OutErrorMessage = FString::Printf(TEXT("Property not found: %s"), *PropertyName);
        return false;
    }

    void* PropertyAddr = Property->ContainerPtrToValuePtr<void>(Object);
    
    // Handle different property types
    if (Property->IsA<FBoolProperty>())
    {
        ((FBoolProperty*)Property)->SetPropertyValue(PropertyAddr, Value->AsBool());
        return true;
    }
    else if (Property->IsA<FIntProperty>())
    {
        int32 IntValue = static_cast<int32>(Value->AsNumber());
        FIntProperty* IntProperty = CastField<FIntProperty>(Property);
        if (IntProperty)
        {
            IntProperty->SetPropertyValue_InContainer(Object, IntValue);
            return true;
        }
    }
    else if (Property->IsA<FFloatProperty>())
    {
        ((FFloatProperty*)Property)->SetPropertyValue(PropertyAddr, Value->AsNumber());
        return true;
    }
    else if (Property->IsA<FStrProperty>())
    {
        ((FStrProperty*)Property)->SetPropertyValue(PropertyAddr, Value->AsString());
        return true;
    }
    else if (Property->IsA<FByteProperty>())
    {
        FByteProperty* ByteProp = CastField<FByteProperty>(Property);
        UEnum* EnumDef = ByteProp ? ByteProp->GetIntPropertyEnum() : nullptr;
        
        // If this is a TEnumAsByte property (has associated enum)
        if (EnumDef)
        {
            // Handle numeric value
            if (Value->Type == EJson::Number)
            {
                uint8 ByteValue = static_cast<uint8>(Value->AsNumber());
                ByteProp->SetPropertyValue(PropertyAddr, ByteValue);
                
                UE_LOG(LogTemp, Display, TEXT("Setting enum property %s to numeric value: %d"), 
                      *PropertyName, ByteValue);
                return true;
            }
            // Handle string enum value
            else if (Value->Type == EJson::String)
            {
                FString EnumValueName = Value->AsString();
                
                // Try to convert numeric string to number first
                if (EnumValueName.IsNumeric())
                {
                    uint8 ByteValue = FCString::Atoi(*EnumValueName);
                    ByteProp->SetPropertyValue(PropertyAddr, ByteValue);
                    
                    UE_LOG(LogTemp, Display, TEXT("Setting enum property %s to numeric string value: %s -> %d"), 
                          *PropertyName, *EnumValueName, ByteValue);
                    return true;
                }
                
                // Handle qualified enum names (e.g., "Player0" or "EAutoReceiveInput::Player0")
                if (EnumValueName.Contains(TEXT("::")))
                {
                    EnumValueName.Split(TEXT("::"), nullptr, &EnumValueName);
                }
                
                int64 EnumValue = EnumDef->GetValueByNameString(EnumValueName);
                if (EnumValue == INDEX_NONE)
                {
                    // Try with full name as fallback
                    EnumValue = EnumDef->GetValueByNameString(Value->AsString());
                }
                
                if (EnumValue != INDEX_NONE)
                {
                    ByteProp->SetPropertyValue(PropertyAddr, static_cast<uint8>(EnumValue));
                    
                    UE_LOG(LogTemp, Display, TEXT("Setting enum property %s to name value: %s -> %lld"), 
                          *PropertyName, *EnumValueName, EnumValue);
                    return true;
                }
                else
                {
                    // Log all possible enum values for debugging
                    UE_LOG(LogTemp, Warning, TEXT("Could not find enum value for '%s'. Available options:"), *EnumValueName);
                    for (int32 i = 0; i < EnumDef->NumEnums(); i++)
                    {
                        UE_LOG(LogTemp, Warning, TEXT("  - %s (value: %d)"), 
                               *EnumDef->GetNameStringByIndex(i), EnumDef->GetValueByIndex(i));
                    }
                    
                    OutErrorMessage = FString::Printf(TEXT("Could not find enum value for '%s'"), *EnumValueName);
                    return false;
                }
            }
        }
        else
        {
            // Regular byte property
            uint8 ByteValue = static_cast<uint8>(Value->AsNumber());
            ByteProp->SetPropertyValue(PropertyAddr, ByteValue);
            return true;
        }
    }
    else if (Property->IsA<FEnumProperty>())
    {
        FEnumProperty* EnumProp = CastField<FEnumProperty>(Property);
        UEnum* EnumDef = EnumProp ? EnumProp->GetEnum() : nullptr;
        FNumericProperty* UnderlyingNumericProp = EnumProp ? EnumProp->GetUnderlyingProperty() : nullptr;
        
        if (EnumDef && UnderlyingNumericProp)
        {
            // Handle numeric value
            if (Value->Type == EJson::Number)
            {
                int64 EnumValue = static_cast<int64>(Value->AsNumber());
                UnderlyingNumericProp->SetIntPropertyValue(PropertyAddr, EnumValue);
                
                UE_LOG(LogTemp, Display, TEXT("Setting enum property %s to numeric value: %lld"), 
                      *PropertyName, EnumValue);
                return true;
            }
            // Handle string enum value
            else if (Value->Type == EJson::String)
            {
                FString EnumValueName = Value->AsString();
                
                // Try to convert numeric string to number first
                if (EnumValueName.IsNumeric())
                {
                    int64 EnumValue = FCString::Atoi64(*EnumValueName);
                    UnderlyingNumericProp->SetIntPropertyValue(PropertyAddr, EnumValue);
                    
                    UE_LOG(LogTemp, Display, TEXT("Setting enum property %s to numeric string value: %s -> %lld"), 
                          *PropertyName, *EnumValueName, EnumValue);
                    return true;
                }
                
                // Handle qualified enum names
                if (EnumValueName.Contains(TEXT("::")))
                {
                    EnumValueName.Split(TEXT("::"), nullptr, &EnumValueName);
                }
                
                int64 EnumValue = EnumDef->GetValueByNameString(EnumValueName);
                if (EnumValue == INDEX_NONE)
                {
                    // Try with full name as fallback
                    EnumValue = EnumDef->GetValueByNameString(Value->AsString());
                }
                
                if (EnumValue != INDEX_NONE)
                {
                    UnderlyingNumericProp->SetIntPropertyValue(PropertyAddr, EnumValue);
                    
                    UE_LOG(LogTemp, Display, TEXT("Setting enum property %s to name value: %s -> %lld"), 
                          *PropertyName, *EnumValueName, EnumValue);
                    return true;
                }
                else
                {
                    // Log all possible enum values for debugging
                    UE_LOG(LogTemp, Warning, TEXT("Could not find enum value for '%s'. Available options:"), *EnumValueName);
                    for (int32 i = 0; i < EnumDef->NumEnums(); i++)
                    {
                        UE_LOG(LogTemp, Warning, TEXT("  - %s (value: %d)"), 
                               *EnumDef->GetNameStringByIndex(i), EnumDef->GetValueByIndex(i));
                    }
                    
                    OutErrorMessage = FString::Printf(TEXT("Could not find enum value for '%s'"), *EnumValueName);
                    return false;
                }
            }
        }
    }
    
    OutErrorMessage = FString::Printf(TEXT("Unsupported property type: %s for property %s"), 
                                    *Property->GetClass()->GetName(), *PropertyName);
    return false;
}
