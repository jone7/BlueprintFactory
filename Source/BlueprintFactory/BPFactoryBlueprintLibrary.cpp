#include "BPFactoryBlueprintLibrary.h"
#include "Animation/AnimBlueprint.h"
#include "Animation/AnimationAsset.h"
#include "AnimGraphNode_Root.h"
#include "AnimGraphNode_SequencePlayer.h"
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
#include "K2Node_FunctionResult.h"
#include "K2Node_VariableGet.h"
#include "Kismet/KismetMathLibrary.h"
#include "Misc/PackageName.h"

#if WITH_UNLUA
#include "UnLuaInterface.h"
#endif

namespace
{
	constexpr float DefaultCrossfade = 0.18f;

	struct FQuadrupedStateSpec
	{
		FName StateName;
		FString AnimationAssetPath;
		FName EnterBool;
		int32 Priority = 0;
		FVector2f NodePosition = FVector2f::ZeroVector;
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

	void EnsureMemberVariable(UBlueprint* Blueprint, const FName VarName, const FEdGraphPinType& PinType)
	{
		if (!Blueprint || HasMemberVariable(Blueprint, VarName))
		{
			return;
		}

		FBlueprintEditorUtils::AddMemberVariable(Blueprint, VarName, PinType);
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
			if (!Node || Node->IsA(UAnimGraphNode_Root::StaticClass()))
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

	UAnimStateNode* CreateStateNode(UBlueprint* Blueprint, UEdGraph* StateMachineGraph, const FQuadrupedStateSpec& Spec)
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

		if (UAnimationAsset* AnimationAsset = LoadEditorAsset<UAnimationAsset>(Spec.AnimationAssetPath))
		{
			if (UEdGraphPin* PosePin = StateNode->GetPoseSinkPinInsideState())
			{
				FGraphNodeCreator<UAnimGraphNode_SequencePlayer> PlayerCreator(*StateNode->BoundGraph);
				UAnimGraphNode_SequencePlayer* PlayerNode = PlayerCreator.CreateNode();
				PlayerCreator.Finalize();
				if (PlayerNode)
				{
					PlayerNode->NodePosX = -320;
					PlayerNode->NodePosY = 0;
					PlayerNode->SetAnimationAsset(AnimationAsset);

					if (UEdGraphPin* PlayerOutput = FindFirstPin(PlayerNode, EGPD_Output))
					{
						StateNode->BoundGraph->GetSchema()->TryCreateConnection(PlayerOutput, PosePin);
					}
				}
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
		const int32 PriorityOrder)
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
		TransitionNode->CrossfadeDuration = DefaultCrossfade;
		TransitionNode->bAutomaticRuleBasedOnSequencePlayerInState = false;
		ConfigureTransitionRule(Blueprint, TransitionNode->BoundGraph, BoolVarName, bExpectedValue);
		return TransitionNode;
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

bool UBPFactoryBlueprintLibrary::SetupQuadrupedBeastStateMachine(
	UAnimBlueprint* AnimBlueprint,
	const FString& StateMachineName,
	const FString& IdleAnimationAssetPath,
	const FString& WalkAnimationAssetPath,
	const FString& SitAnimationAssetPath,
	const FString& SleepAnimationAssetPath,
	const FString& SniffAnimationAssetPath)
{
	if (!AnimBlueprint)
	{
		UE_LOG(LogTemp, Warning, TEXT("[BPFactory] SetupQuadrupedBeastStateMachine failed: AnimBlueprint is null"));
		return false;
	}

	if (IdleAnimationAssetPath.IsEmpty() || WalkAnimationAssetPath.IsEmpty())
	{
		UE_LOG(LogTemp, Warning, TEXT("[BPFactory] SetupQuadrupedBeastStateMachine failed: Idle/Walk animations are required"));
		return false;
	}

	EnsureQuadrupedVariables(AnimBlueprint);
	CompileBlueprint(AnimBlueprint);

	UEdGraph* AnimGraph = FindAnimGraph(AnimBlueprint);
	if (!AnimGraph)
	{
		UE_LOG(LogTemp, Warning, TEXT("[BPFactory] SetupQuadrupedBeastStateMachine failed: AnimGraph not found on %s"), *AnimBlueprint->GetName());
		return false;
	}

	const FName MachineName = StateMachineName.IsEmpty() ? TEXT("QuadrupedLocomotion") : FName(*StateMachineName);
	UAnimGraphNode_StateMachineBase* StateMachineNode = EnsureStateMachineNode(AnimBlueprint, AnimGraph, MachineName);
	UAnimationStateMachineGraph* StateMachineGraph = StateMachineNode ? StateMachineNode->EditorStateMachineGraph.Get() : nullptr;
	if (!StateMachineNode || !StateMachineGraph)
	{
		UE_LOG(LogTemp, Warning, TEXT("[BPFactory] SetupQuadrupedBeastStateMachine failed: state machine node unavailable"));
		return false;
	}

	ConnectStateMachineToRoot(AnimBlueprint, AnimGraph, StateMachineNode);
	ClearStateMachineGraph(AnimBlueprint, StateMachineGraph);

	TArray<FQuadrupedStateSpec> StateSpecs;
	StateSpecs.Add({TEXT("Idle"), IdleAnimationAssetPath, NAME_None, 0, FVector2f(80.0f, -80.0f)});
	StateSpecs.Add({TEXT("Walk"), WalkAnimationAssetPath, TEXT("bIsMoving"), 3, FVector2f(420.0f, -80.0f)});
	if (!SitAnimationAssetPath.IsEmpty())
	{
		StateSpecs.Add({TEXT("Sit"), SitAnimationAssetPath, TEXT("bIsSitting"), 2, FVector2f(80.0f, 220.0f)});
	}
	if (!SleepAnimationAssetPath.IsEmpty())
	{
		StateSpecs.Add({TEXT("Sleep"), SleepAnimationAssetPath, TEXT("bIsSleeping"), 0, FVector2f(-240.0f, 220.0f)});
	}
	if (!SniffAnimationAssetPath.IsEmpty())
	{
		StateSpecs.Add({TEXT("Sniff"), SniffAnimationAssetPath, TEXT("bIsSniffing"), 1, FVector2f(420.0f, 220.0f)});
	}

	TMap<FName, UAnimStateNode*> CreatedStates;
	for (const FQuadrupedStateSpec& Spec : StateSpecs)
	{
		UAnimStateNode* StateNode = CreateStateNode(AnimBlueprint, StateMachineGraph, Spec);
		if (!StateNode)
		{
			UE_LOG(LogTemp, Warning, TEXT("[BPFactory] Failed to create state %s"), *Spec.StateName.ToString());
			return false;
		}
		CreatedStates.Add(Spec.StateName, StateNode);
	}

	UAnimStateEntryNode* EntryNode = FindEntryNode(StateMachineGraph);
	UAnimStateNode* IdleState = CreatedStates.FindRef(TEXT("Idle"));
	if (!EntryNode || !IdleState)
	{
		UE_LOG(LogTemp, Warning, TEXT("[BPFactory] SetupQuadrupedBeastStateMachine failed: entry or idle state missing"));
		return false;
	}

	StateMachineGraph->GetSchema()->TryCreateConnection(FindFirstPin(EntryNode, EGPD_Output), IdleState->GetInputPin());

	UAnimStateNode* WalkState = CreatedStates.FindRef(TEXT("Walk"));
	if (WalkState)
	{
		CreateConditionalTransition(AnimBlueprint, StateMachineGraph, IdleState, WalkState, TEXT("bIsMoving"), true, 3);
		CreateConditionalTransition(AnimBlueprint, StateMachineGraph, WalkState, IdleState, TEXT("bIsMoving"), false, 0);
	}

	for (const FQuadrupedStateSpec& Spec : StateSpecs)
	{
		if (Spec.StateName == TEXT("Idle") || Spec.StateName == TEXT("Walk") || Spec.EnterBool.IsNone())
		{
			continue;
		}

		if (UAnimStateNode* StateNode = CreatedStates.FindRef(Spec.StateName))
		{
			CreateConditionalTransition(AnimBlueprint, StateMachineGraph, IdleState, StateNode, Spec.EnterBool, true, Spec.Priority);
			CreateConditionalTransition(AnimBlueprint, StateMachineGraph, StateNode, IdleState, Spec.EnterBool, false, 0);
		}
	}

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(AnimBlueprint);
	CompileBlueprint(AnimBlueprint);
	return true;
}
