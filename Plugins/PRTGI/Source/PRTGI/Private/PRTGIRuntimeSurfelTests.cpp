#include "PRTGIRuntimeSurfel.h"
#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "RHI.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/HierarchicalInstancedStaticMeshComponent.h"
#include "Elements/SMInstance/SMInstanceElementId.h"
#include "UObject/StrongObjectPtr.h"

namespace
{
TSharedPtr<PRTGI::FRuntimeSurfelAsset,ESPMode::ThreadSafe> TestAsset(uint32 Count)
{
    auto Asset=MakeShared<PRTGI::FRuntimeSurfelAsset,ESPMode::ThreadSafe>();
    for(uint32 I=0;I<Count;++I)
        Asset->Seeds.Append({FVector4f(I*1.3f,-.7f,.2f,0),FVector4f(.2f,.4f,1,0),FVector4f(.7f,.2f,0,0),
            FVector4f(-.3f,.6f,0,0),FVector4f(.2f,.5f,.8f,0),FVector4f(0,0,1,0)});
    return Asset;
}
class FSurfelLifecycleCommand : public IAutomationLatentCommand
{
public:
    explicit FSurfelLifecycleCommand(FAutomationTestBase* InTest):Test(InTest)
    { Pool=MakeUnique<PRTGI::FRuntimeSurfelPool>(512,256,2.f);Asset=TestAsset(2); }
    bool Update()override
    {
        FString Message;bool Passed=false;
        if(Pending)
        {
            if(!Pool->PollAudit(Message,Passed))
            { if(FPlatformTime::Seconds()-Started<30)return false;Test->AddError(TEXT("Surfel GPU audit timed out"));return true; }
            Test->TestTrue(FString::Printf(TEXT("Stage %d: %s"),Stage,*Message),Passed);
            if(!Passed)return true;Pending=false;++Stage;
        }
        if(Stage==7)return true;
        if(Stage==1)
        {
            // More surfels than buckets forces collisions; exact cell keys must
            // reject unrelated entries. Several radius levels and negative cells.
            for(uint32 I=0;I<130;++I)
            {
                const float Scale=I%7==0?64.f:1.f;
                Items.Add({I+1,Asset,FTransform(FRotator(0,I,0),FVector(int32(I)*7-455,int32(I%9)*11-44,-8),FVector(Scale))});
            }
        }
        if(Stage==2)Items[16].Transform=FTransform(FRotator(23,71,5),FVector(-77,33,-44),FVector(-2,.5,3));
        if(Stage==3) { Items.RemoveAt(2);Items.Add({555,Asset,FTransform(FVector(-32,-32,-32))}); }
        if(Stage==4)Items[0].Asset=TestAsset(3);
        if(Stage==5)Items.Reset();
        if(Stage==6)
        {
            const uint32 Previous=Pool->Revision();
            TArray<PRTGI::FRuntimeSurfelInstance> Bad{{1,TestAsset(513),FTransform::Identity}};
            Test->TestFalse(TEXT("Capacity exhaustion is rejected"),Pool->Update(Bad,Message));
            Test->TestEqual(TEXT("Failed transaction preserves revision"),Pool->Revision(),Previous);
            Bad={{1,Asset,FTransform::Identity},{1,Asset,FTransform::Identity}};
            Test->TestFalse(TEXT("Duplicate owner rejected"),Pool->Update(Bad,Message));
            Bad={{1,Asset,FTransform(FQuat::Identity,FVector::ZeroVector,FVector(0,1,1))}};
            Test->TestFalse(TEXT("Singular transform rejected"),Pool->Update(Bad,Message));
            Items={{777,Asset,FTransform(FRotator(11,32,43),FVector(-.1,-32.1,63.9),FVector(.1,4,-2))}};
        }
        if(!Pool->Update(Items,Message)) { Test->AddError(Message);return true; }
        const uint32 Revision=Pool->Revision();
        Test->TestTrue(TEXT("Identical update succeeds"),Pool->Update(Items,Message));
        Test->TestEqual(TEXT("Identical transforms do not rebuild"),Pool->Revision(),Revision);
        if(!Pool->RequestAudit(Message)) { Test->AddError(Message);return true; }
        Pending=true;Started=FPlatformTime::Seconds();return false;
    }
private:
    FAutomationTestBase* Test;
    TUniquePtr<PRTGI::FRuntimeSurfelPool> Pool;
    TSharedPtr<PRTGI::FRuntimeSurfelAsset,ESPMode::ThreadSafe> Asset;
    TArray<PRTGI::FRuntimeSurfelInstance> Items;
    double Started=0;
    int32 Stage=0;
    bool Pending=false;
};
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPRTSurfelPoolLifecycle,"PRTGI.Runtime.SurfelPoolLifecycle",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter|EAutomationTestFlags::NonNullRHI)
bool FPRTSurfelPoolLifecycle::RunTest(const FString&)
{
    if(GUsingNullRHI){AddError(TEXT("GPU required"));return false;}
    FAutomationTestFramework::Get().EnqueueLatentCommand(MakeShared<FSurfelLifecycleCommand>(this));return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPRTSurfelInstanceIdentity,"PRTGI.Runtime.SurfelInstanceIdentity",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FPRTSurfelInstanceIdentity::RunTest(const FString&)
{
    for(bool Hierarchical:{false,true})
    {
        TStrongObjectPtr<UInstancedStaticMeshComponent> C(Hierarchical?
            NewObject<UHierarchicalInstancedStaticMeshComponent>(GetTransientPackage()):NewObject<UInstancedStaticMeshComponent>(GetTransientPackage()));
        for(int32 I=0;I<3;++I)C->AddInstance(FTransform(FVector(I*20,0,0)),false);
        auto& Map=FSMInstanceElementIdMap::Get();
        const auto Removed=Map.GetSMInstanceElementIdFromSMInstanceId({C.Get(),0});
        const auto Retained=Map.GetSMInstanceElementIdFromSMInstanceId({C.Get(),1});
        C->RemoveInstance(0);
        const auto NewIndex=Map.GetSMInstanceIdFromSMInstanceElementId(Retained);
        TestTrue(TEXT("Surviving instance identity remains valid"),bool(NewIndex));
        FTransform Transform;
        if(NewIndex) { C->GetInstanceTransform(NewIndex.InstanceIndex,Transform,false);TestEqual(TEXT("Retained ID still identifies original transform"),Transform.GetLocation().X,20.); }
        TestFalse(TEXT("Removed identity is unmapped"),bool(Map.GetSMInstanceIdFromSMInstanceElementId(Removed)));
        const int32 AddedIndex=C->AddInstance(FTransform(FVector(100,0,0)),false);
        const auto Added=Map.GetSMInstanceElementIdFromSMInstanceId({C.Get(),AddedIndex});
        TestTrue(TEXT("New instance does not reuse removed identity"),Added!=Removed);
        C->ClearInstances();TestFalse(TEXT("Clear retires retained identity"),bool(Map.GetSMInstanceIdFromSMInstanceElementId(Retained)));
        C->DestroyComponent();
    }
    return true;
}
#endif
