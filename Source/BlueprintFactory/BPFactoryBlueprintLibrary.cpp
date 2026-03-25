#include "BPFactoryBlueprintLibrary.h"
#include "Landscape.h"
#include "LandscapeProxy.h"
#include "LandscapeInfo.h"
#include "LandscapeDataAccess.h"
#include "Engine/World.h"
#include "Editor.h"
#include "Misc/FileHelper.h"

static AActor* CreateLandscapeInternal(
	FVector Location, int32 SizeX, int32 SizeY,
	float ScaleX, float ScaleY, float ScaleZ,
	const TArray<uint16>& Heights)
{
	UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	if (!World)
	{
		UE_LOG(LogTemp, Error, TEXT("[BPFactory] 无法获取编辑器世界"));
		return nullptr;
	}

	if (SizeX < 2 || SizeY < 2)
	{
		UE_LOG(LogTemp, Error, TEXT("[BPFactory] 地形尺寸太小: %dx%d"), SizeX, SizeY);
		return nullptr;
	}

	int32 QuadsPerSection = 63;
	int32 SectionsPerComponent = 1;
	int32 QuadsPerComponent = QuadsPerSection * SectionsPerComponent;
	int32 ComponentCountX = FMath::Max(1, (SizeX - 1) / QuadsPerComponent);
	int32 ComponentCountY = FMath::Max(1, (SizeY - 1) / QuadsPerComponent);
	int32 ActualSizeX = ComponentCountX * QuadsPerComponent + 1;
	int32 ActualSizeY = ComponentCountY * QuadsPerComponent + 1;

	// 调整高度数据
	int32 ActualTotal = ActualSizeX * ActualSizeY;
	TArray<uint16> AdjustedHeights;
	AdjustedHeights.SetNum(ActualTotal);
	for (int32 y = 0; y < ActualSizeY; y++)
	{
		for (int32 x = 0; x < ActualSizeX; x++)
		{
			int32 SrcX = FMath::Min(x, SizeX - 1);
			int32 SrcY = FMath::Min(y, SizeY - 1);
			int32 SrcIdx = SrcY * SizeX + SrcX;
			int32 DstIdx = y * ActualSizeX + x;
			AdjustedHeights[DstIdx] = (SrcIdx < Heights.Num()) ? Heights[SrcIdx] : 32768;
		}
	}

	// Spawn Landscape
	FActorSpawnParameters SpawnParams;
	SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

	ALandscape* Landscape = World->SpawnActor<ALandscape>(
		ALandscape::StaticClass(), Location, FRotator::ZeroRotator, SpawnParams);
	if (!Landscape)
	{
		UE_LOG(LogTemp, Error, TEXT("[BPFactory] Spawn ALandscape 失败"));
		return nullptr;
	}

	Landscape->SetActorScale3D(FVector(ScaleX, ScaleY, ScaleZ));

	FGuid LandscapeGuid = FGuid::NewGuid();
	Landscape->SetLandscapeGuid(LandscapeGuid);

	// UE 5.7 Import 签名: (Guid, MinX, MinY, MaxX, MaxY, NumSubsections, SubsectionSizeQuads, HeightDataMap, ...)
	int32 MinX = 0, MinY = 0;
	int32 MaxX = ActualSizeX - 1;
	int32 MaxY = ActualSizeY - 1;

	// 构建 HeightData Map（单个 Guid 对应整个高度数据）
	TMap<FGuid, TArray<uint16>> HeightDataMap;
	HeightDataMap.Add(LandscapeGuid, AdjustedHeights);

	// 构建空的 MaterialLayerData Map
	TMap<FGuid, TArray<FLandscapeImportLayerInfo>> MaterialLayerDataMap;
	MaterialLayerDataMap.Add(LandscapeGuid, TArray<FLandscapeImportLayerInfo>());

	// 空的 Layers
	TArray<FLandscapeLayer> EmptyLayers;

	Landscape->Import(
		LandscapeGuid,
		MinX, MinY, MaxX, MaxY,
		SectionsPerComponent, QuadsPerSection,
		HeightDataMap,
		TEXT(""),
		MaterialLayerDataMap,
		ELandscapeImportAlphamapType::Additive,
		MakeArrayView(EmptyLayers));

	Landscape->RegisterAllComponents();

	UE_LOG(LogTemp, Log, TEXT("[BPFactory] Landscape 创建成功: %dx%d"), ActualSizeX, ActualSizeY);
	return Landscape;
}

AActor* UBPFactoryBlueprintLibrary::CreateFlatLandscape(
	UObject* WorldContext,
	FVector Location, int32 SizeX, int32 SizeY,
	float ScaleX, float ScaleY, float ScaleZ)
{
	TArray<uint16> Heights;
	Heights.SetNum(SizeX * SizeY);
	for (int32 i = 0; i < Heights.Num(); i++)
		Heights[i] = 32768;
	return CreateLandscapeInternal(Location, SizeX, SizeY, ScaleX, ScaleY, ScaleZ, Heights);
}

AActor* UBPFactoryBlueprintLibrary::CreateLandscapeFromFile(
	UObject* WorldContext,
	FVector Location, int32 SizeX, int32 SizeY,
	float ScaleX, float ScaleY, float ScaleZ,
	const FString& HeightmapPath)
{
	TArray<uint8> RawData;
	if (!FFileHelper::LoadFileToArray(RawData, *HeightmapPath))
	{
		UE_LOG(LogTemp, Error, TEXT("[BPFactory] 无法读取高度图: %s"), *HeightmapPath);
		return CreateFlatLandscape(WorldContext, Location, SizeX, SizeY, ScaleX, ScaleY, ScaleZ);
	}

	int32 Total = SizeX * SizeY;
	int32 Count = RawData.Num() / 2;
	TArray<uint16> Heights;
	Heights.SetNum(FMath::Max(Count, Total));
	FMemory::Memcpy(Heights.GetData(), RawData.GetData(), FMath::Min(Count, Total) * 2);
	for (int32 i = Count; i < Total; i++)
		Heights[i] = 32768;

	return CreateLandscapeInternal(Location, SizeX, SizeY, ScaleX, ScaleY, ScaleZ, Heights);
}
