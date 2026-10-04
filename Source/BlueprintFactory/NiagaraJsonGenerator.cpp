#include "NiagaraJsonGenerator.h"
#include "NiagaraSystem.h"
#include "NiagaraEmitter.h"
#include "NiagaraScript.h"
#include "NiagaraEffectType.h"
#include "NiagaraMeshRendererProperties.h"
#include "NiagaraGraph.h"
#include "NiagaraScriptSource.h"
#include "NiagaraNodeInput.h"
#include "NiagaraNodeOutput.h"
#include "NiagaraNodeCustomHlsl.h"
#include "NiagaraNodeAssignment.h"
#include "ViewModels/Stack/NiagaraParameterHandle.h"
#include "NiagaraSystemFactoryNew.h"
#include "ViewModels/Stack/NiagaraStackGraphUtilities.h"
#include "EdGraphSchema_Niagara.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/PackageName.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Engine/StaticMesh.h"
#include "Materials/MaterialInterface.h"
#include "UObject/UnrealType.h"
#include "UObject/SavePackage.h"
#include "Editor.h"

// 编辑器生成细节集中于本 Builder，运动参数来自 JSON；不存在四色专用运动分支。
class FLeafNiagaraBuilder
{
public:
	// 本次生成的系统，动态输入脚本嵌入此资产生命周期。
	UNiagaraSystem* System = nullptr;
	// 新复制的发射器，所有变更发生在输出实例而非引擎模板。
	UNiagaraEmitter* Emitter = nullptr;
	// 发射器模块栈图，仅用于编辑器生成。
	UNiagaraGraph* Graph = nullptr;
	// 绑定一个已声明的系统用户参数默认值。
	template<typename T> void Parameter(const FNiagaraTypeDefinition& Type, const TCHAR* Name, const T& Value)
	{
		const FNiagaraVariable Variable(Type, Name);
		System->GetExposedParameters().AddParameter(Variable);
		System->GetExposedParameters().SetParameterData(reinterpret_cast<const uint8*>(&Value), Variable);
	}
	// 创建返回单一属性值的动态输入；公式由受限预设持有，JSON 不能注入代码。
	UNiagaraScript* Expression(const FNiagaraVariable& Variable, const FString& Code)
	{
		UNiagaraScript* Script = NewObject<UNiagaraScript>(Emitter);
		Script->SetUsage(ENiagaraScriptUsage::DynamicInput);
		UNiagaraScriptSource* Source = NewObject<UNiagaraScriptSource>(Script);
		UNiagaraGraph* ExpressionGraph = NewObject<UNiagaraGraph>(Source);
		ExpressionGraph->Schema = UEdGraphSchema_Niagara::StaticClass();
		Source->NodeGraph = ExpressionGraph;
		Script->SetLatestSource(Source);
		Script->GetLatestScriptData()->ModuleUsageBitmask = -1;
		UNiagaraNodeInput* Input = NewObject<UNiagaraNodeInput>(ExpressionGraph);
		Input->Input = FNiagaraVariable(FNiagaraTypeDefinition::GetParameterMapDef(), TEXT("Map"));
		Input->Usage = ENiagaraInputNodeUsage::Parameter;
		ExpressionGraph->AddNode(Input, false, false);
		Input->CreateNewGuid();
		static_cast<UEdGraphNode*>(Input)->AllocateDefaultPins();
		UNiagaraNodeCustomHlsl* Custom = NewObject<UNiagaraNodeCustomHlsl>(ExpressionGraph);
		Custom->ScriptUsage = ENiagaraScriptUsage::DynamicInput;
		Custom->Signature.Name = TEXT("LeafValue");
		Custom->Signature.Inputs.Add(Input->Input);
		Custom->Signature.Outputs.Add(FNiagaraVariable(Variable.GetType(), TEXT("Value")));
		Custom->Signature.bRequiresContext = true;
		// CustomHlsl 为明确的反射属性，使用反射写入避免依赖 NiagaraEditor 未导出的私有编辑方法。
		FindFProperty<FStrProperty>(Custom->GetClass(), TEXT("CustomHlsl"))->SetPropertyValue_InContainer(Custom, Code);
		ExpressionGraph->AddNode(Custom, false, false);
		Custom->CreateNewGuid();
		static_cast<UEdGraphNode*>(Custom)->AllocateDefaultPins();
		UNiagaraNodeOutput* Output = NewObject<UNiagaraNodeOutput>(ExpressionGraph);
		Output->ScriptType = ENiagaraScriptUsage::DynamicInput;
		Output->Outputs.Add(FNiagaraVariable(Variable.GetType(), TEXT("Value")));
		ExpressionGraph->AddNode(Output, false, false);
		Output->CreateNewGuid();
		static_cast<UEdGraphNode*>(Output)->AllocateDefaultPins();
		Input->FindPinChecked(TEXT("Input"), EGPD_Output)->MakeLinkTo(Custom->FindPinChecked(TEXT("Map"), EGPD_Input));
		Custom->FindPinChecked(TEXT("Value"), EGPD_Output)->MakeLinkTo(Output->FindPinChecked(TEXT("Value"), EGPD_Input));
		return Script;
	}
	// 在唯一模块栈上增加属性赋值，动态输入读取系统参数和已有粒子属性。
	void Assign(UNiagaraNodeOutput& Output, const FNiagaraTypeDefinition& Type, const TCHAR* Name, const FString& Code)
	{
		const FNiagaraVariable Variable(Type, Name);
		UNiagaraNodeAssignment* Assignment = FNiagaraStackGraphUtilities::AddParameterModuleToStack(
			{Variable}, Output, INDEX_NONE, {FString()});
		const FNiagaraParameterHandle Handle = FNiagaraParameterHandle::CreateAliasedModuleParameterHandle(
			FNiagaraParameterHandle(*(TEXT("Module.") + Variable.GetName().ToString())), Assignment);
		UEdGraphPin& Pin = FNiagaraStackGraphUtilities::GetOrCreateStackFunctionInputOverridePin(*Assignment, Handle, Type, FGuid(), FGuid());
		UNiagaraNodeFunctionCall* Dynamic = nullptr;
		FNiagaraStackGraphUtilities::SetDynamicInputForFunctionInput(Pin, Expression(Variable, Code), Dynamic);
	}
	// 序列化一个作者数组为有限浮点向量，类型长度已经由入口校验。
	static FVector Vector(const TSharedPtr<FJsonObject>& Json, const TCHAR* Key)
	{
		const auto& Array = Json->GetArrayField(Key);
		return FVector(Array[0]->AsNumber(), Array[1]->AsNumber(), Array[2]->AsNumber());
	}
	// 保存输出与自身 EffectType，调用者据返回值判定真实磁盘写入是否成功。
	static bool Save(UObject* Asset)
	{
		Asset->MarkPackageDirty();
		FSavePackageArgs Args;
		Args.TopLevelFlags = RF_Public | RF_Standalone;
		Args.SaveFlags = SAVE_NoError;
		return UPackage::SavePackage(Asset->GetOutermost(), Asset,
			*FPackageName::LongPackageNameToFilename(Asset->GetOutermost()->GetName(), FPackageName::GetAssetPackageExtension()), Args);
	}
};

// 外部作者 JSON 在唯一入口严格校验，错误参数不进入原生模块构建。
bool UNiagaraJsonGenerator::ValidateJsonDefinition(const FString& Document, FString& OutError)
{
	OutError.Reset();
	TSharedPtr<FJsonObject> Json;
	if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Document), Json) || !Json)
	{
		OutError = TEXT("JSON must be an object."); return false;
	}
	double Version = 0;
	FString Preset;
	if (!Json->TryGetNumberField(TEXT("SchemaVersion"), Version) || Version != 1
		|| !Json->TryGetStringField(TEXT("Preset"), Preset) || Preset != TEXT("LeafFall"))
	{
		OutError = TEXT("Only SchemaVersion=1, Preset=LeafFall is supported."); return false;
	}
	const TSet<FString> Allowed = {TEXT("SchemaVersion"), TEXT("Preset"), TEXT("Name"), TEXT("Meshes"),
		TEXT("DefaultMaterial"), TEXT("EffectTypePath"), TEXT("SpawnRate"), TEXT("Lifetime"), TEXT("ScaleRange"),
		TEXT("SpawnHalfExtent"), TEXT("WindVelocity"), TEXT("FallSpeed"), TEXT("SwayStrength"), TEXT("RotationRate"),
		TEXT("CullDistanceCm"), TEXT("MaxInstances"), TEXT("FixedBoundsMin"), TEXT("FixedBoundsMax")};
	for (const auto& Field : Json->Values)
	{
		const FString Key(Field.Key);
		if (!Allowed.Contains(Key)) { OutError = TEXT("Unknown field: ") + Key; return false; }
	}
	for (const TCHAR* Key : {TEXT("Name"), TEXT("DefaultMaterial"), TEXT("EffectTypePath")})
	{
		FString Value;
		if (!Json->TryGetStringField(Key, Value) || Value.IsEmpty()) { OutError = FString(TEXT("Missing string: ")) + Key; return false; }
	}
	const TArray<TSharedPtr<FJsonValue>>* Meshes = nullptr;
	if (!Json->TryGetArrayField(TEXT("Meshes"), Meshes) || Meshes->Num() < 1 || Meshes->Num() > 8)
	{
		OutError = TEXT("Meshes must contain 1..8 asset paths."); return false;
	}
	for (const auto& Mesh : *Meshes)
	{
		FString Path;
		if (!Mesh->TryGetString(Path) || !Path.StartsWith(TEXT("/Game/"))) { OutError = TEXT("Invalid mesh asset path."); return false; }
	}
	const TMap<FString, FVector2D> Ranges = {{TEXT("SpawnRate"), {0, 12}}, {TEXT("FallSpeed"), {1, 200}},
		{TEXT("SwayStrength"), {0, 100}}, {TEXT("RotationRate"), {0, 6.3}}, {TEXT("CullDistanceCm"), {100, 10000}},
		{TEXT("MaxInstances"), {1, 64}}};
	for (const auto& Range : Ranges)
	{
		double Value = 0;
		if (!Json->TryGetNumberField(Range.Key, Value) || !FMath::IsFinite(Value) || Value < Range.Value.X || Value > Range.Value.Y)
		{
			OutError = TEXT("Out of range: ") + Range.Key; return false;
		}
	}
	for (const TCHAR* Key : {TEXT("Lifetime"), TEXT("ScaleRange"), TEXT("SpawnHalfExtent"), TEXT("WindVelocity"), TEXT("FixedBoundsMin"), TEXT("FixedBoundsMax")})
	{
		const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
		const int32 Expected = FString(Key) == TEXT("Lifetime") || FString(Key) == TEXT("ScaleRange") ? 2 : 3;
		if (!Json->TryGetArrayField(Key, Values) || Values->Num() != Expected)
		{
			OutError = FString(TEXT("Wrong vector size: ")) + Key; return false;
		}
		for (const auto& Entry : *Values)
		{
			double Value = 0;
			if (!Entry->TryGetNumber(Value) || !FMath::IsFinite(Value) || FMath::Abs(Value) > 10000)
			{
				OutError = FString(TEXT("Invalid vector value: ")) + Key; return false;
			}
		}
	}
	const auto& Lifetime = Json->GetArrayField(TEXT("Lifetime"));
	const auto& Scale = Json->GetArrayField(TEXT("ScaleRange"));
	if (Lifetime[0]->AsNumber() < .5 || Lifetime[1]->AsNumber() > 20 || Lifetime[1]->AsNumber() < Lifetime[0]->AsNumber()
		|| Scale[0]->AsNumber() <= 0 || Scale[1]->AsNumber() > 5 || Scale[1]->AsNumber() < Scale[0]->AsNumber()
		|| FLeafNiagaraBuilder::Vector(Json, TEXT("SpawnHalfExtent")).GetMin() <= 0
		|| FLeafNiagaraBuilder::Vector(Json, TEXT("SpawnHalfExtent")).GetMax() > 1000)
	{
		OutError = TEXT("Invalid lifetime, scale range or spawn half extent."); return false;
	}
	const FVector Minimum = FLeafNiagaraBuilder::Vector(Json, TEXT("FixedBoundsMin"));
	const FVector Maximum = FLeafNiagaraBuilder::Vector(Json, TEXT("FixedBoundsMax"));
	if ((Maximum - Minimum).GetMin() <= 0 || Json->GetNumberField(TEXT("MaxInstances")) != FMath::FloorToDouble(Json->GetNumberField(TEXT("MaxInstances"))))
	{
		OutError = TEXT("Bounds must be ordered; MaxInstances must be an integer."); return false;
	}
	const FString EffectPath = Json->GetStringField(TEXT("EffectTypePath"));
	if (!EffectPath.StartsWith(TEXT("/Game/")) || !FPackageName::IsValidLongPackageName(EffectPath)
		|| !Json->GetStringField(TEXT("DefaultMaterial")).StartsWith(TEXT("/Game/")))
	{
		OutError = TEXT("Material and EffectType must be project assets."); return false;
	}
	// 固定包围盒必须覆盖最久存活粒子的全部轨迹及叶片尺寸，避免运动正常却提前裁剪。
	const FVector Extent = FLeafNiagaraBuilder::Vector(Json, TEXT("SpawnHalfExtent"));
	const FVector Wind = FLeafNiagaraBuilder::Vector(Json, TEXT("WindVelocity"));
	const double Age = Lifetime[1]->AsNumber();
	const double Margin = Json->GetNumberField(TEXT("SwayStrength")) + Scale[1]->AsNumber() * 10;
	const FVector End(Wind.X * Age, Wind.Y * Age, -Json->GetNumberField(TEXT("FallSpeed")) * Age);
	for (int32 Axis = 0; Axis < 3; ++Axis)
	{
		if (Minimum[Axis] > -Extent[Axis] + FMath::Min(0.0, End[Axis]) - Margin
			|| Maximum[Axis] < Extent[Axis] + FMath::Max(0.0, End[Axis]) + Margin)
		{
			OutError = TEXT("Fixed bounds do not cover the complete leaf trajectory."); return false;
		}
	}
	return true;
}

// 资产回读仅返回此系统可验证的局部信息，不输出 Profile 或世界快照。
FString UNiagaraJsonGenerator::DescribeSystem(UNiagaraSystem* System)
{
	if (!System) return TEXT("{}");
	System->WaitForCompilationComplete(false, false);
	TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
	Json->SetBoolField(TEXT("valid"), System->IsValid());
	Json->SetBoolField(TEXT("ready"), System->IsReadyToRun());
	Json->SetBoolField(TEXT("fixedBounds"), System->bFixedBounds);
	Json->SetNumberField(TEXT("emitters"), System->GetEmitterHandles().Num());
	int32 MeshCount = 0;
	bool bWorldSpace = true, bCpu = true, bUserMaterial = true;
	for (const FNiagaraEmitterHandle& Handle : System->GetEmitterHandles())
	{
		const FVersionedNiagaraEmitterData* Data = Handle.GetInstance().GetEmitterData();
		bWorldSpace &= !Data->bLocalSpace;
		bCpu &= Data->SimTarget == ENiagaraSimTarget::CPUSim;
		for (UNiagaraRendererProperties* Properties : Data->GetRenderers())
		{
			const UNiagaraMeshRendererProperties* Renderer = Cast<UNiagaraMeshRendererProperties>(Properties);
			if (!Renderer) { bUserMaterial = false; continue; }
			MeshCount += Renderer->Meshes.Num();
			bUserMaterial &= Renderer->bOverrideMaterials && Renderer->OverrideMaterials.Num() == 1
				&& Renderer->OverrideMaterials[0].UserParamBinding.Parameter.GetName() == TEXT("User.LeafMaterial");
		}
	}
	Json->SetNumberField(TEXT("meshCount"), MeshCount);
	Json->SetBoolField(TEXT("worldSpace"), bWorldSpace);
	Json->SetBoolField(TEXT("cpu"), bCpu);
	Json->SetBoolField(TEXT("userMaterial"), bUserMaterial);
	const UNiagaraEffectType* Effect = System->GetEffectType();
	Json->SetStringField(TEXT("effectType"), Effect ? Effect->GetPathName() : FString());
	if (Effect && Effect->SystemScalabilitySettings.Settings.Num() == 1)
	{
		const FNiagaraSystemScalabilitySettings& Settings = Effect->SystemScalabilitySettings.Settings[0];
		Json->SetNumberField(TEXT("maxInstances"), Settings.bCullMaxInstanceCount ? Settings.MaxInstances : 0);
		Json->SetNumberField(TEXT("cullDistance"), Settings.bCullByDistance ? Settings.MaxDistance : 0);
	}
	FString Report;
	FJsonSerializer::Serialize(Json, TJsonWriterFactory<>::Create(&Report));
	return Report;
}

// 生产入口与面板配方调用同一生成器；编译和保存都成功后才返回真实输出资产。
UNiagaraSystem* UNiagaraJsonGenerator::GenerateFromJson(const FString& JsonPath, const FString& OutputAssetPath)
{
	FString Document, Error;
	const FString FullPath = FPaths::IsRelative(JsonPath) ? FPaths::ProjectDir() / JsonPath : JsonPath;
	if (!FFileHelper::LoadFileToString(Document, *FullPath) || !ValidateJsonDefinition(Document, Error)
		|| !OutputAssetPath.StartsWith(TEXT("/Game/")) || !FPackageName::IsValidLongPackageName(OutputAssetPath)
		|| (GEditor && GEditor->PlayWorld))
	{
		UE_LOG(LogTemp, Warning, TEXT("Niagara JSON rejected: %s"), *Error); return nullptr;
	}
	TSharedPtr<FJsonObject> Json;
	FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Document), Json);
	if (FPaths::GetBaseFilename(OutputAssetPath) != Json->GetStringField(TEXT("Name"))
		|| FPaths::GetPath(Json->GetStringField(TEXT("EffectTypePath"))) != FPaths::GetPath(OutputAssetPath)) return nullptr;
	UNiagaraEmitter* Minimal = LoadObject<UNiagaraEmitter>(nullptr, TEXT("/Niagara/DefaultAssets/Templates/Emitters/Minimal.Minimal"));
	UNiagaraScript* SpawnRate = LoadObject<UNiagaraScript>(nullptr, TEXT("/Niagara/Modules/Emitter/SpawnRate.SpawnRate"));
	UMaterialInterface* Material = LoadObject<UMaterialInterface>(nullptr, *Json->GetStringField(TEXT("DefaultMaterial")));
	TArray<UStaticMesh*> Meshes;
	for (const auto& Path : Json->GetArrayField(TEXT("Meshes")))
	{
		UStaticMesh* Mesh = LoadObject<UStaticMesh>(nullptr, *Path->AsString());
		if (!Mesh) return nullptr;
		Meshes.Add(Mesh);
	}
	if (!Minimal || !SpawnRate || !Material) return nullptr;
	UPackage* Package = CreatePackage(*OutputAssetPath);
	FLeafNiagaraBuilder Builder;
	Builder.System = LoadObject<UNiagaraSystem>(nullptr, *(OutputAssetPath + TEXT(".") + Json->GetStringField(TEXT("Name"))));
	if (!Builder.System)
	{
		Builder.System = NewObject<UNiagaraSystem>(Package, *Json->GetStringField(TEXT("Name")), RF_Public | RF_Standalone | RF_Transactional);
		UNiagaraSystemFactoryNew::InitializeSystem(Builder.System, true);
		FAssetRegistryModule::AssetCreated(Builder.System);
	}
	TSet<FGuid> OldHandles;
	for (const FNiagaraEmitterHandle& Handle : Builder.System->GetEmitterHandles()) OldHandles.Add(Handle.GetId());
	Builder.System->RemoveEmitterHandlesById(OldHandles);
	const FNiagaraEmitterHandle Handle = Builder.System->AddEmitterHandle(*Minimal, TEXT("LeafFall"), Minimal->GetExposedVersion().VersionGuid);
	Builder.Emitter = Handle.GetInstance().Emitter;
	auto* Data = Builder.Emitter->GetLatestEmitterData();
	Data->bLocalSpace = false;
	Data->SimTarget = ENiagaraSimTarget::CPUSim;
	Data->bRequiresPersistentIDs = false;
	Builder.Graph = CastChecked<UNiagaraScriptSource>(Data->GraphSource)->NodeGraph;
	TArray<UNiagaraNodeOutput*> Outputs;
	Builder.Graph->GetNodesOfClass(Outputs);
	UNiagaraNodeOutput* Spawn = nullptr;
	UNiagaraNodeOutput* Update = nullptr;
	UNiagaraNodeOutput* EmitterUpdate = nullptr;
	for (UNiagaraNodeOutput* Output : Outputs)
	{
		if (Output->ScriptType == ENiagaraScriptUsage::ParticleSpawnScript) Spawn = Output;
		if (Output->ScriptType == ENiagaraScriptUsage::ParticleUpdateScript) Update = Output;
		if (Output->ScriptType == ENiagaraScriptUsage::EmitterUpdateScript) EmitterUpdate = Output;
	}
	if (!Spawn || !Update || !EmitterUpdate) return nullptr;
	const FNiagaraTypeDefinition Float = FNiagaraTypeDefinition::GetFloatDef();
	const FNiagaraTypeDefinition Vec3 = FNiagaraTypeDefinition::GetVec3Def();
	const auto& Lifetime = Json->GetArrayField(TEXT("Lifetime"));
	const auto& Scale = Json->GetArrayField(TEXT("ScaleRange"));
	Builder.Parameter(Float, TEXT("User.SpawnRate"), float(Json->GetNumberField(TEXT("SpawnRate"))));
	Builder.Parameter(FNiagaraTypeDefinition::GetVec2Def(), TEXT("User.Lifetime"), FVector2f(Lifetime[0]->AsNumber(), Lifetime[1]->AsNumber()));
	Builder.Parameter(FNiagaraTypeDefinition::GetVec2Def(), TEXT("User.ScaleRange"), FVector2f(Scale[0]->AsNumber(), Scale[1]->AsNumber()));
	Builder.Parameter(Vec3, TEXT("User.SpawnHalfExtent"), FVector3f(FLeafNiagaraBuilder::Vector(Json, TEXT("SpawnHalfExtent"))));
	Builder.Parameter(Vec3, TEXT("User.WindVelocity"), FVector3f(FLeafNiagaraBuilder::Vector(Json, TEXT("WindVelocity"))));
	for (const TCHAR* Key : {TEXT("FallSpeed"), TEXT("SwayStrength"), TEXT("RotationRate")})
	{
		Builder.Parameter(Float, *(FString(TEXT("User.")) + Key), float(Json->GetNumberField(Key)));
	}
	const FNiagaraVariable MaterialVariable(FNiagaraTypeDefinition(UMaterialInterface::StaticClass()), TEXT("User.LeafMaterial"));
	Builder.System->GetExposedParameters().AddParameter(MaterialVariable);
	Builder.System->GetExposedParameters().SetUObject(Material, MaterialVariable);
	UNiagaraNodeFunctionCall* Rate = FNiagaraStackGraphUtilities::AddScriptModuleToStack(SpawnRate, *EmitterUpdate);
	const FNiagaraParameterHandle RateHandle = FNiagaraParameterHandle::CreateAliasedModuleParameterHandle(FNiagaraParameterHandle(TEXT("Module.SpawnRate")), Rate);
	UEdGraphPin& RatePin = FNiagaraStackGraphUtilities::GetOrCreateStackFunctionInputOverridePin(*Rate, RateHandle, Float, FGuid(), FGuid());
	FNiagaraStackGraphUtilities::SetLinkedParameterValueForFunctionInput(RatePin, FNiagaraVariable(Float, TEXT("User.SpawnRate")), {});
	// Niagara DynamicInput 自己生成输出赋值；这里仅提供表达式，不再嵌套语句块。
	Builder.Assign(*Spawn, Vec3, TEXT("Particles.LeafSeed"), TEXT("frac(sin((float)Particles.UniqueID*float3(12.9898,78.233,39.425)+float3(0.137,0.37,0.72))*43758.5453)"));
	Builder.Assign(*Spawn, Float, TEXT("Particles.Lifetime"), TEXT("lerp(User.Lifetime.x,User.Lifetime.y,Particles.LeafSeed.x)"));
	Builder.Assign(*Spawn, FNiagaraTypeDefinition::GetPositionDef(), TEXT("Particles.Position"), TEXT("Engine.Owner.Position+(Particles.LeafSeed*2.0-1.0)*User.SpawnHalfExtent"));
	Builder.Assign(*Spawn, FNiagaraTypeDefinition::GetPositionDef(), TEXT("Particles.LeafOrigin"), TEXT("Particles.Position"));
	Builder.Assign(*Spawn, FNiagaraTypeDefinition::GetIntDef(), TEXT("Particles.MeshIndex"), FString::Printf(TEXT("(int)floor(Particles.LeafSeed.y*%d.0)"), Meshes.Num()));
	Builder.Assign(*Spawn, FNiagaraTypeDefinition::GetColorDef(), TEXT("Particles.Color"), TEXT("float4(1.0,1.0,1.0,1.0)"));
	Builder.Assign(*Update, FNiagaraTypeDefinition::GetPositionDef(), TEXT("Particles.Position"), TEXT("Particles.LeafOrigin+float3(User.WindVelocity.xy*Particles.Age,-User.FallSpeed*Particles.Age)+float3(sin(Particles.Age*0.9+Particles.LeafSeed.x*6.283)*User.SwayStrength,cos(Particles.Age*1.1+Particles.LeafSeed.y*6.283)*User.SwayStrength,0.0)"));
	const FString ScaleCode = TEXT("float3(1.0,1.0,1.0)*lerp(User.ScaleRange.x,User.ScaleRange.y,Particles.LeafSeed.z)*saturate((Particles.Lifetime-Particles.Age)/1.2)");
	const FString RotationCode = TEXT("float4(normalize(Particles.LeafSeed+float3(0.2,0.1,0.3))*sin((Particles.LeafSeed.z*6.283+Particles.Age*User.RotationRate)*0.5),cos((Particles.LeafSeed.z*6.283+Particles.Age*User.RotationRate)*0.5))");
	for (UNiagaraNodeOutput* Output : {Spawn, Update})
	{
		Builder.Assign(*Output, Vec3, TEXT("Particles.Scale"), ScaleCode);
		Builder.Assign(*Output, FNiagaraTypeDefinition::GetQuatDef(), TEXT("Particles.MeshOrientation"), RotationCode);
	}
	const auto OldRenderers = Data->GetRenderers();
	for (UNiagaraRendererProperties* Renderer : OldRenderers) Builder.Emitter->RemoveRenderer(Renderer, Builder.Emitter->GetExposedVersion().VersionGuid);
	UNiagaraMeshRendererProperties* Renderer = NewObject<UNiagaraMeshRendererProperties>(Builder.Emitter);
	Renderer->Meshes.Reset();
	for (UStaticMesh* Mesh : Meshes) { FNiagaraMeshRendererMeshProperties Entry; Entry.Mesh = Mesh; Renderer->Meshes.Add(Entry); }
	Renderer->bOverrideMaterials = true;
	FNiagaraMeshMaterialOverride Override;
	Override.ExplicitMat = Material;
	Override.UserParamBinding.Parameter = MaterialVariable;
	Renderer->OverrideMaterials.Add(Override);
	Builder.Emitter->AddRenderer(Renderer, Builder.Emitter->GetExposedVersion().VersionGuid);
	const FBox Bounds(FLeafNiagaraBuilder::Vector(Json, TEXT("FixedBoundsMin")), FLeafNiagaraBuilder::Vector(Json, TEXT("FixedBoundsMax")));
	Data->FixedBounds = Bounds;
	Builder.System->bFixedBounds = true;
	Builder.System->SetFixedBounds(Bounds);
	const FString EffectPath = Json->GetStringField(TEXT("EffectTypePath"));
	UNiagaraEffectType* Effect = LoadObject<UNiagaraEffectType>(nullptr, *(EffectPath + TEXT(".") + FPaths::GetBaseFilename(EffectPath)));
	if (!Effect)
	{
		Effect = NewObject<UNiagaraEffectType>(CreatePackage(*EffectPath), *FPaths::GetBaseFilename(EffectPath), RF_Public | RF_Standalone | RF_Transactional);
		FAssetRegistryModule::AssetCreated(Effect);
	}
	Effect->UpdateFrequency = ENiagaraScalabilityUpdateFrequency::Low;
	Effect->CullReaction = ENiagaraCullReaction::DeactivateImmediateResume;
	Effect->SignificanceHandler = NewObject<UNiagaraSignificanceHandlerDistance>(Effect);
	FNiagaraSystemScalabilitySettings Settings;
	Settings.bCullByDistance = true;
	Settings.MaxDistance = Json->GetNumberField(TEXT("CullDistanceCm"));
	Settings.bCullMaxInstanceCount = true;
	Settings.MaxInstances = Json->GetIntegerField(TEXT("MaxInstances"));
	Effect->SystemScalabilitySettings.Settings = {Settings};
	Builder.System->SetEffectType(Effect);
	Data->GraphSource->ForceGraphToRecompileOnNextCheck();
	Builder.System->RequestCompile(true);
	Builder.System->WaitForCompilationComplete(false, false);
	if (!Builder.System->IsValid() || !Builder.System->IsReadyToRun())
	{
		UE_LOG(LogTemp, Warning, TEXT("Niagara JSON compile failed: %s"), *OutputAssetPath); return nullptr;
	}
	if (!FLeafNiagaraBuilder::Save(Effect) || !FLeafNiagaraBuilder::Save(Builder.System)) return nullptr;
	UE_LOG(LogTemp, Display, TEXT("Niagara JSON generated and compiled: %s"), *OutputAssetPath);
	return Builder.System;
}
