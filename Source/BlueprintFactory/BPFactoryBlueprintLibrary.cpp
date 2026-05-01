#include "BPFactoryBlueprintLibrary.h"
#include "Animation/AnimBlueprint.h"
#include "Animation/AnimationAsset.h"
#include "AnimGraphNode_Root.h"
#include "AnimGraphNode_SequencePlayer.h"
#include "AnimGraphNode_Slot.h"
#include "AnimGraphNode_StateResult.h"
#include "AnimGraphNode_StateMachine.h"
#include "AnimGraphNode_StateMachineBase.h"
#include "AnimGraphNode_TransitionResult.h"
#include "AnimStateEntryNode.h"
#include "AnimStateNode.h"
#include "AnimStateTransitionNode.h"
#include "AnimationGraphSchema.h"
#include "AnimationStateMachineGraph.h"
#include "AnimationStateMachineSchema.h"
#include "Landscape.h"
#include "LandscapeProxy.h"
#include "LandscapeInfo.h"
#include "Engine/World.h"
#include "Editor.h"
#include "Misc/FileHelper.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "K2Node_CallFunction.h"
#include "K2Node_CommutativeAssociativeBinaryOperator.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_FunctionResult.h"
#include "K2Node_VariableGet.h"
#include "Kismet/KismetMathLibrary.h"
#include "Engine/SimpleConstructionScript.h"
#include "Engine/SCS_Node.h"
#include "Dom/JsonObject.h"
#include "Misc/PackageName.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

#if WITH_UNLUA
#include "UnLuaInterface.h"
#endif

namespace
{
	constexpr float DefaultCrossfade = 0.18f;

	enum class EAnimTransitionConditionType : uint8
	{
		Bool,
		NumericComparison
	};

	enum class EAnimTransitionComparisonOp : uint8
	{
		Equal,
		NotEqual,
		Greater,
		GreaterOrEqual,
		Less,
		LessOrEqual
	};

	struct FAnimStateMachineVariableSpec
	{
		FName Name;
		FString Type = TEXT("bool");
	};

	struct FAnimStateMachineStateSpec
	{
		FName StateName;
		FString AnimationAssetPath;
		FVector2f NodePosition = FVector2f::ZeroVector;
	};

	struct FAnimStateMachineTransitionSpec
	{
		FName FromState;
		FName ToState;
		FName ConditionVariable;
		bool bExpectedValue = true;
		int32 Priority = 0;
		float CrossfadeDuration = DefaultCrossfade;
		EAnimTransitionConditionType ConditionType = EAnimTransitionConditionType::Bool;
		EAnimTransitionComparisonOp ComparisonOp = EAnimTransitionComparisonOp::Equal;
		double NumericValue = 0.0;
	};

	struct FAnimStateMachineDefinition
	{
		FName StateMachineName = TEXT("StateMachine");
		FName EntryState;
		FName OutputSlotName;
		bool bAlwaysUpdateSourcePose = false;
		TArray<FAnimStateMachineVariableSpec> Variables;
		TArray<FAnimStateMachineStateSpec> States;
		TArray<FAnimStateMachineTransitionSpec> Transitions;
	};

	struct FAnimAssetOverrideSpec
	{
		FName StateName;
		FString AnimationAssetPath;
	};

	FString ToObjectPath(const FString& AssetPath)
	{
		if (AssetPath.IsEmpty() || AssetPath.Contains(TEXT(".")))
		{
			return AssetPath;
		}

		const FString AssetName = FPackageName::GetLongPackageAssetName(AssetPath);
		return AssetName.IsEmpty() ? AssetPath : FString::Printf(TEXT("%s.%s"), *AssetPath, *AssetName);
	}

	template <typename TObjectType>
	TObjectType* LoadEditorAsset(const FString& AssetPath)
	{
		if (AssetPath.IsEmpty())
		{
			return nullptr;
		}

		return LoadObject<TObjectType>(nullptr, *ToObjectPath(AssetPath));
	}

	void CompileBlueprint(UBlueprint* Blueprint)
	{
		if (Blueprint)
		{
			FKismetEditorUtilities::CompileBlueprint(Blueprint);
		}
	}

	bool HasMemberVariable(const UBlueprint* Blueprint, const FName VarName)
	{
		if (!Blueprint)
		{
			return false;
		}

		for (const FBPVariableDescription& VarDesc : Blueprint->NewVariables)
		{
			if (VarDesc.VarName == VarName)
			{
				return true;
			}
		}

		if (Blueprint->SkeletonGeneratedClass && FindFProperty<FProperty>(Blueprint->SkeletonGeneratedClass, VarName))
		{
			return true;
		}

		if (Blueprint->GeneratedClass && FindFProperty<FProperty>(Blueprint->GeneratedClass, VarName))
		{
			return true;
		}

		return false;
	}

	FEdGraphPinType MakeBoolPinType()
	{
		FEdGraphPinType PinType;
		PinType.PinCategory = UEdGraphSchema_K2::PC_Boolean;
		return PinType;
	}

	FEdGraphPinType MakeFloatPinType()
	{
		FEdGraphPinType PinType;
		PinType.PinCategory = UEdGraphSchema_K2::PC_Real;
		PinType.PinSubCategory = UEdGraphSchema_K2::PC_Float;
		return PinType;
	}

	FEdGraphPinType MakeDoublePinType()
	{
		FEdGraphPinType PinType;
		PinType.PinCategory = UEdGraphSchema_K2::PC_Real;
		PinType.PinSubCategory = UEdGraphSchema_K2::PC_Double;
		return PinType;
	}

	bool TryMakePinTypeFromString(const FString& TypeName, FEdGraphPinType& OutPinType)
	{
		const FString Normalized = TypeName.TrimStartAndEnd().ToLower();
		if (Normalized.IsEmpty() || Normalized == TEXT("bool") || Normalized == TEXT("boolean"))
		{
			OutPinType = MakeBoolPinType();
			return true;
		}

		if (Normalized == TEXT("float") || Normalized == TEXT("number"))
		{
			OutPinType = MakeFloatPinType();
			return true;
		}

		if (Normalized == TEXT("double") || Normalized == TEXT("real"))
		{
			OutPinType = MakeDoublePinType();
			return true;
		}

		return false;
	}

	FString ExportPinTypeName(const FEdGraphPinType& PinType)
	{
		if (PinType.PinCategory == UEdGraphSchema_K2::PC_Boolean)
		{
			return TEXT("bool");
		}

		if (PinType.PinCategory == UEdGraphSchema_K2::PC_Real)
		{
			if (PinType.PinSubCategory == UEdGraphSchema_K2::PC_Double)
			{
				return TEXT("double");
			}
			return TEXT("float");
		}

		return FString();
	}

	bool TryParseTransitionComparisonOp(const FString& OperatorName, EAnimTransitionComparisonOp& OutOp)
	{
		const FString Normalized = OperatorName.TrimStartAndEnd().ToLower();
		if (Normalized.IsEmpty() || Normalized == TEXT("equal") || Normalized == TEXT("equals"))
		{
			OutOp = EAnimTransitionComparisonOp::Equal;
			return true;
		}
		if (Normalized == TEXT("notequal") || Normalized == TEXT("not_equal") || Normalized == TEXT("!="))
		{
			OutOp = EAnimTransitionComparisonOp::NotEqual;
			return true;
		}
		if (Normalized == TEXT("greater") || Normalized == TEXT(">"))
		{
			OutOp = EAnimTransitionComparisonOp::Greater;
			return true;
		}
		if (Normalized == TEXT("greaterorequal") || Normalized == TEXT("greater_or_equal") || Normalized == TEXT(">="))
		{
			OutOp = EAnimTransitionComparisonOp::GreaterOrEqual;
			return true;
		}
		if (Normalized == TEXT("less") || Normalized == TEXT("<"))
		{
			OutOp = EAnimTransitionComparisonOp::Less;
			return true;
		}
		if (Normalized == TEXT("lessorequal") || Normalized == TEXT("less_or_equal") || Normalized == TEXT("<="))
		{
			OutOp = EAnimTransitionComparisonOp::LessOrEqual;
			return true;
		}
		return false;
	}

	FString TransitionComparisonOpToString(const EAnimTransitionComparisonOp ComparisonOp)
	{
		switch (ComparisonOp)
		{
		case EAnimTransitionComparisonOp::Equal:
			return TEXT("Equal");
		case EAnimTransitionComparisonOp::NotEqual:
			return TEXT("NotEqual");
		case EAnimTransitionComparisonOp::Greater:
			return TEXT("Greater");
		case EAnimTransitionComparisonOp::GreaterOrEqual:
			return TEXT("GreaterOrEqual");
		case EAnimTransitionComparisonOp::Less:
			return TEXT("Less");
		case EAnimTransitionComparisonOp::LessOrEqual:
			return TEXT("LessOrEqual");
		default:
			return TEXT("Equal");
		}
	}

	void EnsureMemberVariable(UBlueprint* Blueprint, const FName VarName, const FEdGraphPinType& PinType)
	{
		if (!Blueprint || HasMemberVariable(Blueprint, VarName))
		{
			return;
		}

		FBlueprintEditorUtils::AddMemberVariable(Blueprint, VarName, PinType);
	}

	void EnsureDeclaredVariables(UBlueprint* Blueprint, const TArray<FAnimStateMachineVariableSpec>& VariableSpecs)
	{
		if (!Blueprint)
		{
			return;
		}

		for (const FAnimStateMachineVariableSpec& VarSpec : VariableSpecs)
		{
			if (VarSpec.Name.IsNone())
			{
				continue;
			}

			FEdGraphPinType PinType;
			if (!TryMakePinTypeFromString(VarSpec.Type, PinType))
			{
				UE_LOG(LogTemp, Warning, TEXT("[BPFactory] Unsupported anim variable type for %s: %s"), *VarSpec.Name.ToString(), *VarSpec.Type);
				continue;
			}

			EnsureMemberVariable(Blueprint, VarSpec.Name, PinType);
		}
	}

	void EnsureQuadrupedVariables(UBlueprint* Blueprint)
	{
		EnsureMemberVariable(Blueprint, TEXT("Speed"), MakeFloatPinType());
		EnsureMemberVariable(Blueprint, TEXT("Direction"), MakeFloatPinType());
		EnsureMemberVariable(Blueprint, TEXT("MoveAlpha"), MakeFloatPinType());
		EnsureMemberVariable(Blueprint, TEXT("IdleAlpha"), MakeFloatPinType());
		EnsureMemberVariable(Blueprint, TEXT("bIsMoving"), MakeBoolPinType());
		EnsureMemberVariable(Blueprint, TEXT("bIsGrounded"), MakeBoolPinType());
		EnsureMemberVariable(Blueprint, TEXT("bIsAirborne"), MakeBoolPinType());
		EnsureMemberVariable(Blueprint, TEXT("bIsSitting"), MakeBoolPinType());
		EnsureMemberVariable(Blueprint, TEXT("bIsSleeping"), MakeBoolPinType());
		EnsureMemberVariable(Blueprint, TEXT("bIsSniffing"), MakeBoolPinType());
		EnsureMemberVariable(Blueprint, TEXT("bIsAlert"), MakeBoolPinType());
	}

	bool TryReadVector2f(const TSharedPtr<FJsonObject>& JsonObject, const FString& FieldName, FVector2f& OutVector)
	{
		if (!JsonObject.IsValid())
		{
			return false;
		}

		const TSharedPtr<FJsonObject>* PositionObject = nullptr;
		if (JsonObject->TryGetObjectField(FieldName, PositionObject) && PositionObject && PositionObject->IsValid())
		{
			double X = 0.0;
			double Y = 0.0;
			if ((*PositionObject)->TryGetNumberField(TEXT("X"), X) && (*PositionObject)->TryGetNumberField(TEXT("Y"), Y))
			{
				OutVector = FVector2f(static_cast<float>(X), static_cast<float>(Y));
				return true;
			}
		}

		const TArray<TSharedPtr<FJsonValue>>* PositionArray = nullptr;
		if (JsonObject->TryGetArrayField(FieldName, PositionArray) && PositionArray && PositionArray->Num() >= 2)
		{
			const double X = (*PositionArray)[0].IsValid() ? (*PositionArray)[0]->AsNumber() : 0.0;
			const double Y = (*PositionArray)[1].IsValid() ? (*PositionArray)[1]->AsNumber() : 0.0;
			OutVector = FVector2f(static_cast<float>(X), static_cast<float>(Y));
			return true;
		}

		return false;
	}

	bool ParseStateMachineDefinitionJson(const FString& DefinitionJson, FAnimStateMachineDefinition& OutDefinition)
	{
		if (DefinitionJson.IsEmpty())
		{
			return false;
		}

		TSharedPtr<FJsonObject> RootObject;
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(DefinitionJson);
		if (!FJsonSerializer::Deserialize(Reader, RootObject) || !RootObject.IsValid())
		{
			return false;
		}

		OutDefinition = FAnimStateMachineDefinition();

		FString MachineName;
		RootObject->TryGetStringField(TEXT("StateMachineName"), MachineName);
		if (!MachineName.IsEmpty())
		{
			OutDefinition.StateMachineName = FName(*MachineName);
		}

		FString EntryState;
		RootObject->TryGetStringField(TEXT("EntryState"), EntryState);
		if (!EntryState.IsEmpty())
		{
			OutDefinition.EntryState = FName(*EntryState);
		}

		FString OutputSlotName;
		RootObject->TryGetStringField(TEXT("OutputSlotName"), OutputSlotName);
		if (!OutputSlotName.IsEmpty())
		{
			OutDefinition.OutputSlotName = FName(*OutputSlotName);
		}

		bool bAlwaysUpdateSourcePose = false;
		if (RootObject->TryGetBoolField(TEXT("AlwaysUpdateSourcePose"), bAlwaysUpdateSourcePose))
		{
			OutDefinition.bAlwaysUpdateSourcePose = bAlwaysUpdateSourcePose;
		}

		if (const TArray<TSharedPtr<FJsonValue>>* VariablesArray = nullptr; RootObject->TryGetArrayField(TEXT("Variables"), VariablesArray) && VariablesArray)
		{
			for (const TSharedPtr<FJsonValue>& Value : *VariablesArray)
			{
				const TSharedPtr<FJsonObject> VarObject = Value.IsValid() ? Value->AsObject() : nullptr;
				if (!VarObject.IsValid())
				{
					continue;
				}

				FString Name;
				VarObject->TryGetStringField(TEXT("Name"), Name);
				if (Name.IsEmpty())
				{
					continue;
				}

				FAnimStateMachineVariableSpec VarSpec;
				VarSpec.Name = FName(*Name);
				VarObject->TryGetStringField(TEXT("Type"), VarSpec.Type);
				if (VarSpec.Type.IsEmpty())
				{
					VarSpec.Type = TEXT("bool");
				}
				OutDefinition.Variables.Add(VarSpec);
			}
		}

		if (const TArray<TSharedPtr<FJsonValue>>* StatesArray = nullptr; RootObject->TryGetArrayField(TEXT("States"), StatesArray) && StatesArray)
		{
			for (const TSharedPtr<FJsonValue>& Value : *StatesArray)
			{
				const TSharedPtr<FJsonObject> StateObject = Value.IsValid() ? Value->AsObject() : nullptr;
				if (!StateObject.IsValid())
				{
					continue;
				}

				FString Name;
				StateObject->TryGetStringField(TEXT("Name"), Name);
				if (Name.IsEmpty())
				{
					continue;
				}

				FAnimStateMachineStateSpec StateSpec;
				StateSpec.StateName = FName(*Name);
				StateObject->TryGetStringField(TEXT("AnimationAsset"), StateSpec.AnimationAssetPath);
				TryReadVector2f(StateObject, TEXT("NodePosition"), StateSpec.NodePosition);
				OutDefinition.States.Add(StateSpec);
			}
		}

		if (const TArray<TSharedPtr<FJsonValue>>* TransitionArray = nullptr; RootObject->TryGetArrayField(TEXT("Transitions"), TransitionArray) && TransitionArray)
		{
			double DefaultCrossfadeFromRoot = DefaultCrossfade;
			RootObject->TryGetNumberField(TEXT("DefaultCrossfade"), DefaultCrossfadeFromRoot);
			for (const TSharedPtr<FJsonValue>& Value : *TransitionArray)
			{
				const TSharedPtr<FJsonObject> TransitionObject = Value.IsValid() ? Value->AsObject() : nullptr;
				if (!TransitionObject.IsValid())
				{
					continue;
				}

				FString FromState;
				FString ToState;
				TransitionObject->TryGetStringField(TEXT("From"), FromState);
				TransitionObject->TryGetStringField(TEXT("To"), ToState);
				if (FromState.IsEmpty() || ToState.IsEmpty())
				{
					continue;
				}

				FAnimStateMachineTransitionSpec TransitionSpec;
				TransitionSpec.FromState = FName(*FromState);
				TransitionSpec.ToState = FName(*ToState);
				double Priority = 0.0;
				TransitionObject->TryGetNumberField(TEXT("Priority"), Priority);
				TransitionSpec.Priority = static_cast<int32>(Priority);
				double CrossfadeDuration = DefaultCrossfadeFromRoot;
				TransitionObject->TryGetNumberField(TEXT("CrossfadeDuration"), CrossfadeDuration);
				TransitionSpec.CrossfadeDuration = static_cast<float>(CrossfadeDuration);

				const TSharedPtr<FJsonObject>* ConditionObject = nullptr;
				if (TransitionObject->TryGetObjectField(TEXT("Condition"), ConditionObject) && ConditionObject && ConditionObject->IsValid())
				{
					FString Variable;
					(*ConditionObject)->TryGetStringField(TEXT("Variable"), Variable);
					if (!Variable.IsEmpty())
					{
						TransitionSpec.ConditionVariable = FName(*Variable);
					}

					FString OperatorName;
					(*ConditionObject)->TryGetStringField(TEXT("Operator"), OperatorName);
					const FString NormalizedOperator = OperatorName.TrimStartAndEnd().ToLower();

					double NumericValue = 0.0;
					if (!NormalizedOperator.IsEmpty()
						&& NormalizedOperator != TEXT("notbool")
						&& NormalizedOperator != TEXT("not_bool")
						&& (*ConditionObject)->TryGetNumberField(TEXT("Value"), NumericValue))
					{
						TransitionSpec.ConditionType = EAnimTransitionConditionType::NumericComparison;
						TransitionSpec.NumericValue = NumericValue;
						if (!TryParseTransitionComparisonOp(OperatorName, TransitionSpec.ComparisonOp))
						{
							TransitionSpec.ComparisonOp = EAnimTransitionComparisonOp::Equal;
						}
					}
					else
					{
						bool bExpectedValue = true;
						if ((*ConditionObject)->TryGetBoolField(TEXT("Value"), bExpectedValue))
						{
							TransitionSpec.ConditionType = EAnimTransitionConditionType::Bool;
							TransitionSpec.bExpectedValue = bExpectedValue;
							if (NormalizedOperator == TEXT("notbool") || NormalizedOperator == TEXT("not_bool"))
							{
								TransitionSpec.bExpectedValue = false;
							}
						}
						else if ((*ConditionObject)->TryGetNumberField(TEXT("Value"), NumericValue))
						{
							TransitionSpec.ConditionType = EAnimTransitionConditionType::NumericComparison;
							TransitionSpec.NumericValue = NumericValue;
							if (!TryParseTransitionComparisonOp(OperatorName, TransitionSpec.ComparisonOp))
							{
								TransitionSpec.ComparisonOp = EAnimTransitionComparisonOp::Equal;
							}
						}
					}
				}
				else
				{
					FString Variable;
					TransitionObject->TryGetStringField(TEXT("ConditionVariable"), Variable);
					if (!Variable.IsEmpty())
					{
						TransitionSpec.ConditionVariable = FName(*Variable);
					}
					bool bExpectedValue = true;
					if (TransitionObject->TryGetBoolField(TEXT("ExpectedValue"), bExpectedValue))
					{
						TransitionSpec.bExpectedValue = bExpectedValue;
					}
				}

				OutDefinition.Transitions.Add(TransitionSpec);
			}
		}

		if (OutDefinition.EntryState.IsNone() && OutDefinition.States.Num() > 0)
		{
			OutDefinition.EntryState = OutDefinition.States[0].StateName;
		}

		return OutDefinition.States.Num() > 0 && !OutDefinition.EntryState.IsNone();
	}

	bool ParseAnimationOverridesJson(const FString& OverridesJson, TArray<FAnimAssetOverrideSpec>& OutOverrides)
	{
		OutOverrides.Reset();
		if (OverridesJson.IsEmpty())
		{
			return true;
		}

		TArray<TSharedPtr<FJsonValue>> RootArray;
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(OverridesJson);
		if (!FJsonSerializer::Deserialize(Reader, RootArray))
		{
			return false;
		}

		for (const TSharedPtr<FJsonValue>& Value : RootArray)
		{
			const TSharedPtr<FJsonObject> OverrideObject = Value.IsValid() ? Value->AsObject() : nullptr;
			if (!OverrideObject.IsValid())
			{
				continue;
			}

			FString StateName;
			OverrideObject->TryGetStringField(TEXT("StateName"), StateName);
			if (StateName.IsEmpty())
			{
				continue;
			}

			FAnimAssetOverrideSpec OverrideSpec;
			OverrideSpec.StateName = FName(*StateName);
			OverrideObject->TryGetStringField(TEXT("AnimationAsset"), OverrideSpec.AnimationAssetPath);
			if (OverrideSpec.AnimationAssetPath.IsEmpty())
			{
				OverrideObject->TryGetStringField(TEXT("AssetPath"), OverrideSpec.AnimationAssetPath);
			}
			OutOverrides.Add(OverrideSpec);
		}

		return true;
	}

	UEdGraphPin* FindFirstPin(UEdGraphNode* Node, EEdGraphPinDirection Direction)
	{
		if (!Node)
		{
			return nullptr;
		}

		for (UEdGraphPin* Pin : Node->Pins)
		{
			if (Pin && Pin->Direction == Direction)
			{
				return Pin;
			}
		}
		return nullptr;
	}

	UEdGraph* FindAnimGraph(UAnimBlueprint* AnimBlueprint)
	{
		if (!AnimBlueprint)
		{
			return nullptr;
		}

		TArray<UEdGraph*> AllGraphs;
		AnimBlueprint->GetAllGraphs(AllGraphs);

		for (UEdGraph* Graph : AllGraphs)
		{
			if (Graph && Graph->GetSchema()->IsA(UAnimationGraphSchema::StaticClass()) && Graph->GetFName() == UEdGraphSchema_K2::GN_AnimGraph)
			{
				return Graph;
			}
		}

		for (UEdGraph* Graph : AllGraphs)
		{
			if (Graph && Graph->GetSchema()->IsA(UAnimationGraphSchema::StaticClass()))
			{
				return Graph;
			}
		}

		return nullptr;
	}

	UAnimGraphNode_Root* FindRootNode(UEdGraph* Graph)
	{
		if (!Graph)
		{
			return nullptr;
		}

		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (UAnimGraphNode_Root* RootNode = Cast<UAnimGraphNode_Root>(Node))
			{
				return RootNode;
			}
		}
		return nullptr;
	}

	UAnimGraphNode_StateMachineBase* FindStateMachineNode(UBlueprint* Blueprint, const FName StateMachineName)
	{
		if (!Blueprint)
		{
			return nullptr;
		}

		TArray<UAnimGraphNode_StateMachineBase*> Nodes;
		FBlueprintEditorUtils::GetAllNodesOfClassEx<UAnimGraphNode_StateMachine, UAnimGraphNode_StateMachineBase>(Blueprint, Nodes);
		for (UAnimGraphNode_StateMachineBase* Node : Nodes)
		{
			UAnimationStateMachineGraph* StateMachineGraph = Node ? Node->EditorStateMachineGraph.Get() : nullptr;
			if (StateMachineGraph && StateMachineGraph->GetFName() == StateMachineName)
			{
				return Node;
			}
		}
		return nullptr;
	}

	UAnimGraphNode_StateMachineBase* EnsureStateMachineNode(UBlueprint* Blueprint, UEdGraph* AnimGraph, const FName StateMachineName)
	{
		if (UAnimGraphNode_StateMachineBase* Existing = FindStateMachineNode(Blueprint, StateMachineName))
		{
			return Existing;
		}

		if (!AnimGraph)
		{
			return nullptr;
		}

		FGraphNodeCreator<UAnimGraphNode_StateMachine> NodeCreator(*AnimGraph);
		UAnimGraphNode_StateMachine* NewNode = NodeCreator.CreateNode();
		NodeCreator.Finalize();
		if (!NewNode || !NewNode->EditorStateMachineGraph)
		{
			return nullptr;
		}

		NewNode->NodePosX = -320;
		NewNode->NodePosY = -40;
		FBlueprintEditorUtils::RenameGraph(NewNode->EditorStateMachineGraph.Get(), StateMachineName.ToString());
		return NewNode;
	}

	UAnimGraphNode_Slot* FindSlotNode(UEdGraph* Graph, const FName SlotName)
	{
		if (!Graph)
		{
			return nullptr;
		}

		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (UAnimGraphNode_Slot* SlotNode = Cast<UAnimGraphNode_Slot>(Node))
			{
				if (SlotName.IsNone() || SlotNode->Node.SlotName == SlotName)
				{
					return SlotNode;
				}
			}
		}
		return nullptr;
	}

	UAnimGraphNode_Slot* EnsureSlotNode(UEdGraph* AnimGraph, const FName SlotName, const bool bAlwaysUpdateSourcePose)
	{
		if (!AnimGraph || SlotName.IsNone())
		{
			return nullptr;
		}

		UAnimGraphNode_Slot* SlotNode = FindSlotNode(AnimGraph, SlotName);
		if (!SlotNode)
		{
			FGraphNodeCreator<UAnimGraphNode_Slot> NodeCreator(*AnimGraph);
			SlotNode = NodeCreator.CreateNode();
			NodeCreator.Finalize();
			if (!SlotNode)
			{
				return nullptr;
			}
			SlotNode->NodePosX = -208;
			SlotNode->NodePosY = 64;
		}

		SlotNode->Node.SlotName = SlotName;
		SlotNode->Node.bAlwaysUpdateSourcePose = bAlwaysUpdateSourcePose;
		return SlotNode;
	}

	void ConnectStateMachineToOutput(
		UBlueprint* Blueprint,
		UEdGraph* AnimGraph,
		UAnimGraphNode_StateMachineBase* StateMachineNode,
		const FAnimStateMachineDefinition& Definition)
	{
		if (!Blueprint || !AnimGraph || !StateMachineNode)
		{
			return;
		}

		UAnimGraphNode_Root* RootNode = FindRootNode(AnimGraph);
		if (!RootNode)
		{
			return;
		}

		UEdGraphPin* StateMachineOut = FindFirstPin(StateMachineNode, EGPD_Output);
		UEdGraphPin* RootIn = FindFirstPin(RootNode, EGPD_Input);
		if (!StateMachineOut || !RootIn)
		{
			return;
		}

		if (RootIn->LinkedTo.Num() > 0)
		{
			RootIn->BreakAllPinLinks();
		}

		if (!Definition.OutputSlotName.IsNone())
		{
			if (UAnimGraphNode_Slot* SlotNode = EnsureSlotNode(AnimGraph, Definition.OutputSlotName, Definition.bAlwaysUpdateSourcePose))
			{
				UEdGraphPin* SlotIn = FindFirstPin(SlotNode, EGPD_Input);
				UEdGraphPin* SlotOut = FindFirstPin(SlotNode, EGPD_Output);
				if (SlotIn && SlotOut)
				{
					if (SlotIn->LinkedTo.Num() > 0)
					{
						SlotIn->BreakAllPinLinks();
					}
					AnimGraph->GetSchema()->TryCreateConnection(StateMachineOut, SlotIn);
					AnimGraph->GetSchema()->TryCreateConnection(SlotOut, RootIn);
					return;
				}
			}
		}

		AnimGraph->GetSchema()->TryCreateConnection(StateMachineOut, RootIn);
	}

	void ClearStateMachineGraph(UBlueprint* Blueprint, UEdGraph* StateMachineGraph)
	{
		if (!Blueprint || !StateMachineGraph)
		{
			return;
		}

		TArray<UEdGraphNode*> NodesToRemove;
		for (UEdGraphNode* Node : StateMachineGraph->Nodes)
		{
			if (!Node || Node->IsA(UAnimStateEntryNode::StaticClass()))
			{
				continue;
			}
			NodesToRemove.Add(Node);
		}

		for (UEdGraphNode* Node : NodesToRemove)
		{
			FBlueprintEditorUtils::RemoveNode(Blueprint, Node, true);
		}
	}

	void ClearAnimStateGraph(UBlueprint* Blueprint, UEdGraph* StateGraph)
	{
		if (!Blueprint || !StateGraph)
		{
			return;
		}

		TArray<UEdGraphNode*> NodesToRemove;
		for (UEdGraphNode* Node : StateGraph->Nodes)
		{
			if (!Node || Node->IsA(UAnimGraphNode_Root::StaticClass()) || Node->IsA(UAnimGraphNode_StateResult::StaticClass()))
			{
				continue;
			}
			NodesToRemove.Add(Node);
		}

		for (UEdGraphNode* Node : NodesToRemove)
		{
			FBlueprintEditorUtils::RemoveNode(Blueprint, Node, true);
		}
	}

	void ClearTransitionRuleGraph(UBlueprint* Blueprint, UEdGraph* TransitionGraph)
	{
		if (!Blueprint || !TransitionGraph)
		{
			return;
		}

		TArray<UEdGraphNode*> NodesToRemove;
		for (UEdGraphNode* Node : TransitionGraph->Nodes)
		{
			if (!Node || Node->IsA(UAnimGraphNode_TransitionResult::StaticClass()))
			{
				continue;
			}
			NodesToRemove.Add(Node);
		}

		for (UEdGraphNode* Node : NodesToRemove)
		{
			FBlueprintEditorUtils::RemoveNode(Blueprint, Node, true);
		}
	}

	void ClearK2Graph(UBlueprint* Blueprint, UEdGraph* Graph)
	{
		if (!Blueprint || !Graph)
		{
			return;
		}

		if (!Graph->GetSchema() || !Graph->GetSchema()->IsA(UEdGraphSchema_K2::StaticClass()))
		{
			return;
		}

		TArray<UEdGraphNode*> NodesToRemove;
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (!Node || Node->IsA(UK2Node_FunctionEntry::StaticClass()) || Node->IsA(UK2Node_FunctionResult::StaticClass()))
			{
				continue;
			}
			NodesToRemove.Add(Node);
		}

		for (UEdGraphNode* Node : NodesToRemove)
		{
			FBlueprintEditorUtils::RemoveNode(Blueprint, Node, true);
		}
	}

	void ClearBlueprintOwnedVariables(UBlueprint* Blueprint)
	{
		if (!Blueprint)
		{
			return;
		}

		TArray<FName> VariablesToRemove;
		for (const FBPVariableDescription& VarDesc : Blueprint->NewVariables)
		{
			if (!VarDesc.VarName.IsNone())
			{
				VariablesToRemove.Add(VarDesc.VarName);
			}
		}

		for (const FName& VarName : VariablesToRemove)
		{
			FBlueprintEditorUtils::RemoveMemberVariable(Blueprint, VarName);
		}
	}

	void ClearBlueprintOwnedComponents(UBlueprint* Blueprint)
	{
		if (!Blueprint)
		{
			return;
		}

		USimpleConstructionScript* SCS = Blueprint->SimpleConstructionScript;
		if (!SCS)
		{
			return;
		}

		TArray<USCS_Node*> NodesToRemove = SCS->GetAllNodes();
		for (USCS_Node* Node : NodesToRemove)
		{
			if (!Node)
			{
				continue;
			}

			if (Node == SCS->GetDefaultSceneRootNode())
			{
				continue;
			}

			SCS->RemoveNode(Node);
		}
	}

	UAnimStateEntryNode* FindEntryNode(UEdGraph* StateMachineGraph)
	{
		if (!StateMachineGraph)
		{
			return nullptr;
		}

		for (UEdGraphNode* Node : StateMachineGraph->Nodes)
		{
			if (UAnimStateEntryNode* EntryNode = Cast<UAnimStateEntryNode>(Node))
			{
				return EntryNode;
			}
		}
		return nullptr;
	}

	UAnimStateNode* FindStateNodeByName(TMap<FName, UAnimStateNode*>& CreatedStates, const FName StateName)
	{
		if (UAnimStateNode* const* FoundState = CreatedStates.Find(StateName))
		{
			return *FoundState;
		}
		return nullptr;
	}

	UAnimStateNode* CreateStateNode(UBlueprint* Blueprint, UEdGraph* StateMachineGraph, const FAnimStateMachineStateSpec& Spec)
	{
		if (!Blueprint || !StateMachineGraph)
		{
			return nullptr;
		}

		UAnimStateNode* StateNode = FEdGraphSchemaAction_NewStateNode::SpawnNodeFromTemplate<UAnimStateNode>(
			StateMachineGraph,
			NewObject<UAnimStateNode>(),
			Spec.NodePosition,
			false);
		if (!StateNode || !StateNode->BoundGraph)
		{
			return nullptr;
		}

		StateNode->NodePosX = static_cast<int32>(Spec.NodePosition.X);
		StateNode->NodePosY = static_cast<int32>(Spec.NodePosition.Y);
		FBlueprintEditorUtils::RenameGraph(StateNode->BoundGraph, Spec.StateName.ToString());

		ClearAnimStateGraph(Blueprint, StateNode->BoundGraph);

		if (UEdGraphPin* PosePin = StateNode->GetPoseSinkPinInsideState())
		{
			FGraphNodeCreator<UAnimGraphNode_SequencePlayer> PlayerCreator(*StateNode->BoundGraph);
			UAnimGraphNode_SequencePlayer* PlayerNode = PlayerCreator.CreateNode();
			PlayerCreator.Finalize();
			if (!PlayerNode)
			{
				return nullptr;
			}

			PlayerNode->NodePosX = -320;
			PlayerNode->NodePosY = 0;
			PlayerNode->NodeComment = FString::Printf(TEXT("__AnimStateSlot:%s"), *Spec.StateName.ToString());
			PlayerNode->bCommentBubbleVisible = false;

			if (UAnimationAsset* AnimationAsset = LoadEditorAsset<UAnimationAsset>(Spec.AnimationAssetPath))
			{
				PlayerNode->SetAnimationAsset(AnimationAsset);
			}

			if (UEdGraphPin* PlayerOutput = FindFirstPin(PlayerNode, EGPD_Output))
			{
				StateNode->BoundGraph->GetSchema()->TryCreateConnection(PlayerOutput, PosePin);
			}
		}

		return StateNode;
	}

	UAnimStateTransitionNode* FindTransitionNode(UEdGraph* StateMachineGraph, UAnimStateNodeBase* PreviousState, UAnimStateNodeBase* NextState)
	{
		if (!StateMachineGraph || !PreviousState || !NextState)
		{
			return nullptr;
		}

		for (UEdGraphNode* Node : StateMachineGraph->Nodes)
		{
			UAnimStateTransitionNode* TransitionNode = Cast<UAnimStateTransitionNode>(Node);
			if (TransitionNode && TransitionNode->GetPreviousState() == PreviousState && TransitionNode->GetNextState() == NextState)
			{
				return TransitionNode;
			}
		}
		return nullptr;
	}

	UClass* GetTransitionVariableOwnerClass(UBlueprint* Blueprint)
	{
		if (!Blueprint)
		{
			return nullptr;
		}

		return Blueprint->SkeletonGeneratedClass ? Blueprint->SkeletonGeneratedClass : Blueprint->GeneratedClass;
	}

	UFunction* ResolveNumericComparisonFunction(const FProperty* Property, const EAnimTransitionComparisonOp ComparisonOp)
	{
		if (!Property)
		{
			return nullptr;
		}

		const TCHAR* Suffix = TEXT("DoubleDouble");
		FName FunctionName = NAME_None;

		switch (ComparisonOp)
		{
		case EAnimTransitionComparisonOp::Equal:
			FunctionName = FName(*FString::Printf(TEXT("EqualEqual_%s"), Suffix));
			break;
		case EAnimTransitionComparisonOp::NotEqual:
			FunctionName = FName(*FString::Printf(TEXT("NotEqual_%s"), Suffix));
			break;
		case EAnimTransitionComparisonOp::Greater:
			FunctionName = FName(*FString::Printf(TEXT("Greater_%s"), Suffix));
			break;
		case EAnimTransitionComparisonOp::GreaterOrEqual:
			FunctionName = FName(*FString::Printf(TEXT("GreaterEqual_%s"), Suffix));
			break;
		case EAnimTransitionComparisonOp::Less:
			FunctionName = FName(*FString::Printf(TEXT("Less_%s"), Suffix));
			break;
		case EAnimTransitionComparisonOp::LessOrEqual:
			FunctionName = FName(*FString::Printf(TEXT("LessEqual_%s"), Suffix));
			break;
		default:
			return nullptr;
		}

		return FindUField<UFunction>(UKismetMathLibrary::StaticClass(), FunctionName);
	}

	UEdGraphPin* FindNthDataInputPin(UK2Node* Node, int32 InputIndex)
	{
		if (!Node || InputIndex < 0)
		{
			return nullptr;
		}

		int32 CurrentIndex = 0;
		for (UEdGraphPin* Pin : Node->Pins)
		{
			if (!Pin || Pin->Direction != EGPD_Input)
			{
				continue;
			}

			if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec || Pin->PinName == UEdGraphSchema_K2::PN_Self)
			{
				continue;
			}

			if (CurrentIndex == InputIndex)
			{
				return Pin;
			}
			++CurrentIndex;
		}

		return nullptr;
	}

	UEdGraphPin* FindBoolOutputPin(UK2Node* Node)
	{
		if (!Node)
		{
			return nullptr;
		}

		if (UK2Node_CallFunction* CallFunctionNode = Cast<UK2Node_CallFunction>(Node))
		{
			if (UEdGraphPin* ReturnPin = CallFunctionNode->GetReturnValuePin())
			{
				return ReturnPin;
			}
		}

		if (UK2Node_CommutativeAssociativeBinaryOperator* BinaryOperatorNode = Cast<UK2Node_CommutativeAssociativeBinaryOperator>(Node))
		{
			if (UEdGraphPin* ReturnPin = BinaryOperatorNode->GetReturnValuePin())
			{
				return ReturnPin;
			}
		}

		for (UEdGraphPin* Pin : Node->Pins)
		{
			if (!Pin || Pin->Direction != EGPD_Output)
			{
				continue;
			}

			if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Boolean)
			{
				return Pin;
			}
		}

		return nullptr;
	}

	void ConfigureTransitionRule(UBlueprint* Blueprint, UEdGraph* TransitionGraph, const FAnimStateMachineTransitionSpec& TransitionSpec)
	{
		if (!Blueprint || !TransitionGraph || TransitionSpec.ConditionVariable.IsNone())
		{
			return;
		}

		ClearTransitionRuleGraph(Blueprint, TransitionGraph);

		UAnimGraphNode_TransitionResult* ResultNode = nullptr;
		for (UEdGraphNode* Node : TransitionGraph->Nodes)
		{
			ResultNode = Cast<UAnimGraphNode_TransitionResult>(Node);
			if (ResultNode)
			{
				break;
			}
		}
		if (!ResultNode)
		{
			return;
		}

		UClass* VariableOwnerClass = GetTransitionVariableOwnerClass(Blueprint);
		if (!VariableOwnerClass)
		{
			return;
		}

		FProperty* Property = FindFProperty<FProperty>(VariableOwnerClass, TransitionSpec.ConditionVariable);
		if (!Property)
		{
			UE_LOG(LogTemp, Warning, TEXT("[BPFactory] Missing transition variable: %s"), *TransitionSpec.ConditionVariable.ToString());
			return;
		}

		FGraphNodeCreator<UK2Node_VariableGet> VarCreator(*TransitionGraph);
		UK2Node_VariableGet* VarNode = VarCreator.CreateNode();
		VarNode->SetFromProperty(Property, true, VariableOwnerClass);
		VarCreator.Finalize();
		VarNode->NodePosX = -360;
		VarNode->NodePosY = -8;

		UEdGraphPin* ValuePin = VarNode->GetValuePin();
		UEdGraphPin* ResultPin = ResultNode->FindPin(TEXT("bCanEnterTransition"));
		if (!ValuePin || !ResultPin)
		{
			return;
		}

		if (TransitionSpec.ConditionType == EAnimTransitionConditionType::Bool)
		{
			if (TransitionSpec.bExpectedValue)
			{
				TransitionGraph->GetSchema()->TryCreateConnection(ValuePin, ResultPin);
				return;
			}

			UFunction* NotFunction = FindUField<UFunction>(UKismetMathLibrary::StaticClass(), GET_FUNCTION_NAME_CHECKED(UKismetMathLibrary, Not_PreBool));
			if (!NotFunction)
			{
				return;
			}

			FGraphNodeCreator<UK2Node_CallFunction> FuncCreator(*TransitionGraph);
			UK2Node_CallFunction* NotNode = FuncCreator.CreateNode();
			NotNode->SetFromFunction(NotFunction);
			FuncCreator.Finalize();
			NotNode->NodePosX = -120;
			NotNode->NodePosY = -8;

			UEdGraphPin* InputPin = NotNode->FindPin(TEXT("A"));
			UEdGraphPin* ReturnPin = NotNode->GetReturnValuePin();
			if (!InputPin || !ReturnPin)
			{
				return;
			}

			TransitionGraph->GetSchema()->TryCreateConnection(ValuePin, InputPin);
			TransitionGraph->GetSchema()->TryCreateConnection(ReturnPin, ResultPin);
			return;
		}

		if (!CastField<const FNumericProperty>(Property))
		{
			UE_LOG(LogTemp, Warning, TEXT("[BPFactory] Transition requested numeric comparison on non-numeric variable: %s"), *TransitionSpec.ConditionVariable.ToString());
			return;
		}

		UFunction* ComparisonFunction = ResolveNumericComparisonFunction(Property, TransitionSpec.ComparisonOp);
		if (!ComparisonFunction)
		{
			UE_LOG(LogTemp, Warning, TEXT("[BPFactory] Unsupported numeric transition comparison for %s"), *TransitionSpec.ConditionVariable.ToString());
			return;
		}

		FGraphNodeCreator<UK2Node_CallFunction> FuncCreator(*TransitionGraph);
		UK2Node_CallFunction* CompareNode = FuncCreator.CreateNode();
		CompareNode->SetFromFunction(ComparisonFunction);
		FuncCreator.Finalize();
		CompareNode->NodePosX = -120;
		CompareNode->NodePosY = -8;
		CompareNode->ReconstructNode();

		UEdGraphPin* InputPinA = CompareNode->FindPin(TEXT("A"));
		UEdGraphPin* InputPinB = CompareNode->FindPin(TEXT("B"));
		if (!InputPinA)
		{
			InputPinA = FindNthDataInputPin(CompareNode, 0);
		}
		if (!InputPinB)
		{
			InputPinB = FindNthDataInputPin(CompareNode, 1);
		}
		UEdGraphPin* ReturnPin = FindBoolOutputPin(CompareNode);
		if (!InputPinA || !InputPinB || !ReturnPin)
		{
			UE_LOG(
				LogTemp,
				Warning,
				TEXT("[BPFactory] Failed to resolve pins for numeric transition comparison %s using node %s"),
				*TransitionSpec.ConditionVariable.ToString(),
				*GetNameSafe(CompareNode));
			return;
		}

		InputPinB->DefaultValue = FString::SanitizeFloat(TransitionSpec.NumericValue);
		TransitionGraph->GetSchema()->TryCreateConnection(ValuePin, InputPinA);
		TransitionGraph->GetSchema()->TryCreateConnection(ReturnPin, ResultPin);
	}

	UAnimStateTransitionNode* CreateConditionalTransition(
		UBlueprint* Blueprint,
		UEdGraph* StateMachineGraph,
		UAnimStateNodeBase* FromState,
		UAnimStateNodeBase* ToState,
		const FAnimStateMachineTransitionSpec& TransitionSpec,
		const int32 PriorityOrder,
		const float CrossfadeDuration)
	{
		if (!Blueprint || !StateMachineGraph || !FromState || !ToState)
		{
			return nullptr;
		}

		const UAnimationStateMachineSchema* Schema = Cast<const UAnimationStateMachineSchema>(StateMachineGraph->GetSchema());
		if (!Schema)
		{
			return nullptr;
		}

		if (!Schema->CreateAutomaticConversionNodeAndConnections(FromState->GetOutputPin(), ToState->GetInputPin()))
		{
			return nullptr;
		}

		UAnimStateTransitionNode* TransitionNode = FindTransitionNode(StateMachineGraph, FromState, ToState);
		if (!TransitionNode)
		{
			return nullptr;
		}

		TransitionNode->PriorityOrder = PriorityOrder;
		TransitionNode->CrossfadeDuration = CrossfadeDuration;
		TransitionNode->bAutomaticRuleBasedOnSequencePlayerInState = false;
		ConfigureTransitionRule(Blueprint, TransitionNode->BoundGraph, TransitionSpec);
		return TransitionNode;
	}

	FName GetStateName(const UAnimStateNode* StateNode)
	{
		if (!StateNode)
		{
			return NAME_None;
		}

		if (StateNode->BoundGraph)
		{
			return StateNode->BoundGraph->GetFName();
		}

		return FName(*StateNode->GetNodeTitle(ENodeTitleType::ListView).ToString());
	}

	UAnimGraphNode_SequencePlayer* FindSequencePlayerNode(UEdGraph* StateGraph)
	{
		if (!StateGraph)
		{
			return nullptr;
		}

		for (UEdGraphNode* Node : StateGraph->Nodes)
		{
			if (UAnimGraphNode_SequencePlayer* SequencePlayer = Cast<UAnimGraphNode_SequencePlayer>(Node))
			{
				return SequencePlayer;
			}
		}

		return nullptr;
	}

	UAnimGraphNode_StateMachineBase* FindPrimaryStateMachineNode(UEdGraph* AnimGraph)
	{
		if (!AnimGraph)
		{
			return nullptr;
		}

		UAnimGraphNode_Root* RootNode = FindRootNode(AnimGraph);
		if (RootNode)
		{
			if (UEdGraphPin* RootIn = FindFirstPin(RootNode, EGPD_Input))
			{
				for (UEdGraphPin* LinkedPin : RootIn->LinkedTo)
				{
					if (!LinkedPin)
					{
						continue;
					}

					if (UAnimGraphNode_StateMachineBase* DirectStateMachine = Cast<UAnimGraphNode_StateMachineBase>(LinkedPin->GetOwningNode()))
					{
						return DirectStateMachine;
					}

					if (UAnimGraphNode_Slot* SlotNode = Cast<UAnimGraphNode_Slot>(LinkedPin->GetOwningNode()))
					{
						if (UEdGraphPin* SlotSource = FindFirstPin(SlotNode, EGPD_Input))
						{
							for (UEdGraphPin* SlotLinkedPin : SlotSource->LinkedTo)
							{
								if (SlotLinkedPin)
								{
									if (UAnimGraphNode_StateMachineBase* IndirectStateMachine = Cast<UAnimGraphNode_StateMachineBase>(SlotLinkedPin->GetOwningNode()))
									{
										return IndirectStateMachine;
									}
								}
							}
						}
					}
				}
			}
		}

		for (UEdGraphNode* Node : AnimGraph->Nodes)
		{
			if (UAnimGraphNode_StateMachineBase* StateMachineNode = Cast<UAnimGraphNode_StateMachineBase>(Node))
			{
				return StateMachineNode;
			}
		}

		return nullptr;
	}

	UAnimGraphNode_Slot* FindPrimaryOutputSlotNode(UEdGraph* AnimGraph)
	{
		if (!AnimGraph)
		{
			return nullptr;
		}

		if (UAnimGraphNode_Root* RootNode = FindRootNode(AnimGraph))
		{
			if (UEdGraphPin* RootIn = FindFirstPin(RootNode, EGPD_Input))
			{
				for (UEdGraphPin* LinkedPin : RootIn->LinkedTo)
				{
					if (LinkedPin)
					{
						if (UAnimGraphNode_Slot* SlotNode = Cast<UAnimGraphNode_Slot>(LinkedPin->GetOwningNode()))
						{
							return SlotNode;
						}
					}
				}
			}
		}

		return nullptr;
	}

	bool TryExtractLinkedVariable(UEdGraphPin* Pin, FName& OutVariableName)
	{
		OutVariableName = NAME_None;
		if (!Pin || Pin->LinkedTo.Num() == 0)
		{
			return false;
		}

		if (UK2Node_VariableGet* VarNode = Cast<UK2Node_VariableGet>(Pin->LinkedTo[0] ? Pin->LinkedTo[0]->GetOwningNode() : nullptr))
		{
			OutVariableName = VarNode->GetVarName();
			return !OutVariableName.IsNone();
		}

		return false;
	}

	bool TryMapFunctionNameToComparison(const FName FunctionName, EAnimTransitionComparisonOp& OutComparisonOp)
	{
		const FString FunctionNameString = FunctionName.ToString();
		if (FunctionNameString.StartsWith(TEXT("EqualEqual_")))
		{
			OutComparisonOp = EAnimTransitionComparisonOp::Equal;
			return true;
		}
		if (FunctionNameString.StartsWith(TEXT("NotEqual_")))
		{
			OutComparisonOp = EAnimTransitionComparisonOp::NotEqual;
			return true;
		}
		if (FunctionNameString.StartsWith(TEXT("GreaterEqual_")))
		{
			OutComparisonOp = EAnimTransitionComparisonOp::GreaterOrEqual;
			return true;
		}
		if (FunctionNameString.StartsWith(TEXT("Greater_")))
		{
			OutComparisonOp = EAnimTransitionComparisonOp::Greater;
			return true;
		}
		if (FunctionNameString.StartsWith(TEXT("LessEqual_")))
		{
			OutComparisonOp = EAnimTransitionComparisonOp::LessOrEqual;
			return true;
		}
		if (FunctionNameString.StartsWith(TEXT("Less_")))
		{
			OutComparisonOp = EAnimTransitionComparisonOp::Less;
			return true;
		}
		return false;
	}

	bool TryExtractNumericDefaultValue(UEdGraphPin* Pin, double& OutValue)
	{
		OutValue = 0.0;
		if (!Pin)
		{
			return false;
		}

		if (!Pin->DefaultValue.IsEmpty())
		{
			OutValue = FCString::Atod(*Pin->DefaultValue);
			return true;
		}

		return false;
	}

	bool ExtractTransitionCondition(UEdGraph* TransitionGraph, FAnimStateMachineTransitionSpec& OutTransitionSpec)
	{
		OutTransitionSpec.ConditionVariable = NAME_None;
		OutTransitionSpec.ConditionType = EAnimTransitionConditionType::Bool;
		OutTransitionSpec.bExpectedValue = true;
		OutTransitionSpec.ComparisonOp = EAnimTransitionComparisonOp::Equal;
		OutTransitionSpec.NumericValue = 0.0;
		if (!TransitionGraph)
		{
			return false;
		}

		UAnimGraphNode_TransitionResult* ResultNode = nullptr;
		for (UEdGraphNode* Node : TransitionGraph->Nodes)
		{
			if (UAnimGraphNode_TransitionResult* Candidate = Cast<UAnimGraphNode_TransitionResult>(Node))
			{
				ResultNode = Candidate;
				break;
			}
		}
		if (!ResultNode)
		{
			return false;
		}

		UEdGraphPin* ResultPin = ResultNode->FindPin(TEXT("bCanEnterTransition"));
		if (!ResultPin || ResultPin->LinkedTo.Num() == 0)
		{
			return false;
		}

		UEdGraphNode* SourceNode = ResultPin->LinkedTo[0] ? ResultPin->LinkedTo[0]->GetOwningNode() : nullptr;
		if (UK2Node_VariableGet* VarNode = Cast<UK2Node_VariableGet>(SourceNode))
		{
			OutTransitionSpec.ConditionVariable = VarNode->GetVarName();
			OutTransitionSpec.ConditionType = EAnimTransitionConditionType::Bool;
			OutTransitionSpec.bExpectedValue = true;
			return !OutTransitionSpec.ConditionVariable.IsNone();
		}

		if (UK2Node_CallFunction* CallNode = Cast<UK2Node_CallFunction>(SourceNode))
		{
			if (CallNode->GetFunctionName() == GET_FUNCTION_NAME_CHECKED(UKismetMathLibrary, Not_PreBool))
			{
				if (UEdGraphPin* InputPin = CallNode->FindPin(TEXT("A")))
				{
					if (TryExtractLinkedVariable(InputPin, OutTransitionSpec.ConditionVariable))
					{
						OutTransitionSpec.ConditionType = EAnimTransitionConditionType::Bool;
						OutTransitionSpec.bExpectedValue = false;
						return true;
					}
				}
			}

			EAnimTransitionComparisonOp ComparisonOp = EAnimTransitionComparisonOp::Equal;
			if (TryMapFunctionNameToComparison(CallNode->GetFunctionName(), ComparisonOp))
			{
				FName VariableName = NAME_None;
				double NumericValue = 0.0;
				if (TryExtractLinkedVariable(CallNode->FindPin(TEXT("A")), VariableName)
					&& TryExtractNumericDefaultValue(CallNode->FindPin(TEXT("B")), NumericValue))
				{
					OutTransitionSpec.ConditionVariable = VariableName;
					OutTransitionSpec.ConditionType = EAnimTransitionConditionType::NumericComparison;
					OutTransitionSpec.ComparisonOp = ComparisonOp;
					OutTransitionSpec.NumericValue = NumericValue;
					return !OutTransitionSpec.ConditionVariable.IsNone();
				}

				if (TryExtractLinkedVariable(CallNode->FindPin(TEXT("B")), VariableName)
					&& TryExtractNumericDefaultValue(CallNode->FindPin(TEXT("A")), NumericValue))
				{
					OutTransitionSpec.ConditionVariable = VariableName;
					OutTransitionSpec.ConditionType = EAnimTransitionConditionType::NumericComparison;
					OutTransitionSpec.ComparisonOp = ComparisonOp;
					OutTransitionSpec.NumericValue = NumericValue;
					return !OutTransitionSpec.ConditionVariable.IsNone();
				}
			}
		}

		return false;
	}

	TMap<FGuid, FName> BuildPlayerNodeGuidToStateMap(UAnimBlueprint* AnimBlueprint)
	{
		TMap<FGuid, FName> Result;
		if (!AnimBlueprint)
		{
			return Result;
		}

		TArray<UEdGraph*> AllGraphs;
		AnimBlueprint->GetAllGraphs(AllGraphs);
		for (UEdGraph* Graph : AllGraphs)
		{
			UAnimationStateMachineGraph* StateMachineGraph = Cast<UAnimationStateMachineGraph>(Graph);
			if (!StateMachineGraph)
			{
				continue;
			}

			for (UEdGraphNode* Node : StateMachineGraph->Nodes)
			{
				UAnimStateNode* StateNode = Cast<UAnimStateNode>(Node);
				if (!StateNode || !StateNode->BoundGraph)
				{
					continue;
				}

				if (UAnimGraphNode_SequencePlayer* SequencePlayer = FindSequencePlayerNode(StateNode->BoundGraph))
				{
					Result.Add(SequencePlayer->NodeGuid, GetStateName(StateNode));
				}
			}
		}

		return Result;
	}

	TMap<FName, FGuid> BuildStateToPlayerNodeGuidMap(UAnimBlueprint* AnimBlueprint)
	{
		TMap<FName, FGuid> Result;
		for (const TPair<FGuid, FName>& Pair : BuildPlayerNodeGuidToStateMap(AnimBlueprint))
		{
			Result.Add(Pair.Value, Pair.Key);
		}
		return Result;
	}

	FString GetAnimationAssetPath(UAnimationAsset* AnimationAsset)
	{
		return AnimationAsset ? AnimationAsset->GetOutermost()->GetName() : FString();
	}

	FString GetBlueprintUnLuaModuleName(UBlueprint* Blueprint)
	{
		if (!Blueprint)
		{
			return FString();
		}

		bool bHasUnLuaInterface = false;
		for (const FBPInterfaceDescription& InterfaceDesc : Blueprint->ImplementedInterfaces)
		{
			if (InterfaceDesc.Interface && InterfaceDesc.Interface->GetName().Contains(TEXT("UnLuaInterface")))
			{
				bHasUnLuaInterface = true;
				break;
			}
		}
		if (!bHasUnLuaInterface)
		{
			return FString();
		}

		TArray<UEdGraph*> AllGraphs;
		AllGraphs.Append(Blueprint->FunctionGraphs);
		for (const FBPInterfaceDescription& InterfaceDesc : Blueprint->ImplementedInterfaces)
		{
			AllGraphs.Append(InterfaceDesc.Graphs);
		}

		for (UEdGraph* Graph : AllGraphs)
		{
			if (!Graph || !Graph->GetFName().ToString().Contains(TEXT("GetModuleName")))
			{
				continue;
			}

			for (UEdGraphNode* Node : Graph->Nodes)
			{
				UK2Node_FunctionResult* ResultNode = Cast<UK2Node_FunctionResult>(Node);
				if (!ResultNode)
				{
					continue;
				}

				for (UEdGraphPin* Pin : ResultNode->Pins)
				{
					if (Pin && Pin->PinName == TEXT("ReturnValue") && !Pin->DefaultValue.IsEmpty())
					{
						return Pin->DefaultValue;
					}
				}
			}
		}

		return FString();
	}

	TSharedPtr<FJsonObject> BuildStateMachineDefinitionObject(UAnimBlueprint* AnimBlueprint)
	{
		UEdGraph* AnimGraph = FindAnimGraph(AnimBlueprint);
		UAnimGraphNode_StateMachineBase* StateMachineNode = FindPrimaryStateMachineNode(AnimGraph);
		UAnimationStateMachineGraph* StateMachineGraph = StateMachineNode ? StateMachineNode->EditorStateMachineGraph.Get() : nullptr;
		if (!AnimGraph || !StateMachineNode || !StateMachineGraph)
		{
			return nullptr;
		}

		TSharedPtr<FJsonObject> DefinitionObject = MakeShared<FJsonObject>();
		DefinitionObject->SetStringField(TEXT("StateMachineName"), StateMachineGraph->GetName());

		if (UAnimGraphNode_Slot* SlotNode = FindPrimaryOutputSlotNode(AnimGraph))
		{
			if (!SlotNode->Node.SlotName.IsNone())
			{
				DefinitionObject->SetStringField(TEXT("OutputSlotName"), SlotNode->Node.SlotName.ToString());
			}
			if (SlotNode->Node.bAlwaysUpdateSourcePose)
			{
				DefinitionObject->SetBoolField(TEXT("AlwaysUpdateSourcePose"), true);
			}
		}

		TArray<TSharedPtr<FJsonValue>> VariableValues;
		for (const FBPVariableDescription& VariableDesc : AnimBlueprint->NewVariables)
		{
			const FString TypeName = ExportPinTypeName(VariableDesc.VarType);
			if (TypeName.IsEmpty())
			{
				continue;
			}

			TSharedPtr<FJsonObject> VariableObject = MakeShared<FJsonObject>();
			VariableObject->SetStringField(TEXT("Name"), VariableDesc.VarName.ToString());
			VariableObject->SetStringField(TEXT("Type"), TypeName);
			VariableValues.Add(MakeShared<FJsonValueObject>(VariableObject));
		}
		if (VariableValues.Num() > 0)
		{
			DefinitionObject->SetArrayField(TEXT("Variables"), VariableValues);
		}

		TArray<TSharedPtr<FJsonValue>> StateValues;
		for (UEdGraphNode* Node : StateMachineGraph->Nodes)
		{
			UAnimStateNode* StateNode = Cast<UAnimStateNode>(Node);
			if (!StateNode)
			{
				continue;
			}

			TSharedPtr<FJsonObject> StateObject = MakeShared<FJsonObject>();
			StateObject->SetStringField(TEXT("Name"), GetStateName(StateNode).ToString());
			StateObject->SetObjectField(TEXT("NodePosition"), MakeShared<FJsonObject>());
			StateObject->GetObjectField(TEXT("NodePosition"))->SetNumberField(TEXT("X"), StateNode->NodePosX);
			StateObject->GetObjectField(TEXT("NodePosition"))->SetNumberField(TEXT("Y"), StateNode->NodePosY);
			if (UAnimGraphNode_SequencePlayer* SequencePlayer = FindSequencePlayerNode(StateNode->BoundGraph))
			{
				StateObject->SetStringField(TEXT("AnimationAsset"), GetAnimationAssetPath(SequencePlayer->GetAnimationAsset()));
			}
			StateValues.Add(MakeShared<FJsonValueObject>(StateObject));
		}
		if (StateValues.Num() > 0)
		{
			DefinitionObject->SetArrayField(TEXT("States"), StateValues);
		}

		if (UAnimStateEntryNode* EntryNode = FindEntryNode(StateMachineGraph))
		{
			if (UEdGraphPin* EntryOutput = FindFirstPin(EntryNode, EGPD_Output))
			{
				for (UEdGraphPin* LinkedPin : EntryOutput->LinkedTo)
				{
					if (UAnimStateNode* EntryState = Cast<UAnimStateNode>(LinkedPin ? LinkedPin->GetOwningNode() : nullptr))
					{
						DefinitionObject->SetStringField(TEXT("EntryState"), GetStateName(EntryState).ToString());
						break;
					}
				}
			}
		}

		TArray<TSharedPtr<FJsonValue>> TransitionValues;
		for (UEdGraphNode* Node : StateMachineGraph->Nodes)
		{
			UAnimStateTransitionNode* TransitionNode = Cast<UAnimStateTransitionNode>(Node);
			if (!TransitionNode)
			{
				continue;
			}

			UAnimStateNode* FromState = Cast<UAnimStateNode>(TransitionNode->GetPreviousState());
			UAnimStateNode* ToState = Cast<UAnimStateNode>(TransitionNode->GetNextState());
			if (!FromState || !ToState)
			{
				continue;
			}

			TSharedPtr<FJsonObject> TransitionObject = MakeShared<FJsonObject>();
			TransitionObject->SetStringField(TEXT("From"), GetStateName(FromState).ToString());
			TransitionObject->SetStringField(TEXT("To"), GetStateName(ToState).ToString());
			TransitionObject->SetNumberField(TEXT("Priority"), TransitionNode->PriorityOrder);
			TransitionObject->SetNumberField(TEXT("CrossfadeDuration"), TransitionNode->CrossfadeDuration);

			FAnimStateMachineTransitionSpec ExtractedTransitionSpec;
			if (ExtractTransitionCondition(TransitionNode->BoundGraph, ExtractedTransitionSpec) && !ExtractedTransitionSpec.ConditionVariable.IsNone())
			{
				TSharedPtr<FJsonObject> ConditionObject = MakeShared<FJsonObject>();
				ConditionObject->SetStringField(TEXT("Variable"), ExtractedTransitionSpec.ConditionVariable.ToString());
				if (ExtractedTransitionSpec.ConditionType == EAnimTransitionConditionType::NumericComparison)
				{
					ConditionObject->SetStringField(TEXT("Operator"), TransitionComparisonOpToString(ExtractedTransitionSpec.ComparisonOp));
					ConditionObject->SetNumberField(TEXT("Value"), ExtractedTransitionSpec.NumericValue);
				}
				else
				{
					ConditionObject->SetBoolField(TEXT("Value"), ExtractedTransitionSpec.bExpectedValue);
				}
				TransitionObject->SetObjectField(TEXT("Condition"), ConditionObject);
			}

			TransitionValues.Add(MakeShared<FJsonValueObject>(TransitionObject));
		}
		if (TransitionValues.Num() > 0)
		{
			DefinitionObject->SetArrayField(TEXT("Transitions"), TransitionValues);
		}

		return DefinitionObject;
	}

	TArray<TSharedPtr<FJsonValue>> BuildAnimationOverridesArray(UAnimBlueprint* AnimBlueprint)
	{
		TArray<TSharedPtr<FJsonValue>> OverrideValues;
		if (!AnimBlueprint || AnimBlueprint->ParentAssetOverrides.Num() == 0)
		{
			return OverrideValues;
		}

		UAnimBlueprint* RootAnimBlueprint = UAnimBlueprint::FindRootAnimBlueprint(AnimBlueprint);
		if (!RootAnimBlueprint)
		{
			RootAnimBlueprint = AnimBlueprint;
		}

		const TMap<FGuid, FName> GuidToStateName = BuildPlayerNodeGuidToStateMap(RootAnimBlueprint);
		for (const FAnimParentNodeAssetOverride& Override : AnimBlueprint->ParentAssetOverrides)
		{
			const FName* StateName = GuidToStateName.Find(Override.ParentNodeGuid);
			if (!StateName || StateName->IsNone() || !Override.NewAsset)
			{
				continue;
			}

			TSharedPtr<FJsonObject> OverrideObject = MakeShared<FJsonObject>();
			OverrideObject->SetStringField(TEXT("StateName"), StateName->ToString());
			OverrideObject->SetStringField(TEXT("AnimationAsset"), GetAnimationAssetPath(Override.NewAsset));
			OverrideValues.Add(MakeShared<FJsonValueObject>(OverrideObject));
		}

		return OverrideValues;
	}

	bool ApplyStateMachineDefinition(UAnimBlueprint* AnimBlueprint, const FAnimStateMachineDefinition& Definition)
	{
		if (!AnimBlueprint)
		{
			UE_LOG(LogTemp, Warning, TEXT("[BPFactory] ApplyStateMachineDefinition failed: AnimBlueprint is null"));
			return false;
		}

		if (Definition.States.Num() == 0)
		{
			UE_LOG(LogTemp, Warning, TEXT("[BPFactory] ApplyStateMachineDefinition failed: no states declared"));
			return false;
		}

		if (Definition.EntryState.IsNone())
		{
			UE_LOG(LogTemp, Warning, TEXT("[BPFactory] ApplyStateMachineDefinition failed: entry state missing"));
			return false;
		}

		EnsureDeclaredVariables(AnimBlueprint, Definition.Variables);
		CompileBlueprint(AnimBlueprint);

		UEdGraph* AnimGraph = FindAnimGraph(AnimBlueprint);
		if (!AnimGraph)
		{
			UE_LOG(LogTemp, Warning, TEXT("[BPFactory] ApplyStateMachineDefinition failed: AnimGraph not found on %s"), *AnimBlueprint->GetName());
			return false;
		}

		const FName MachineName = Definition.StateMachineName.IsNone() ? TEXT("StateMachine") : Definition.StateMachineName;
		UAnimGraphNode_StateMachineBase* StateMachineNode = EnsureStateMachineNode(AnimBlueprint, AnimGraph, MachineName);
		UAnimationStateMachineGraph* StateMachineGraph = StateMachineNode ? StateMachineNode->EditorStateMachineGraph.Get() : nullptr;
		if (!StateMachineNode || !StateMachineGraph)
		{
			UE_LOG(LogTemp, Warning, TEXT("[BPFactory] ApplyStateMachineDefinition failed: state machine node unavailable"));
			return false;
		}

		ConnectStateMachineToOutput(AnimBlueprint, AnimGraph, StateMachineNode, Definition);
		ClearStateMachineGraph(AnimBlueprint, StateMachineGraph);

		TMap<FName, UAnimStateNode*> CreatedStates;
		for (const FAnimStateMachineStateSpec& StateSpec : Definition.States)
		{
			if (StateSpec.StateName.IsNone())
			{
				continue;
			}

			UAnimStateNode* StateNode = CreateStateNode(AnimBlueprint, StateMachineGraph, StateSpec);
			if (!StateNode)
			{
				UE_LOG(LogTemp, Warning, TEXT("[BPFactory] ApplyStateMachineDefinition failed: state creation failed for %s"), *StateSpec.StateName.ToString());
				return false;
			}
			CreatedStates.Add(StateSpec.StateName, StateNode);
		}

		UAnimStateEntryNode* EntryNode = FindEntryNode(StateMachineGraph);
		UAnimStateNode* EntryStateNode = FindStateNodeByName(CreatedStates, Definition.EntryState);
		if (!EntryNode || !EntryStateNode)
		{
			UE_LOG(LogTemp, Warning, TEXT("[BPFactory] ApplyStateMachineDefinition failed: entry node or entry state missing"));
			return false;
		}

		if (UEdGraphPin* EntryOutput = FindFirstPin(EntryNode, EGPD_Output))
		{
			EntryOutput->BreakAllPinLinks();
			StateMachineGraph->GetSchema()->TryCreateConnection(EntryOutput, EntryStateNode->GetInputPin());
		}

		for (const FAnimStateMachineTransitionSpec& TransitionSpec : Definition.Transitions)
		{
			UAnimStateNode* FromStateNode = FindStateNodeByName(CreatedStates, TransitionSpec.FromState);
			UAnimStateNode* ToStateNode = FindStateNodeByName(CreatedStates, TransitionSpec.ToState);
			if (!FromStateNode || !ToStateNode)
			{
				UE_LOG(LogTemp, Warning, TEXT("[BPFactory] ApplyStateMachineDefinition failed: transition references unknown state %s -> %s"),
					*TransitionSpec.FromState.ToString(),
					*TransitionSpec.ToState.ToString());
				return false;
			}

			if (TransitionSpec.ConditionVariable.IsNone())
			{
				UE_LOG(LogTemp, Warning, TEXT("[BPFactory] ApplyStateMachineDefinition failed: transition %s -> %s has no condition variable"),
					*TransitionSpec.FromState.ToString(),
					*TransitionSpec.ToState.ToString());
				return false;
			}

			UAnimStateTransitionNode* TransitionNode = CreateConditionalTransition(
				AnimBlueprint,
				StateMachineGraph,
				FromStateNode,
				ToStateNode,
				TransitionSpec,
				TransitionSpec.Priority,
				TransitionSpec.CrossfadeDuration);

			if (!TransitionNode)
			{
				UE_LOG(LogTemp, Warning, TEXT("[BPFactory] ApplyStateMachineDefinition failed: transition creation failed for %s -> %s"),
					*TransitionSpec.FromState.ToString(),
					*TransitionSpec.ToState.ToString());
				return false;
			}
		}

		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(AnimBlueprint);
		CompileBlueprint(AnimBlueprint);
		return true;
	}
}

// 地形创建：UE 5.7 的 ALandscapeProxy::Import 从零创建会崩溃
// 暂时只打印参数提示，由用户在编辑器中手动创建 Landscape
// TODO: 研究通过 FEdModeLandscape UISettings 创建

AActor* UBPFactoryBlueprintLibrary::CreateFlatLandscape(
	UObject* WorldContext,
	FVector Location, int32 SizeX, int32 SizeY,
	float ScaleX, float ScaleY, float ScaleZ)
{
	UE_LOG(LogTemp, Log, TEXT("[BPFactory] 请在编辑器中手动创建 Landscape:"));
	UE_LOG(LogTemp, Log, TEXT("[BPFactory]   位置: (%f, %f, %f)"), Location.X, Location.Y, Location.Z);
	UE_LOG(LogTemp, Log, TEXT("[BPFactory]   尺寸: %dx%d"), SizeX, SizeY);
	UE_LOG(LogTemp, Log, TEXT("[BPFactory]   缩放: (%f, %f, %f)"), ScaleX, ScaleY, ScaleZ);
	UE_LOG(LogTemp, Log, TEXT("[BPFactory]   操作: Landscape 工具 → 新建 → 设置上述参数"));
	return nullptr;
}

AActor* UBPFactoryBlueprintLibrary::CreateLandscapeFromFile(
	UObject* WorldContext,
	FVector Location, int32 SizeX, int32 SizeY,
	float ScaleX, float ScaleY, float ScaleZ,
	const FString& HeightmapPath)
{
	UE_LOG(LogTemp, Log, TEXT("[BPFactory] 请在编辑器中手动导入 Landscape:"));
	UE_LOG(LogTemp, Log, TEXT("[BPFactory]   高度图: %s"), *HeightmapPath);
	UE_LOG(LogTemp, Log, TEXT("[BPFactory]   位置: (%f, %f, %f)"), Location.X, Location.Y, Location.Z);
	UE_LOG(LogTemp, Log, TEXT("[BPFactory]   尺寸: %dx%d"), SizeX, SizeY);
	UE_LOG(LogTemp, Log, TEXT("[BPFactory]   缩放: (%f, %f, %f)"), ScaleX, ScaleY, ScaleZ);
	UE_LOG(LogTemp, Log, TEXT("[BPFactory]   操作: Landscape 工具 → 导入 → 选择高度图 → 设置上述参数"));
	return nullptr;
}

bool UBPFactoryBlueprintLibrary::IsUnLuaAvailable()
{
#if WITH_UNLUA
	return true;
#else
	return false;
#endif
}

bool UBPFactoryBlueprintLibrary::SetupUnLuaBinding(UBlueprint* Blueprint, const FString& ModuleName)
{
#if WITH_UNLUA
	if (!Blueprint)
	{
		UE_LOG(LogTemp, Warning, TEXT("[BPFactory] SetupUnLuaBinding 失败: Blueprint 为空"));
		return false;
	}

	if (ModuleName.IsEmpty())
	{
		UE_LOG(LogTemp, Warning, TEXT("[BPFactory] SetupUnLuaBinding 失败: ModuleName 为空"));
		return false;
	}

	UClass* UnLuaInterfaceClass = UUnLuaInterface::StaticClass();
	if (!UnLuaInterfaceClass)
	{
		UE_LOG(LogTemp, Warning, TEXT("[BPFactory] SetupUnLuaBinding 失败: UnLuaInterface 不可用"));
		return false;
	}

	bool bAlreadyImplemented = false;
	for (const FBPInterfaceDescription& Iface : Blueprint->ImplementedInterfaces)
	{
		if (Iface.Interface == UnLuaInterfaceClass)
		{
			bAlreadyImplemented = true;
			break;
		}
	}

	if (!bAlreadyImplemented)
	{
		FBPInterfaceDescription InterfaceDesc;
		InterfaceDesc.Interface = UnLuaInterfaceClass;
		Blueprint->ImplementedInterfaces.Add(InterfaceDesc);

#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 1
		FTopLevelAssetPath InterfacePath(UnLuaInterfaceClass->GetPackage()->GetFName(), UnLuaInterfaceClass->GetFName());
		FBlueprintEditorUtils::ImplementNewInterface(Blueprint, InterfacePath);
#else
		FBlueprintEditorUtils::ImplementNewInterface(Blueprint, FName(*UnLuaInterfaceClass->GetPathName()));
#endif
		UE_LOG(LogTemp, Log, TEXT("[BPFactory] 已添加 UnLuaInterface: %s"), *Blueprint->GetName());
	}

	FKismetEditorUtilities::CompileBlueprint(Blueprint);

	TArray<UEdGraph*> AllGraphs;
	AllGraphs.Append(Blueprint->FunctionGraphs);
	for (FBPInterfaceDescription& InterfaceDesc : Blueprint->ImplementedInterfaces)
	{
		AllGraphs.Append(InterfaceDesc.Graphs);
	}

	bool bFoundAndSet = false;
	for (UEdGraph* Graph : AllGraphs)
	{
		if (!Graph || !Graph->GetFName().ToString().Contains(TEXT("GetModuleName")))
		{
			continue;
		}

		for (UEdGraphNode* Node : Graph->Nodes)
		{
			UK2Node_FunctionResult* ResultNode = Cast<UK2Node_FunctionResult>(Node);
			if (!ResultNode)
			{
				continue;
			}

			for (UEdGraphPin* Pin : ResultNode->Pins)
			{
				if (Pin->Direction == EGPD_Input && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_String)
				{
					Pin->DefaultValue = ModuleName;
					Pin->DefaultTextValue = FText::GetEmpty();
					bFoundAndSet = true;
					UE_LOG(LogTemp, Log, TEXT("[BPFactory] 设置 GetModuleName 返回值: %s"), *ModuleName);
				}
			}

			if (bFoundAndSet)
			{
				ResultNode->Modify();
				break;
			}
		}

		if (bFoundAndSet)
		{
			Graph->Modify();
			break;
		}
	}

	if (!bFoundAndSet)
	{
		UE_LOG(LogTemp, Warning, TEXT("[BPFactory] 未找到 GetModuleName 返回引脚: %s"), *Blueprint->GetName());
		return false;
	}

	FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
	FKismetEditorUtilities::CompileBlueprint(Blueprint);
	return true;
#else
	if (!ModuleName.IsEmpty())
	{
		UE_LOG(LogTemp, Warning, TEXT("[BPFactory] JSON 中配置了 UnLua 绑定，但 UnLua 插件未安装，已跳过"));
	}
	return false;
#endif
}

bool UBPFactoryBlueprintLibrary::ResetBlueprintForRegeneration(
	UBlueprint* Blueprint,
	bool bClearGraphs,
	bool bClearVariables)
{
	if (!Blueprint)
	{
		UE_LOG(LogTemp, Warning, TEXT("[BPFactory] ResetBlueprintForRegeneration failed: Blueprint is null"));
		return false;
	}

	ClearBlueprintOwnedComponents(Blueprint);

	if (bClearGraphs)
	{
		TArray<UEdGraph*> GraphsToClear;
		GraphsToClear.Append(Blueprint->UbergraphPages);
		GraphsToClear.Append(Blueprint->FunctionGraphs);
		GraphsToClear.Append(Blueprint->MacroGraphs);

		for (UEdGraph* Graph : GraphsToClear)
		{
			ClearK2Graph(Blueprint, Graph);
		}
	}

	if (bClearVariables)
	{
		ClearBlueprintOwnedVariables(Blueprint);
	}

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
	return true;
}

bool UBPFactoryBlueprintLibrary::ResetAnimBlueprintForRegeneration(
	UAnimBlueprint* AnimBlueprint,
	bool bClearVariables)
{
	if (!AnimBlueprint)
	{
		UE_LOG(LogTemp, Warning, TEXT("[BPFactory] ResetAnimBlueprintForRegeneration failed: AnimBlueprint is null"));
		return false;
	}

	if (bClearVariables)
	{
		ClearBlueprintOwnedVariables(AnimBlueprint);
	}

	TArray<UEdGraph*> GraphsToClear;
	GraphsToClear.Append(AnimBlueprint->UbergraphPages);
	GraphsToClear.Append(AnimBlueprint->FunctionGraphs);
	GraphsToClear.Append(AnimBlueprint->MacroGraphs);
	for (UEdGraph* Graph : GraphsToClear)
	{
		ClearK2Graph(AnimBlueprint, Graph);
	}

	if (UEdGraph* AnimGraph = FindAnimGraph(AnimBlueprint))
	{
		TArray<UEdGraphNode*> NodesToRemove;
		for (UEdGraphNode* Node : AnimGraph->Nodes)
		{
			if (!Node || Node->IsA(UAnimGraphNode_Root::StaticClass()))
			{
				continue;
			}
			NodesToRemove.Add(Node);
		}

		for (UEdGraphNode* Node : NodesToRemove)
		{
			FBlueprintEditorUtils::RemoveNode(AnimBlueprint, Node, true);
		}
	}

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(AnimBlueprint);
	return true;
}

bool UBPFactoryBlueprintLibrary::SetupQuadrupedBeastStateMachine(
	UAnimBlueprint* AnimBlueprint,
	const FString& StateMachineName,
	const FString& IdleAnimationAssetPath,
	const FString& WalkAnimationAssetPath,
	const FString& SitAnimationAssetPath,
	const FString& SleepAnimationAssetPath,
	const FString& SniffAnimationAssetPath)
{
	FAnimStateMachineDefinition Definition;
	Definition.StateMachineName = StateMachineName.IsEmpty() ? TEXT("QuadrupedLocomotion") : FName(*StateMachineName);
	Definition.EntryState = TEXT("Idle");
	Definition.Variables = {
		{TEXT("Speed"), TEXT("float")},
		{TEXT("Direction"), TEXT("float")},
		{TEXT("MoveAlpha"), TEXT("float")},
		{TEXT("IdleAlpha"), TEXT("float")},
		{TEXT("bIsMoving"), TEXT("bool")},
		{TEXT("bIsGrounded"), TEXT("bool")},
		{TEXT("bIsAirborne"), TEXT("bool")},
		{TEXT("bIsSitting"), TEXT("bool")},
		{TEXT("bIsSleeping"), TEXT("bool")},
		{TEXT("bIsSniffing"), TEXT("bool")},
		{TEXT("bIsAlert"), TEXT("bool")}
	};

	Definition.States = {
		{TEXT("Idle"), IdleAnimationAssetPath, FVector2f(80.0f, -80.0f)},
		{TEXT("Walk"), WalkAnimationAssetPath, FVector2f(420.0f, -80.0f)},
		{TEXT("Sit"), SitAnimationAssetPath, FVector2f(80.0f, 220.0f)},
		{TEXT("Sleep"), SleepAnimationAssetPath, FVector2f(-240.0f, 220.0f)},
		{TEXT("Sniff"), SniffAnimationAssetPath, FVector2f(420.0f, 220.0f)}
	};

	Definition.Transitions = {
		{TEXT("Idle"), TEXT("Walk"), TEXT("bIsMoving"), true, 3, DefaultCrossfade},
		{TEXT("Walk"), TEXT("Idle"), TEXT("bIsMoving"), false, 0, DefaultCrossfade},
		{TEXT("Idle"), TEXT("Sit"), TEXT("bIsSitting"), true, 2, DefaultCrossfade},
		{TEXT("Sit"), TEXT("Idle"), TEXT("bIsSitting"), false, 0, DefaultCrossfade},
		{TEXT("Idle"), TEXT("Sleep"), TEXT("bIsSleeping"), true, 0, DefaultCrossfade},
		{TEXT("Sleep"), TEXT("Idle"), TEXT("bIsSleeping"), false, 0, DefaultCrossfade},
		{TEXT("Idle"), TEXT("Sniff"), TEXT("bIsSniffing"), true, 1, DefaultCrossfade},
		{TEXT("Sniff"), TEXT("Idle"), TEXT("bIsSniffing"), false, 0, DefaultCrossfade}
	};

	return ApplyStateMachineDefinition(AnimBlueprint, Definition);
}

bool UBPFactoryBlueprintLibrary::SetupAnimStateMachineFromJson(
	UAnimBlueprint* AnimBlueprint,
	const FString& DefinitionJson)
{
	if (!AnimBlueprint)
	{
		UE_LOG(LogTemp, Warning, TEXT("[BPFactory] SetupAnimStateMachineFromJson failed: AnimBlueprint is null"));
		return false;
	}

	FAnimStateMachineDefinition Definition;
	if (!ParseStateMachineDefinitionJson(DefinitionJson, Definition))
	{
		UE_LOG(LogTemp, Warning, TEXT("[BPFactory] SetupAnimStateMachineFromJson failed: invalid definition json"));
		return false;
	}

	return ApplyStateMachineDefinition(AnimBlueprint, Definition);
}

bool UBPFactoryBlueprintLibrary::SetupAnimAssetOverridesFromJson(
	UAnimBlueprint* AnimBlueprint,
	const FString& OverridesJson)
{
	if (!AnimBlueprint)
	{
		UE_LOG(LogTemp, Warning, TEXT("[BPFactory] SetupAnimAssetOverridesFromJson failed: AnimBlueprint is null"));
		return false;
	}

	TArray<FAnimAssetOverrideSpec> OverrideSpecs;
	if (!ParseAnimationOverridesJson(OverridesJson, OverrideSpecs))
	{
		UE_LOG(LogTemp, Warning, TEXT("[BPFactory] SetupAnimAssetOverridesFromJson failed: invalid overrides json"));
		return false;
	}

	UAnimBlueprint* RootAnimBlueprint = UAnimBlueprint::FindRootAnimBlueprint(AnimBlueprint);
	if (!RootAnimBlueprint)
	{
		RootAnimBlueprint = AnimBlueprint;
	}

	const TMap<FName, FGuid> StateToGuidMap = BuildStateToPlayerNodeGuidMap(RootAnimBlueprint);
	TMap<FGuid, FAnimParentNodeAssetOverride> PreservedOverrides;
	for (const FAnimParentNodeAssetOverride& ExistingOverride : AnimBlueprint->ParentAssetOverrides)
	{
		PreservedOverrides.Add(ExistingOverride.ParentNodeGuid, ExistingOverride);
	}

	TArray<FGuid> KnownGuids;
	StateToGuidMap.GenerateValueArray(KnownGuids);
	for (const FGuid& KnownGuid : KnownGuids)
	{
		PreservedOverrides.Remove(KnownGuid);
	}

	for (const FAnimAssetOverrideSpec& OverrideSpec : OverrideSpecs)
	{
		if (OverrideSpec.StateName.IsNone())
		{
			continue;
		}

		const FGuid* ParentNodeGuid = StateToGuidMap.Find(OverrideSpec.StateName);
		if (!ParentNodeGuid || !ParentNodeGuid->IsValid())
		{
			UE_LOG(LogTemp, Warning, TEXT("[BPFactory] Anim override skipped: unknown state %s on %s"),
				*OverrideSpec.StateName.ToString(),
				*AnimBlueprint->GetName());
			continue;
		}

		if (OverrideSpec.AnimationAssetPath.IsEmpty())
		{
			continue;
		}

		UAnimationAsset* AnimationAsset = LoadEditorAsset<UAnimationAsset>(OverrideSpec.AnimationAssetPath);
		if (!AnimationAsset)
		{
			UE_LOG(LogTemp, Warning, TEXT("[BPFactory] Anim override skipped: missing animation asset %s"),
				*OverrideSpec.AnimationAssetPath);
			continue;
		}

		FAnimParentNodeAssetOverride NewOverride;
		NewOverride.ParentNodeGuid = *ParentNodeGuid;
		NewOverride.NewAsset = AnimationAsset;
		PreservedOverrides.Add(NewOverride.ParentNodeGuid, NewOverride);
	}

	AnimBlueprint->Modify();
	AnimBlueprint->ParentAssetOverrides.Reset();
	TArray<FGuid> SortedGuids;
	PreservedOverrides.GetKeys(SortedGuids);
	SortedGuids.Sort([](const FGuid& A, const FGuid& B)
	{
		return A.ToString(EGuidFormats::Digits) < B.ToString(EGuidFormats::Digits);
	});
	for (const FGuid& Guid : SortedGuids)
	{
		if (const FAnimParentNodeAssetOverride* Override = PreservedOverrides.Find(Guid))
		{
			AnimBlueprint->ParentAssetOverrides.Add(*Override);
		}
	}

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(AnimBlueprint);
	CompileBlueprint(AnimBlueprint);
	return true;
}

bool UBPFactoryBlueprintLibrary::SetAnimBlueprintPreviewMesh(
	UAnimBlueprint* AnimBlueprint,
	USkeletalMesh* PreviewMesh,
	bool bMarkDirty)
{
	if (!AnimBlueprint || !PreviewMesh)
	{
		UE_LOG(LogTemp, Warning, TEXT("[BPFactory] SetAnimBlueprintPreviewMesh failed: invalid input"));
		return false;
	}

	AnimBlueprint->Modify();
	AnimBlueprint->SetPreviewMesh(PreviewMesh, bMarkDirty);
	if (bMarkDirty)
	{
		AnimBlueprint->MarkPackageDirty();
	}
	return true;
}

FString UBPFactoryBlueprintLibrary::ExportAnimBlueprintMetadataToJson(
	UAnimBlueprint* AnimBlueprint)
{
	if (!AnimBlueprint)
	{
		return TEXT("{}");
	}

	TSharedPtr<FJsonObject> RootObject = MakeShared<FJsonObject>();
	UAnimBlueprint* RootAnimBlueprint = UAnimBlueprint::FindRootAnimBlueprint(AnimBlueprint);
	if (!RootAnimBlueprint)
	{
		RootAnimBlueprint = AnimBlueprint;
	}

	if (RootAnimBlueprint == AnimBlueprint)
	{
		if (TSharedPtr<FJsonObject> StateMachineDefinition = BuildStateMachineDefinitionObject(AnimBlueprint))
		{
			RootObject->SetObjectField(TEXT("StateMachineDefinition"), StateMachineDefinition);
		}
	}

	const TArray<TSharedPtr<FJsonValue>> AnimationOverrides = BuildAnimationOverridesArray(AnimBlueprint);
	if (AnimationOverrides.Num() > 0)
	{
		RootObject->SetArrayField(TEXT("AnimationOverrides"), AnimationOverrides);
	}

	const FString UnLuaBinding = GetBlueprintUnLuaModuleName(AnimBlueprint);
	if (!UnLuaBinding.IsEmpty())
	{
		RootObject->SetStringField(TEXT("UnLuaBinding"), UnLuaBinding);
	}

	FString JsonString;
	const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&JsonString);
	FJsonSerializer::Serialize(RootObject.ToSharedRef(), Writer);
	return JsonString.IsEmpty() ? TEXT("{}") : JsonString;
}
