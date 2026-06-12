#include "BlueprintFactoryModule.h"
#include "SBlueprintFactoryPanel.h"
#include "Widgets/Docking/SDockTab.h"
#include "ToolMenus.h"
#include "Framework/Docking/TabManager.h"
#include "IPythonScriptPlugin.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Dom/JsonObject.h"
#include "UObject/ObjectSaveContext.h"
#include "UObject/Package.h"
#include "Engine/Blueprint.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstanceConstant.h"

#define LOCTEXT_NAMESPACE "BlueprintFactory"

static const FName TabId("BlueprintFactoryTab");

namespace
{
const FString GSharedCharacterAnimTemplate = TEXT("Config/BlueprintTemplates/ABP_CharacterHumanoidChild.json");
const FString GSharedCharacterBlueprintTemplate = TEXT("Config/BlueprintTemplates/BP_CharacterHumanoidChild.json");

FString NormalizePackagePath(const FString& InPath)
{
	FString Normalized = InPath;
	Normalized.TrimStartAndEndInline();
	if (Normalized.IsEmpty())
	{
		return Normalized;
	}

	FString Left;
	FString Right;
	if (Normalized.Split(TEXT("."), &Left, &Right, ESearchCase::CaseSensitive, ESearchDir::FromEnd)
		&& Left.StartsWith(TEXT("/Game/")))
	{
		return Left;
	}

	return Normalized;
}

FString NormalizeProjectRelativePath(const FString& InPath)
{
	FString Normalized = InPath;
	Normalized.ReplaceInline(TEXT("\\"), TEXT("/"));
	Normalized.TrimStartAndEndInline();
	return Normalized;
}

FString ResolveFullJsonPath(const FString& JsonPath)
{
	const FString Normalized = NormalizeProjectRelativePath(JsonPath);
	if (Normalized.IsEmpty())
	{
		return FString();
	}

	if (FPaths::IsRelative(Normalized))
	{
		return FPaths::ConvertRelativePathToFull(FPaths::ProjectDir() / Normalized);
	}

	return FPaths::ConvertRelativePathToFull(Normalized);
}

FString BuildAssetObjectPath(const FString& PackagePath)
{
	const FString NormalizedPackagePath = NormalizePackagePath(PackagePath);
	const FString AssetName = FPaths::GetBaseFilename(NormalizedPackagePath);
	if (NormalizedPackagePath.IsEmpty() || AssetName.IsEmpty())
	{
		return FString();
	}

	return FString::Printf(TEXT("%s.%s"), *NormalizedPackagePath, *AssetName);
}

UObject* LoadSavedAsset(const FString& PackagePath)
{
	const FString ObjectPath = BuildAssetObjectPath(PackagePath);
	return ObjectPath.IsEmpty() ? nullptr : LoadObject<UObject>(nullptr, *ObjectPath);
}

bool IsClassNamedOrDerived(const UClass* Class, const TCHAR* TargetClassName)
{
	for (const UClass* Current = Class; Current; Current = Current->GetSuperClass())
	{
		if (Current->GetName() == TargetClassName)
		{
			return true;
		}
	}
	return false;
}

bool LoadJsonObjectFromFile(const FString& FilePath, TSharedPtr<FJsonObject>& OutJsonObject)
{
	OutJsonObject.Reset();

	FString JsonText;
	if (!FFileHelper::LoadFileToString(JsonText, *FilePath))
	{
		return false;
	}

	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonText);
	return FJsonSerializer::Deserialize(Reader, OutJsonObject) && OutJsonObject.IsValid();
}

FString ResolveJsonTemplatePathField(const TSharedPtr<FJsonObject>& IngredientObject)
{
	if (!IngredientObject.IsValid())
	{
		return FString();
	}

	FString Value;
	if (IngredientObject->TryGetStringField(TEXT("currentFilePath"), Value) && !Value.IsEmpty())
	{
		return Value;
	}
	if (IngredientObject->TryGetStringField(TEXT("defaultFilePath"), Value) && !Value.IsEmpty())
	{
		return Value;
	}
	if (IngredientObject->TryGetStringField(TEXT("filePath"), Value) && !Value.IsEmpty())
	{
		return Value;
	}
	return FString();
}

bool IsSharedCharacterTemplatePath(const FString& JsonPath)
{
	const FString Normalized = NormalizeProjectRelativePath(JsonPath);
	return Normalized == GSharedCharacterAnimTemplate || Normalized == GSharedCharacterBlueprintTemplate;
}

bool TryResolveJsonPathFromManifest(
	const FString& PackagePath,
	const TArray<FString>& SupportedGenerationTypes,
	FString& OutJsonPath)
{
	OutJsonPath.Reset();

	const FString MasterManifestPath = FPaths::ProjectConfigDir() / TEXT("AssetPipeline/AssetManifest.json");
	TSharedPtr<FJsonObject> MasterManifest;
	if (!LoadJsonObjectFromFile(MasterManifestPath, MasterManifest))
	{
		return false;
	}

	const TArray<TSharedPtr<FJsonValue>>* SubManifestValues = nullptr;
	if (!MasterManifest->TryGetArrayField(TEXT("subManifests"), SubManifestValues) || !SubManifestValues)
	{
		return false;
	}

	const FString NormalizedPackagePath = NormalizePackagePath(PackagePath);
	for (const TSharedPtr<FJsonValue>& SubManifestValue : *SubManifestValues)
	{
		FString RelativeSubManifestPath;
		if (!SubManifestValue.IsValid() || !SubManifestValue->TryGetString(RelativeSubManifestPath) || RelativeSubManifestPath.IsEmpty())
		{
			continue;
		}

		const FString FullSubManifestPath = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir() / RelativeSubManifestPath);
		TSharedPtr<FJsonObject> SubManifestObject;
		if (!LoadJsonObjectFromFile(FullSubManifestPath, SubManifestObject))
		{
			continue;
		}

		const TArray<TSharedPtr<FJsonValue>>* Tasks = nullptr;
		if (!SubManifestObject->TryGetArrayField(TEXT("tasks"), Tasks) || !Tasks)
		{
			continue;
		}

		for (const TSharedPtr<FJsonValue>& TaskValue : *Tasks)
		{
			const TSharedPtr<FJsonObject> TaskObject = TaskValue.IsValid() ? TaskValue->AsObject() : nullptr;
			const TSharedPtr<FJsonObject>* RecipeObjectPtr = nullptr;
			if (!TaskObject.IsValid()
				|| !TaskObject->TryGetObjectField(TEXT("recipe"), RecipeObjectPtr)
				|| !RecipeObjectPtr
				|| !RecipeObjectPtr->IsValid())
			{
				continue;
			}
			const TSharedPtr<FJsonObject> RecipeObject = *RecipeObjectPtr;

			FString GenerationType;
			FString OutputPath;
			if (!RecipeObject->TryGetStringField(TEXT("generationType"), GenerationType)
				|| !SupportedGenerationTypes.Contains(GenerationType)
				|| !RecipeObject->TryGetStringField(TEXT("outputPath"), OutputPath)
				|| NormalizePackagePath(OutputPath) != NormalizedPackagePath)
			{
				continue;
			}

			const TArray<TSharedPtr<FJsonValue>>* Ingredients = nullptr;
			if (!RecipeObject->TryGetArrayField(TEXT("ingredients"), Ingredients) || !Ingredients)
			{
				continue;
			}

			for (const TSharedPtr<FJsonValue>& IngredientValue : *Ingredients)
			{
				const TSharedPtr<FJsonObject> IngredientObject = IngredientValue.IsValid() ? IngredientValue->AsObject() : nullptr;
				FString IngredientType;
				if (!IngredientObject.IsValid()
					|| !IngredientObject->TryGetStringField(TEXT("type"), IngredientType)
					|| IngredientType != TEXT("json_template"))
				{
					continue;
				}

				OutJsonPath = ResolveJsonTemplatePathField(IngredientObject);
				return !OutJsonPath.IsEmpty();
			}
		}
	}

	return false;
}

bool TryClassifyAssetForAutoExport(
	UObject* Asset,
	FString& OutGenerationType,
	FString& OutDefaultJsonPath)
{
	OutGenerationType.Reset();
	OutDefaultJsonPath.Reset();

	if (!Asset)
	{
		return false;
	}

	const UClass* AssetClass = Asset->GetClass();
	if (IsClassNamedOrDerived(AssetClass, TEXT("WidgetBlueprint")))
	{
		return false;
	}

	FString TemplateDirectory;
	if (IsClassNamedOrDerived(AssetClass, TEXT("AnimBlueprint")))
	{
		OutGenerationType = TEXT("AnimBlueprint");
		TemplateDirectory = TEXT("Config/BlueprintTemplates");
	}
	else if (Asset->IsA(UBlueprint::StaticClass()))
	{
		OutGenerationType = TEXT("Blueprint");
		TemplateDirectory = TEXT("Config/BlueprintTemplates");
	}
	else if (Asset->IsA(UMaterialInstanceConstant::StaticClass()))
	{
		OutGenerationType = TEXT("MaterialInstance");
		TemplateDirectory = TEXT("Config/MaterialTemplates");
	}
	else if (Asset->IsA(UMaterial::StaticClass()))
	{
		OutGenerationType = TEXT("Material");
		TemplateDirectory = TEXT("Config/MaterialTemplates");
	}
	else
	{
		return false;
	}

	const FString AssetName = Asset->GetName();
	OutDefaultJsonPath = FPaths::Combine(TemplateDirectory, AssetName + TEXT(".json"));
	return true;
}

FString BuildExportPythonCommand(const FString& GenerationType, const FString& AssetPath, const FString& JsonPath)
{
	if (GenerationType == TEXT("AnimBlueprint"))
	{
		return FString::Printf(
			TEXT("import importlib; import blueprint_factory.anim_bp_generator as anim_bp_generator; importlib.reload(anim_bp_generator); anim_bp_generator.export_anim_blueprint(r'%s', r'%s')"),
			*AssetPath,
			*JsonPath);
	}

	if (GenerationType == TEXT("Blueprint") || GenerationType == TEXT("BarrierBlueprint"))
	{
		return FString::Printf(
			TEXT("import importlib; import blueprint_factory.bp_generator as bp_generator; importlib.reload(bp_generator); bp_generator.export_blueprint(r'%s', r'%s')"),
			*AssetPath,
			*JsonPath);
	}

	if (GenerationType == TEXT("Material") || GenerationType == TEXT("MaterialInstance"))
	{
		return FString::Printf(
			TEXT("import importlib; import blueprint_factory.mat_generator as mat_generator; importlib.reload(mat_generator); mat_generator.export_material(r'%s', r'%s')"),
			*AssetPath,
			*JsonPath);
	}

	return FString();
}
}

void FBlueprintFactoryModule::StartupModule()
{
	FGlobalTabmanager::Get()->RegisterNomadTabSpawner(TabId,
		FOnSpawnTab::CreateRaw(this, &FBlueprintFactoryModule::SpawnTab))
		.SetDisplayName(LOCTEXT("TabTitle", "Blueprint Factory"))
		.SetMenuType(ETabSpawnerMenuType::Hidden);

	UToolMenus::RegisterStartupCallback(FSimpleMulticastDelegate::FDelegate::CreateRaw(this, &FBlueprintFactoryModule::RegisterMenus));

	PackageSavedHandle = UPackage::PackageSavedWithContextEvent.AddLambda(
		[](const FString&, UPackage* SavedPackage, FObjectPostSaveContext)
		{
			if (!SavedPackage)
			{
				return;
			}

			const FString PackagePath = SavedPackage->GetName();
			if (PackagePath.IsEmpty() || !PackagePath.StartsWith(TEXT("/Game/")))
			{
				return;
			}

			UObject* SavedAsset = LoadSavedAsset(PackagePath);
			FString GenerationType;
			FString DefaultJsonPath;
			if (!TryClassifyAssetForAutoExport(SavedAsset, GenerationType, DefaultJsonPath))
			{
				return;
			}

			const TArray<FString> SupportedGenerationTypes = {
				TEXT("Blueprint"),
				TEXT("BarrierBlueprint"),
				TEXT("AnimBlueprint"),
				TEXT("Material"),
				TEXT("MaterialInstance")
			};

			FString RelativeJsonPath;
			if (!TryResolveJsonPathFromManifest(PackagePath, SupportedGenerationTypes, RelativeJsonPath))
			{
				RelativeJsonPath = DefaultJsonPath;
			}

			if (IsSharedCharacterTemplatePath(RelativeJsonPath))
			{
				return;
			}

			FString AssetPath = NormalizePackagePath(PackagePath);
			AssetPath.ReplaceInline(TEXT("\\"), TEXT("/"));
			FString JsonPath = ResolveFullJsonPath(RelativeJsonPath);
			JsonPath.ReplaceInline(TEXT("\\"), TEXT("/"));

			const FString PythonCommand = BuildExportPythonCommand(GenerationType, AssetPath, JsonPath);
			if (PythonCommand.IsEmpty())
			{
				UE_LOG(LogTemp, Warning, TEXT("[BlueprintFactory] 自动保存回写不支持的类型: %s"), *GenerationType);
				return;
			}

			if (!IPythonScriptPlugin::Get()->ExecPythonCommand(*PythonCommand))
			{
				UE_LOG(LogTemp, Warning, TEXT("[BlueprintFactory] 自动保存回写失败: %s -> %s"), *PackagePath, *RelativeJsonPath);
				return;
			}

			UE_LOG(LogTemp, Log, TEXT("[BlueprintFactory] 保存后已回写 JSON: %s -> %s"), *PackagePath, *RelativeJsonPath);
		});
}

void FBlueprintFactoryModule::ShutdownModule()
{
	if (PackageSavedHandle.IsValid())
	{
		UPackage::PackageSavedWithContextEvent.Remove(PackageSavedHandle);
		PackageSavedHandle.Reset();
	}

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
