#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "../NiagaraJsonGenerator.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"

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
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSmokeJsonContractTest, "Cooker.ChimneySmoke.JsonContract",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

// 使用实际模板对象改变单项约束，检查未知字段、帧越界和完整烟轨迹，而非依赖旧文本数值替换。
bool FSmokeJsonContractTest::RunTest(const FString& Parameters)
{
    FString Document, Error;
    if (!TestTrue(TEXT("读取烟作者模板"), FFileHelper::LoadFileToString(Document,
        *(FPaths::ProjectDir() / TEXT("Config/NiagaraTemplates/NS_ChimneySmoke.json"))))) return false;
    TestTrue(TEXT("正式烟配置通过"), UNiagaraJsonGenerator::ValidateJsonDefinition(Document, Error));
    TSharedPtr<FJsonObject> Json;
    FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Document), Json);
    Json->SetNumberField(TEXT("RiseSpeed"), 1000);
    FString Changed; FJsonSerializer::Serialize(Json.ToSharedRef(), TJsonWriterFactory<>::Create(&Changed));
    TestFalse(TEXT("轨迹越界在写入前拒绝"), UNiagaraJsonGenerator::ValidateJsonDefinition(Changed, Error));
    FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Document), Json);
    Json->SetArrayField(TEXT("FrameRange"), {MakeShared<FJsonValueNumber>(0), MakeShared<FJsonValueNumber>(64)});
    Changed.Empty(); FJsonSerializer::Serialize(Json.ToSharedRef(), TJsonWriterFactory<>::Create(&Changed));
    TestFalse(TEXT("8x8帧索引64越界"), UNiagaraJsonGenerator::ValidateJsonDefinition(Changed, Error));
    FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Document), Json);
    Json->SetStringField(TEXT("HLSL"), TEXT("injected"));
    Changed.Empty(); FJsonSerializer::Serialize(Json.ToSharedRef(), TJsonWriterFactory<>::Create(&Changed));
    TestFalse(TEXT("不接受作者脚本注入"), UNiagaraJsonGenerator::ValidateJsonDefinition(Changed, Error));
    return !HasAnyErrors();
}
#endif
