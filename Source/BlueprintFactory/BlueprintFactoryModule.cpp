#include "BlueprintFactoryModule.h"
#include "SBlueprintFactoryPanel.h"
#include "Widgets/Docking/SDockTab.h"
#include "ToolMenus.h"
#include "Framework/Docking/TabManager.h"

#define LOCTEXT_NAMESPACE "BlueprintFactory"

static const FName TabId("BlueprintFactoryTab");

void FBlueprintFactoryModule::StartupModule()
{
	FGlobalTabmanager::Get()->RegisterNomadTabSpawner(TabId,
		FOnSpawnTab::CreateRaw(this, &FBlueprintFactoryModule::SpawnTab))
		.SetDisplayName(LOCTEXT("TabTitle", "Blueprint Factory"))
		.SetMenuType(ETabSpawnerMenuType::Hidden);

	UToolMenus::RegisterStartupCallback(FSimpleMulticastDelegate::FDelegate::CreateRaw(this, &FBlueprintFactoryModule::RegisterMenus));
}

void FBlueprintFactoryModule::ShutdownModule()
{
	FGlobalTabmanager::Get()->UnregisterNomadTabSpawner(TabId);
}

void FBlueprintFactoryModule::RegisterMenus()
{
	UToolMenu* Menu = UToolMenus::Get()->ExtendMenu("LevelEditor.MainMenu.Tools");
	FToolMenuSection& Section = Menu->FindOrAddSection("BlueprintFactory");
	Section.AddMenuEntry(
		"OpenBlueprintFactory",
		LOCTEXT("MenuLabel", "Blueprint Factory"),
		LOCTEXT("MenuTooltip", "JSON 模板驱动的蓝图/材质/地形生成工具"),
		FSlateIcon(),
		FUIAction(FExecuteAction::CreateLambda([]()
		{
			FGlobalTabmanager::Get()->TryInvokeTab(TabId);
		}))
	);
}

TSharedRef<SDockTab> FBlueprintFactoryModule::SpawnTab(const FSpawnTabArgs& Args)
{
	return SNew(SDockTab)
		.TabRole(ETabRole::NomadTab)
		[
			SNew(SBlueprintFactoryPanel)
		];
}

IMPLEMENT_MODULE(FBlueprintFactoryModule, BlueprintFactory)

#undef LOCTEXT_NAMESPACE
