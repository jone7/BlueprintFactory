#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "../BPFactoryBlueprintLibrary.h"
#include "Animation/AnimBlueprint.h"
#include "Animation/AnimInstance.h"
#include "Factories/AnimBlueprintFactory.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Dom/JsonObject.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCookerLocomotionPlayerRoundTripTest, "Cooker.Character.Locomotion.AnimationRoundTrip",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

// 只在 Transient 包生成模板图，验证速率、冻结姿态、落地起点与坐睡状态的导出往返。
bool FCookerLocomotionPlayerRoundTripTest::RunTest(const FString& Parameters)
{
    FString Document;
    if (!TestTrue(TEXT("读取共享父模板"), FFileHelper::LoadFileToString(Document,
        *(FPaths::ProjectDir()/TEXT("Config/BlueprintTemplates/ABP_Hero.json"))))) return false;
    TSharedPtr<FJsonObject> Root;
    FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Document), Root);
    FString Definition;
    FJsonSerializer::Serialize(Root->GetObjectField(TEXT("StateMachineDefinition")).ToSharedRef(), TJsonWriterFactory<>::Create(&Definition));
    auto* Factory=NewObject<UAnimBlueprintFactory>();
    Factory->bTemplate=true; Factory->ParentClass=UAnimInstance::StaticClass();
    auto* Blueprint=Cast<UAnimBlueprint>(Factory->FactoryCreateNew(UAnimBlueprint::StaticClass(), GetTransientPackage(),
        MakeUniqueObjectName(GetTransientPackage(), UAnimBlueprint::StaticClass(), TEXT("LocomotionContract")), RF_Transient, nullptr, GWarn));
    if (!TestNotNull(TEXT("创建临时模板"), Blueprint)) return false;
    if (!TestTrue(TEXT("生成共享状态机"), UBPFactoryBlueprintLibrary::SetupAnimStateMachineFromJson(Blueprint, Definition))) return false;
    TSharedPtr<FJsonObject> Exported;
    FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(UBPFactoryBlueprintLibrary::ExportAnimBlueprintMetadataToJson(Blueprint)), Exported);
    const auto& States=Exported->GetObjectField(TEXT("StateMachineDefinition"))->GetArrayField(TEXT("States"));
    TestEqual(TEXT("保留坐睡并增加四个移动状态"), States.Num(), 8);
    for (const auto& Value : States)
    {
        const auto State=Value->AsObject(); const FString Name=State->GetStringField(TEXT("Name"));
        if (Name==TEXT("InAir"))
        {
            TestFalse(TEXT("空中不循环跳跃"), State->GetBoolField(TEXT("LoopAnimation")));
            TestEqual(TEXT("空中冻结播放"), State->GetNumberField(TEXT("PlayRate")), 0.);
            TestEqual(TEXT("采样点绑定往返"), State->GetStringField(TEXT("StartPositionVariable")), FString(TEXT("AirPoseTime")));
        }
        if (Name==TEXT("Land")) TestEqual(TEXT("落地起点绑定往返"), State->GetStringField(TEXT("StartPositionVariable")), FString(TEXT("LandStartTime")));
        if (Name==TEXT("Walk") || Name==TEXT("Sprint"))
            TestEqual(TEXT("地面速率绑定往返"), State->GetStringField(TEXT("PlayRateVariable")), FString(TEXT("GroundPlayRate")));
    }
    return !HasAnyErrors();
}
#endif
