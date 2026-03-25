#pragma once
#include "Widgets/SCompoundWidget.h"
#include "Widgets/Input/SEditableTextBox.h"

class SBlueprintFactoryPanel : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SBlueprintFactoryPanel) {}
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

private:
	// 通用
	void RunPython(const FString& Code);
	void* GetParentWindow();
	void UpdateStatus(const FString& Text);

	// 蓝图标签页
	FReply OnBrowseBlueprintJson();
	FReply OnGenerateBlueprint();

	// 材质标签页
	FReply OnBrowseMaterialJson();
	FReply OnGenerateMaterial();

	// 地形标签页
	FReply OnBrowseLandscapeJson();
	FReply OnGenerateLandscape();

	// 控件
	TSharedPtr<SEditableTextBox> BlueprintJsonPath;
	TSharedPtr<SEditableTextBox> MaterialJsonPath;
	TSharedPtr<SEditableTextBox> LandscapeJsonPath;
	TSharedPtr<STextBlock> StatusText;
};
