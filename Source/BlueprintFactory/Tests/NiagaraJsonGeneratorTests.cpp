#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "../NiagaraJsonGenerator.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLeafJsonContractTest, "Cooker.TreeLeafFall.JsonContract",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

// 真实作者模板可接受，但脚本注入、数量失控和不完整轨迹包围盒必须在写资产前拒绝。
bool FLeafJsonContractTest::RunTest(const FString& Parameters)
{
    FString Document, Error;
    if (!TestTrue(TEXT("读取作者模板"), FFileHelper::LoadFileToString(Document,
        *(FPaths::ProjectDir() / TEXT("Config/NiagaraTemplates/NS_TreeLeafFall.json"))))) return false;
    TestTrue(TEXT("正式配置通过"), UNiagaraJsonGenerator::ValidateJsonDefinition(Document, Error));
    TestFalse(TEXT("拒绝任意脚本"), UNiagaraJsonGenerator::ValidateJsonDefinition(Document.Replace(
        TEXT("\"Preset\": \"LeafFall\""), TEXT("\"Preset\": \"LeafFall\", \"HLSL\": \"injected\"")), Error));
    TestFalse(TEXT("拒绝无限发射"), UNiagaraJsonGenerator::ValidateJsonDefinition(Document.Replace(
        TEXT("\"SpawnRate\": 5"), TEXT("\"SpawnRate\": 9999")), Error));
    TestFalse(TEXT("拒绝轨迹超出固定包围盒"), UNiagaraJsonGenerator::ValidateJsonDefinition(Document.Replace(
        TEXT("\"FallSpeed\": 40"), TEXT("\"FallSpeed\": 200")), Error));
    TestFalse(TEXT("拒绝错误 JSON"), UNiagaraJsonGenerator::ValidateJsonDefinition(TEXT("[]"), Error));
    return !HasAnyErrors();
}
#endif
