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

	// 关卡标签页
	FReply OnBrowseLevelJson();
	FReply OnGenerateLevel();
	FReply OnExportLevel();

	// 蓝图标签页
	FReply OnBrowseBlueprintJson();
	FReply OnGenerateBlueprint();
	FReply OnExportBlueprint();

	// 材质标签页
	FReply OnBrowseMaterialJson();
	FReply OnGenerateMaterial();
	FReply OnExportMaterial();

	// 地形标签页
	FReply OnBrowseLandscapeJson();
	FReply OnGenerateLandscape();

	// 控件
	TSharedPtr<SEditableTextBox> LevelJsonPath;
	TSharedPtr<SEditableTextBox> ExportLevelPath;
	TSharedPtr<SEditableTextBox> BlueprintJsonPath;
	TSharedPtr<SEditableTextBox> ExportBPAssetPath;
	TSharedPtr<SEditableTextBox> ExportBPJsonPath;
	TSharedPtr<SEditableTextBox> MaterialJsonPath;
	TSharedPtr<SEditableTextBox> ExportMatAssetPath;
	TSharedPtr<SEditableTextBox> ExportMatJsonPath;
	TSharedPtr<SEditableTextBox> LandscapeJsonPath;
	TSharedPtr<STextBlock> StatusText;
};
