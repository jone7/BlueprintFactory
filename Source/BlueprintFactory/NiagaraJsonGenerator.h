#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "NiagaraJsonGenerator.generated.h"

class UNiagaraSystem;

// 将有限的语义粒子 JSON 编译为真实 Niagara 资产；不接受任意脚本或 HLSL 文本。
UCLASS()
class BLUEPRINTFACTORY_API UNiagaraJsonGenerator : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()
public:
	// 校验版本、预设、数量和运动边界；无资产加载或文件写入，供生产入口与测试共用。
	UFUNCTION(BlueprintCallable, Category="BlueprintFactory|Niagara")
	static bool ValidateJsonDefinition(const FString& Document, FString& OutError);
	// 从本地作者 JSON 创建或更新指定 Niagara 输出，编译失败返回空并报告具体错误。
	UFUNCTION(BlueprintCallable, Category="BlueprintFactory|Niagara")
	static UNiagaraSystem* GenerateFromJson(const FString& JsonPath, const FString& OutputAssetPath);

	// 回读编译状态、模拟空间和渲染绑定，供离线资源验收；本地报告不参与网络传播。
	UFUNCTION(BlueprintCallable, Category="BlueprintFactory|Niagara")
	static FString DescribeSystem(UNiagaraSystem* System);
};
