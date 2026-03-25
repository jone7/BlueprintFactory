#pragma once
#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "BPFactoryBlueprintLibrary.generated.h"

UCLASS()
class BLUEPRINTFACTORY_API UBPFactoryBlueprintLibrary : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	/**
	 * 创建平坦 Landscape
	 * @param WorldContext 世界上下文
	 * @param Location 地形位置
	 * @param SizeX 横向格子数
	 * @param SizeY 纵向格子数
	 * @param ScaleX X 缩放
	 * @param ScaleY Y 缩放
	 * @param ScaleZ Z 缩放
	 * @return 创建的 Landscape Actor
	 */
	UFUNCTION(BlueprintCallable, Category = "BlueprintFactory", meta = (WorldContext = "WorldContext"))
	static AActor* CreateFlatLandscape(
		UObject* WorldContext,
		FVector Location,
		int32 SizeX,
		int32 SizeY,
		float ScaleX,
		float ScaleY,
		float ScaleZ);

	/**
	 * 从 RAW 高度图文件创建 Landscape
	 * @param HeightmapPath RAW 文件路径（uint16 数组，小端序）
	 */
	UFUNCTION(BlueprintCallable, Category = "BlueprintFactory", meta = (WorldContext = "WorldContext"))
	static AActor* CreateLandscapeFromFile(
		UObject* WorldContext,
		FVector Location,
		int32 SizeX,
		int32 SizeY,
		float ScaleX,
		float ScaleY,
		float ScaleZ,
		const FString& HeightmapPath);
};
