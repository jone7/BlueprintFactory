#include "BPFactoryBlueprintLibrary.h"
#include "Animation/AnimBlueprint.h"
#include "Animation/AnimationAsset.h"
#include "AnimGraphNode_Root.h"
#include "AnimGraphNode_SequencePlayer.h"
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
	};

	struct FAnimStateMachineDefinition
	{
		FName StateMachineName = TEXT("StateMachine");
		FName EntryState;
		TArray<FAnimStateMachineVariableSpec> Variables;
		TArray<FAnimStateMachineStateSpec> States;
		TArray<FAnimStateMachineTransitionSpec> Transitions;
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

		return false;
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
					bool bExpectedValue = true;
					if ((*ConditionObject)->TryGetBoolField(TEXT("Value"), bExpectedValue))
					{
						TransitionSpec.bExpectedValue = bExpectedValue;
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

	void ConnectStateMachineToRoot(UBlueprint* Blueprint, UEdGraph* AnimGraph, UAnimGraphNode_StateMachineBase* StateMachineNode)
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

	void ConfigureTransitionRule(UBlueprint* Blueprint, UEdGraph* TransitionGraph, const FName BoolVarName, const bool bExpectedValue)
	{
		if (!Blueprint || !TransitionGraph || BoolVarName.IsNone())
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

		UClass* VariableOwnerClass = Blueprint->SkeletonGeneratedClass ? Blueprint->SkeletonGeneratedClass : Blueprint->GeneratedClass;
		if (!VariableOwnerClass)
		{
			return;
		}

		FProperty* Property = FindFProperty<FProperty>(VariableOwnerClass, BoolVarName);
		if (!Property)
		{
			UE_LOG(LogTemp, Warning, TEXT("[BPFactory] Missing bool variable for transition: %s"), *BoolVarName.ToString());
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

		if (bExpectedValue)
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
	}

	UAnimStateTransitionNode* CreateConditionalTransition(
		UBlueprint* Blueprint,
		UEdGraph* StateMachineGraph,
		UAnimStateNodeBase* FromState,
		UAnimStateNodeBase* ToState,
		const FName BoolVarName,
		const bool bExpectedValue,
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
		ConfigureTransitionRule(Blueprint, TransitionNode->BoundGraph, BoolVarName, bExpectedValue);
		return TransitionNode;
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

		ConnectStateMachineToRoot(AnimBlueprint, AnimGraph, StateMachineNode);
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
				TransitionSpec.ConditionVariable,
				TransitionSpec.bExpectedValue,
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
