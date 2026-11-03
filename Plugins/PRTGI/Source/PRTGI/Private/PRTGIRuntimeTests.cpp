#include "PRTGIRuntime.h"
#include "PRTGIMultiBounce.h"
#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "RHI.h"

// An analytic radiance hemisphere checks units, SH band convolution, GPU
// traversal, batch boundaries and validity independently of the Showcase.
class FPRTRuntimeHemisphereCommand : public IAutomationLatentCommand
{
public:
    explicit FPRTRuntimeHemisphereCommand(FAutomationTestBase* InTest):Test(InTest),Start(FPlatformTime::Seconds())
    {
        auto Scene=MakeShared<PRTGI::FRuntimeScene,ESPMode::ThreadSafe>();
        Scene->Geometry=MakeShared<PRTGI::FMultiBounceData,ESPMode::ThreadSafe>();
        constexpr float Size=10000;
        Scene->Geometry->Triangles={FVector4f(-Size,-Size,0,1),FVector4f(2*Size,0,0,0),FVector4f(2*Size,2*Size,0,0),
            FVector4f(-Size,-Size,0,1),FVector4f(2*Size,2*Size,0,0),FVector4f(0,2*Size,0,0)};
        Scene->Geometry->Nodes={FVector4f(-Size,-Size,0,0),FVector4f(Size,Size,0,2)};
        for(int32 I=0;I<128;++I) { Scene->Materials.Add(FVector4f(.5f,.5f,.5f,0));Scene->Materials.Add(FVector4f(0,0,1,0)); }
        Scene->Lights={FVector4f(0,0,1,0),FVector4f(UE_PI,UE_PI,UE_PI,0)};
        // The most grazing Fibonacci ray must still hit within the 64 m
        // transport radius; a high probe would truncate the analytic hemisphere.
        Scene->Probes={FVector4f(0,0,1,1),FVector4f(0,0,1,1),FVector4f(0,0,-1,1)};
        Scene->Rays=1024;Scene->Bounces=6;
        GPU=MakeUnique<PRTGI::FRuntimeGPU>();GPU->Submit(Scene,1);
    }
    virtual bool Update()override
    {
        TArray<FVector4f> SH;
        if(!GPU->Tick(SH))
        {
            if(FPlatformTime::Seconds()-Start<30)return false;
            Test->AddError(TEXT("Runtime GPU analytic readback timed out"));return true;
        }
        if(!Test->TestEqual(TEXT("Three complete probe rows"),SH.Num(),27))return true;
        const float L0=.5f*2*UE_PI*.2820947918f*UE_PI;
        const float L1Z=-.5f*UE_PI*.4886025119f*(2*UE_PI/3);
        Test->TestTrue(FString::Printf(TEXT("Lambert L0: actual %.9g expected %.9g"),SH[0].X,L0),FMath::IsNearlyEqual(SH[0].X,L0,.003f));
        Test->TestTrue(FString::Printf(TEXT("Lower-hemisphere L1Z: actual %.9g expected %.9g"),SH[2].X,L1Z),FMath::IsNearlyEqual(SH[2].X,L1Z,.003f));
        Test->TestTrue(TEXT("Batch boundary preserves identical analytic lighting"),FVector3f(SH[0]).Equals(FVector3f(SH[9]),1.e-5f));
        Test->TestEqual(TEXT("Front-side probe valid"),SH[0].W,1.f);
        Test->TestEqual(TEXT("Back-side probe rejected"),SH[18].W,-1.f);
        for(const auto& C:SH)Test->TestFalse(TEXT("All SH coefficients finite"),C.ContainsNaN());
        return true;
    }
private:
    FAutomationTestBase* Test;
    double Start;
    TUniquePtr<PRTGI::FRuntimeGPU> GPU;
};
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPRTRuntimeHemisphere,"PRTGI.Runtime.AnalyticHemisphere",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter | EAutomationTestFlags::NonNullRHI)
bool FPRTRuntimeHemisphere::RunTest(const FString&)
{
    if(GUsingNullRHI) { AddError(TEXT("This test requires a GPU"));return false; }
    FAutomationTestFramework::Get().EnqueueLatentCommand(MakeShared<FPRTRuntimeHemisphereCommand>(this));
    return true;
}
#endif
