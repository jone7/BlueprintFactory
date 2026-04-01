#include "BPFactoryBlueprintLibrary.h"
#include "Landscape.h"
#include "LandscapeProxy.h"
#include "LandscapeInfo.h"
#include "Engine/World.h"
#include "Editor.h"
#include "Misc/FileHelper.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "K2Node_FunctionResult.h"

#if WITH_UNLUA
#include "UnLuaInterface.h"
#endif

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
