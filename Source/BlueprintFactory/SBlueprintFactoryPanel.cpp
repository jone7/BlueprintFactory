#include "SBlueprintFactoryPanel.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Layout/SSeparator.h"
#include "Widgets/Input/SButton.h"
#include "DesktopPlatformModule.h"
#include "IPythonScriptPlugin.h"
#include "Framework/Application/SlateApplication.h"

#define LOCTEXT_NAMESPACE "BlueprintFactoryPanel"

void SBlueprintFactoryPanel::Construct(const FArguments& InArgs)
{
	ChildSlot
	[
		SNew(SScrollBox)
		+ SScrollBox::Slot().Padding(12.f)
		[
			SNew(SVerticalBox)

			// ===== 标题 =====
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 8)
			[
				SNew(STextBlock)
				.Text(LOCTEXT("Title", "Blueprint Factory"))
				.Font(FCoreStyle::GetDefaultFontStyle("Bold", 18))
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 4)
			[
				SNew(SSeparator)
			]

			// ===== 蓝图生成 =====
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 8, 0, 4)
			[
				SNew(STextBlock)
				.Text(LOCTEXT("BPTitle", "蓝图生成"))
				.Font(FCoreStyle::GetDefaultFontStyle("Bold", 14))
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 4, 0, 0)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
				[
					SNew(STextBlock).Text(LOCTEXT("BPJson", "JSON:"))
				]
				+ SHorizontalBox::Slot().FillWidth(1.f).Padding(8, 0, 0, 0)
				[
					SAssignNew(BlueprintJsonPath, SEditableTextBox)
					.HintText(LOCTEXT("BPHint", "BlueprintTemplates/*.json"))
				]
				+ SHorizontalBox::Slot().AutoWidth().Padding(4, 0, 0, 0)
				[
					SNew(SButton)
					.Text(LOCTEXT("Browse", "浏览..."))
					.OnClicked(this, &SBlueprintFactoryPanel::OnBrowseBlueprintJson)
				]
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 4, 0, 0)
			[
				SNew(SButton)
				.Text(LOCTEXT("GenBP", "生成蓝图"))
				.OnClicked(this, &SBlueprintFactoryPanel::OnGenerateBlueprint)
			]

			+ SVerticalBox::Slot().AutoHeight().Padding(0, 12, 0, 4)
			[
				SNew(SSeparator)
			]

			// ===== 材质生成 =====
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 4, 0, 4)
			[
				SNew(STextBlock)
				.Text(LOCTEXT("MatTitle", "材质生成"))
				.Font(FCoreStyle::GetDefaultFontStyle("Bold", 14))
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 4, 0, 0)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
				[
					SNew(STextBlock).Text(LOCTEXT("MatJson", "JSON:"))
				]
				+ SHorizontalBox::Slot().FillWidth(1.f).Padding(8, 0, 0, 0)
				[
					SAssignNew(MaterialJsonPath, SEditableTextBox)
					.HintText(LOCTEXT("MatHint", "MaterialTemplates/*.json"))
				]
				+ SHorizontalBox::Slot().AutoWidth().Padding(4, 0, 0, 0)
				[
					SNew(SButton)
					.Text(LOCTEXT("Browse2", "浏览..."))
					.OnClicked(this, &SBlueprintFactoryPanel::OnBrowseMaterialJson)
				]
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 4, 0, 0)
			[
				SNew(SButton)
				.Text(LOCTEXT("GenMat", "生成材质"))
				.OnClicked(this, &SBlueprintFactoryPanel::OnGenerateMaterial)
			]

			+ SVerticalBox::Slot().AutoHeight().Padding(0, 12, 0, 4)
			[
				SNew(SSeparator)
			]

			// ===== 地形导入 =====
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 4, 0, 4)
			[
				SNew(STextBlock)
				.Text(LOCTEXT("LandTitle", "地形导入"))
				.Font(FCoreStyle::GetDefaultFontStyle("Bold", 14))
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 4, 0, 0)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
				[
					SNew(STextBlock).Text(LOCTEXT("LandJson", "JSON:"))
				]
				+ SHorizontalBox::Slot().FillWidth(1.f).Padding(8, 0, 0, 0)
				[
					SAssignNew(LandscapeJsonPath, SEditableTextBox)
					.HintText(LOCTEXT("LandHint", "LandscapeTemplates/*.json"))
				]
				+ SHorizontalBox::Slot().AutoWidth().Padding(4, 0, 0, 0)
				[
					SNew(SButton)
					.Text(LOCTEXT("Browse3", "浏览..."))
					.OnClicked(this, &SBlueprintFactoryPanel::OnBrowseLandscapeJson)
				]
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 4, 0, 0)
			[
				SNew(SButton)
				.Text(LOCTEXT("GenLand", "导入地形"))
				.OnClicked(this, &SBlueprintFactoryPanel::OnGenerateLandscape)
			]

			+ SVerticalBox::Slot().AutoHeight().Padding(0, 12, 0, 4)
			[
				SNew(SSeparator)
			]

			// ===== 状态 =====
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 8, 0, 0)
			[
				SAssignNew(StatusText, STextBlock)
				.Text(LOCTEXT("Ready", "就绪"))
				.AutoWrapText(true)
			]
		]
	];
}

void SBlueprintFactoryPanel::RunPython(const FString& Code)
{
	IPythonScriptPlugin::Get()->ExecPythonCommand(*Code);
}

void* SBlueprintFactoryPanel::GetParentWindow()
{
	return FSlateApplication::Get().GetActiveTopLevelWindow().IsValid()
		? FSlateApplication::Get().GetActiveTopLevelWindow()->GetNativeWindow()->GetOSWindowHandle()
		: nullptr;
}

void SBlueprintFactoryPanel::UpdateStatus(const FString& Text)
{
	if (StatusText.IsValid())
		StatusText->SetText(FText::FromString(Text));
}


// ===== 浏览按钮 =====

FReply SBlueprintFactoryPanel::OnBrowseBlueprintJson()
{
	IDesktopPlatform* Desktop = FDesktopPlatformModule::Get();
	if (!Desktop) return FReply::Handled();
	TArray<FString> Files;
	if (Desktop->OpenFileDialog(GetParentWindow(), TEXT("选择蓝图模板"), TEXT(""), TEXT(""),
		TEXT("JSON (*.json)|*.json"), 0, Files))
	{
		if (Files.Num() > 0 && BlueprintJsonPath.IsValid())
			BlueprintJsonPath->SetText(FText::FromString(Files[0]));
	}
	return FReply::Handled();
}

FReply SBlueprintFactoryPanel::OnBrowseMaterialJson()
{
	IDesktopPlatform* Desktop = FDesktopPlatformModule::Get();
	if (!Desktop) return FReply::Handled();
	TArray<FString> Files;
	if (Desktop->OpenFileDialog(GetParentWindow(), TEXT("选择材质模板"), TEXT(""), TEXT(""),
		TEXT("JSON (*.json)|*.json"), 0, Files))
	{
		if (Files.Num() > 0 && MaterialJsonPath.IsValid())
			MaterialJsonPath->SetText(FText::FromString(Files[0]));
	}
	return FReply::Handled();
}

FReply SBlueprintFactoryPanel::OnBrowseLandscapeJson()
{
	IDesktopPlatform* Desktop = FDesktopPlatformModule::Get();
	if (!Desktop) return FReply::Handled();
	TArray<FString> Files;
	if (Desktop->OpenFileDialog(GetParentWindow(), TEXT("选择地形模板"), TEXT(""), TEXT(""),
		TEXT("JSON (*.json)|*.json"), 0, Files))
	{
		if (Files.Num() > 0 && LandscapeJsonPath.IsValid())
			LandscapeJsonPath->SetText(FText::FromString(Files[0]));
	}
	return FReply::Handled();
}

// ===== 生成按钮 =====

FReply SBlueprintFactoryPanel::OnGenerateBlueprint()
{
	FString JsonPath = BlueprintJsonPath.IsValid() ? BlueprintJsonPath->GetText().ToString() : TEXT("");
	if (JsonPath.IsEmpty())
	{
		UpdateStatus(TEXT("请先选择蓝图 JSON 模板"));
		return FReply::Handled();
	}
	JsonPath.ReplaceInline(TEXT("\\"), TEXT("\\\\"));
	FString Py = FString::Printf(
		TEXT("from blueprint_factory.bp_generator import generate_blueprint; generate_blueprint(r'%s')"),
		*JsonPath);
	UpdateStatus(TEXT("生成蓝图中..."));
	RunPython(Py);
	UpdateStatus(TEXT("蓝图生成完成"));
	return FReply::Handled();
}

FReply SBlueprintFactoryPanel::OnGenerateMaterial()
{
	FString JsonPath = MaterialJsonPath.IsValid() ? MaterialJsonPath->GetText().ToString() : TEXT("");
	if (JsonPath.IsEmpty())
	{
		UpdateStatus(TEXT("请先选择材质 JSON 模板"));
		return FReply::Handled();
	}
	JsonPath.ReplaceInline(TEXT("\\"), TEXT("\\\\"));
	FString Py = FString::Printf(
		TEXT("from blueprint_factory.mat_generator import generate_material; generate_material(r'%s')"),
		*JsonPath);
	UpdateStatus(TEXT("生成材质中..."));
	RunPython(Py);
	UpdateStatus(TEXT("材质生成完成"));
	return FReply::Handled();
}

FReply SBlueprintFactoryPanel::OnGenerateLandscape()
{
	FString JsonPath = LandscapeJsonPath.IsValid() ? LandscapeJsonPath->GetText().ToString() : TEXT("");
	if (JsonPath.IsEmpty())
	{
		UpdateStatus(TEXT("请先选择地形 JSON 模板"));
		return FReply::Handled();
	}
	JsonPath.ReplaceInline(TEXT("\\"), TEXT("\\\\"));
	FString Py = FString::Printf(
		TEXT("from blueprint_factory.land_generator import generate_landscape; generate_landscape(r'%s')"),
		*JsonPath);
	UpdateStatus(TEXT("导入地形中..."));
	RunPython(Py);
	UpdateStatus(TEXT("地形导入完成"));
	return FReply::Handled();
}

#undef LOCTEXT_NAMESPACE
