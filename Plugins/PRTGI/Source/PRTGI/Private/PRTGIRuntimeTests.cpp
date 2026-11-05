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
        Scene->ProgressiveProbes=true;
        GPU=MakeUnique<PRTGI::FRuntimeGPU>();GPU->Submit(Scene,1);
    }
    virtual bool Update()override
    {
        TArray<FVector4f> SH;
        const bool Finished=GPU->Tick(SH);
        TArray<FVector4f> Partial;int32 First=0;
        if(GPU->TakePartial(Partial,First))
        {
            Test->TestEqual(TEXT("Probe chunks arrive in order"),First,PartialCount);
            Test->TestEqual(TEXT("One complete SH row per chunk"),Partial.Num(),9);
            if(Partial.Num()==9)Test->TestEqual(TEXT("Chunk includes computed validity"),Partial[0].W,First<2?1.f:-1.f);
            PartialCount+=Partial.Num()/9;
        }
        if(!Finished)
        {
            if(FPlatformTime::Seconds()-Start<30)return false;
            Test->AddError(TEXT("Runtime GPU analytic readback timed out"));return true;
        }
        if(!Test->TestEqual(TEXT("Three complete probe rows"),SH.Num(),27))return true;
        Test->TestEqual(TEXT("Every probe published once"),PartialCount,3);
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
    int32 PartialCount=0;
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

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPRTPartialPublication,"PRTGI.Runtime.PartialProbePublication",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FPRTPartialPublication::RunTest(const FString&)
{
    FPRTGIRenderVolume V;V.bRuntimeGeometry=true;V.GridCounts=FIntVector(2);
    V.GridMin=FVector3f(0);V.GridMax=FVector3f(10);V.ReceiverMax=FVector3f(10);
    auto Geometry=MakeShared<PRTGI::FMultiBounceData,ESPMode::ThreadSafe>();
    TArray<FPRTGIRenderVolume> Published,Layout{V,V};
    TArray<FVector4f> Chunk;Chunk.Init(FVector4f(2,3,4,0),18);Chunk[0].W=1;Chunk[9].W=-1;
    TestTrue(TEXT("Chunk can cross volume boundaries"),PRTGI::PublishRuntimeProbeRange(Published,Layout,Geometry,7,Chunk));
    TestEqual(TEXT("Both layouts exist"),Published.Num(),2);
    TestEqual(TEXT("Uncomputed probe is invalid, not black valid"),Published[0].Coefficients[0].W,-1.f);
    TestEqual(TEXT("Last probe of first volume ready"),Published[0].Coefficients[63].W,1.f);
    TestEqual(TEXT("Backface rejection preserved in second volume"),Published[1].Coefficients[0].W,-1.f);
    TestEqual(TEXT("Second volume receives its RGB"),Published[1].Coefficients[0].X,2.f);
    TestFalse(TEXT("Out-of-range publication rejected"),PRTGI::PublishRuntimeProbeRange(Published,Layout,Geometry,15,Chunk));
    Chunk.SetNum(9);Chunk[0].W=1;Chunk[0].X=7;
    TestTrue(TEXT("Later chunk preserves earlier ready probes"),PRTGI::PublishRuntimeProbeRange(Published,Layout,Geometry,0,Chunk));
    TestEqual(TEXT("Earlier chunk unchanged"),Published[0].Coefficients[63].X,2.f);
    TestEqual(TEXT("Later chunk installed"),Published[0].Coefficients[0].X,7.f);
    Layout[0].GridMin.X=1;
    TestTrue(TEXT("New layout starts with invalid unpublished rows"),PRTGI::PublishRuntimeProbeRange(Published,Layout,Geometry,0,Chunk));
    TestEqual(TEXT("Stale ready probe discarded on layout change"),Published[0].Coefficients[63].W,-1.f);
    return true;
}
#endif

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPRTPendingLighting,"PRTGI.Runtime.PendingLightingValidity",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FPRTPendingLighting::RunTest(const FString&)
{
    FPRTGIRenderVolume V;V.bRuntimeGeometry=true;V.GridCounts=FIntVector(2);
    V.GridMin=FVector3f(0);V.GridMax=FVector3f(10);V.ReceiverMax=FVector3f(10);
    V.Coefficients.Init(FVector4f(4,3,2,0),72);
    for(int32 P=0;P<8;++P)V.Coefficients[P*9].W=1;
    V.Coefficients[9].W=-1;
    auto Old=MakeShared<PRTGI::FMultiBounceData,ESPMode::ThreadSafe>();V.Geometry=Old;
    auto Current=MakeShared<PRTGI::FMultiBounceData,ESPMode::ThreadSafe>();
    TArray<FPRTGIRenderVolume> Published{V},Layout{V};int32 Masked=0;
    TestTrue(TEXT("Same layout retains lighting"),PRTGI::RetainRuntimeLighting(Published,Layout,Current,{FBox(FVector(-1),FVector(1))},Masked));
    TestEqual(TEXT("Only newly unsafe probe counted"),Masked,1);
    TestTrue(TEXT("Resolve uses latest BVH before lighting completes"),Published[0].Geometry==Current);
    TestEqual(TEXT("Edited-region probe rejected"),Published[0].Coefficients[0].W,-1.f);
    TestEqual(TEXT("Previously invalid probe stays invalid"),Published[0].Coefficients[9].W,-1.f);
    TestEqual(TEXT("Distant valid probe retained"),Published[0].Coefficients[63].W,1.f);
    TestTrue(TEXT("Stale radiance itself is preserved"),FVector3f(Published[0].Coefficients[0])==FVector3f(4,3,2));
    TestTrue(TEXT("Second pending edit retains previous masks"),PRTGI::RetainRuntimeLighting(Published,Layout,Current,{FBox(FVector(9),FVector(11))},Masked));
    TestEqual(TEXT("Second changed region counted once"),Masked,1);
    TestEqual(TEXT("First region remains masked during cancellation"),Published[0].Coefficients[0].W,-1.f);
    TestEqual(TEXT("Second region masked"),Published[0].Coefficients[63].W,-1.f);
    Layout[0].GridMin.X=1;
    TestFalse(TEXT("Moved probe layout cannot inherit old SH"),PRTGI::RetainRuntimeLighting(Published,Layout,Current,{},Masked));
    Layout[0]=V;Layout[0].Priority=2;
    TestFalse(TEXT("Changed volume selection rejects retention"),PRTGI::RetainRuntimeLighting(Published,Layout,Current,{},Masked));
    TestFalse(TEXT("Missing geometry rejects retention"),PRTGI::RetainRuntimeLighting(Published,{V},nullptr,{},Masked));
    return true;
}
#endif

#include "PRTGIRuntime.h"
#include "PRTGIRuntimeSurfel.h"
#include "PRTGIMultiBounce.h"
#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "RHI.h"

// An unoccluded floor sees constant analytic radiance. Two downward receivers
// see that floor. This has a closed-form Jacobi solution independent of sampling:
// F_n = pi*L*(1-q^n)
// R_n = D*(1-q^n) + rho*pi*L*(1-q^n-n*alpha*q^(n-1)).
// The synthetic environment is not connected to live scene sky lighting.
class FPRTCacheAnalyticCommand : public IAutomationLatentCommand
{
public:
    explicit FPRTCacheAnalyticCommand(FAutomationTestBase* InTest):Test(InTest)
    {
        Pool=MakeUnique<PRTGI::FRuntimeSurfelPool>(16,256,32);
        auto Asset=MakeShared<PRTGI::FRuntimeSurfelAsset,ESPMode::ThreadSafe>();
        for(int32 I=0;I<3;++I)
        {
            const FVector3f N(0,0,I==0?1.f:-1.f);
            Asset->Seeds.Append({FVector4f(0,0,I==0?0.f:1.f,0),FVector4f(N,0),FVector4f(1,0,0,0),
                FVector4f(0,1,0,0),FVector4f(.5f,.5f,.5f,0),FVector4f(N,0)});
        }
        FString Error;TArray<PRTGI::FRuntimeSurfelInstance> Items{{1,Asset,FTransform::Identity}};
        Test->TestTrue(TEXT("Fixture pool"),Pool->Update(Items,Error));Snapshot=Pool->CaptureLightingSnapshot();
    }
    bool Update()override
    {
        const int32 Case=Stage>=14?1:Stage%7;
        if(!GPU)
        {
            if(Stage==19)
            {
                // Move only the floor's lighting seed behind the fixed floor.
                // Its irradiance becomes zero. Receiver ray queries are identical,
                // but the second sweep must follow the changed triangle dependency.
                auto ChangedAsset=MakeShared<PRTGI::FRuntimeSurfelAsset,ESPMode::ThreadSafe>();
                for(int32 I=0;I<3;++I)
                {
                    const FVector3f N(0,0,I==0?1.f:-1.f);
                    ChangedAsset->Seeds.Append({FVector4f(0,0,I==0?-1.f:1.f,0),FVector4f(N,0),FVector4f(1,0,0,0),
                        FVector4f(0,1,0,0),FVector4f(.5f,.5f,.5f,0),FVector4f(N,0)});
                }
                Pool=MakeUnique<PRTGI::FRuntimeSurfelPool>(16,256,32);FString Error;
                TArray<PRTGI::FRuntimeSurfelInstance> Items{{1,ChangedAsset,FTransform::Identity}};
                Test->TestTrue(TEXT("Changed dependency fixture"),Pool->Update(Items,Error));Snapshot=Pool->CaptureLightingSnapshot();
            }
            auto Scene=MakeShared<PRTGI::FRuntimeScene,ESPMode::ThreadSafe>();
            Scene->Geometry=MakeShared<PRTGI::FMultiBounceData,ESPMode::ThreadSafe>();
            constexpr float Size=10000;
            Scene->Geometry->Triangles={FVector4f(-Size,-Size,0,1),FVector4f(2*Size,0,0,0),FVector4f(2*Size,2*Size,0,0),
                FVector4f(-Size,-Size,0,1),FVector4f(2*Size,2*Size,0,0),FVector4f(0,2*Size,0,0)};
            Scene->Geometry->Nodes={FVector4f(-Size,-Size,0,0),FVector4f(Size,Size,0,2)};
            for(int32 I=0;I<128;++I){Scene->Materials.Add(FVector4f(.5f,.5f,.5f,0));Scene->Materials.Add(FVector4f(0,0,1,0));}
            Scene->SurfelBindings={FVector4f(0,1,1,1),FVector4f(0,1,1,1)};
            if(Stage==20)Scene->SurfelBindings.SetNumZeroed(65536); // Exercise 2D dispatch beyond D3D's X limit.
            Scene->SurfelSnapshot=Snapshot;Scene->ReadCacheForTest=true;
            Iterations=Case==0?1:((Case<3 || Case>=5)?2:8);Alpha=Case==3?.5f:1.f;
            Scene->CacheIterations=Iterations;Scene->CacheRelaxation=Alpha;Scene->CacheBatch=Case==2?256:1;
            Scene->CacheBatchesPerSubmit=Case==5?2:(Case==6?8:1);
            Scene->ReuseTransport=Stage>=7;Scene->RegionalTransport=Stage>=7;Scene->RegionalIrradiance=Stage>=7;
            if(Stage>=15)
            {
                Scene->OldToNewTriangles={0,1};
                if(Stage==16)Scene->ChangedBounds=FBox3f(FVector3f(-20000000),FVector3f(20000000));
                if(Stage==17)Scene->ChangedBounds=FBox3f(FVector3f(-20000000,-20000000,2),FVector3f(20000000,20000000,3));
                if(Stage==18)Scene->ChangedBounds=FBox3f(FVector3f(-20000000,-20000000,-3),FVector3f(20000000,20000000,-2));
            }
            if(Stage==14)Scene->TransportBudgetBytes=0;
            if(Stage==21)Scene->IrradianceBudgetBytes=0;
            if(Stage==22)Scene->IrradianceBudgetBytes=600; // New 512B history fits; old+new does not.
            Scene->CacheRays=64;Scene->Rays=1024;Scene->Probes={FVector4f(0,0,1,1)};
            Scene->AnalyticEnvironment=Case==4?FVector3f(0):FVector3f(2,3,5);
            if(Case!=4)Scene->Lights={FVector4f(0,0,1,0),FVector4f(2*UE_PI,2*UE_PI,2*UE_PI,0)};
            GPU=MakeUnique<PRTGI::FRuntimeGPU>();GPU->Submit(Scene,1,Stage>=15?History:nullptr);Started=FPlatformTime::Seconds();
        }
        TArray<FVector4f> SH;
        // Room scheduling must retire pending work without granting another
        // dispatch. Alternate grants and pauses, including the initial tick.
        const bool AllowSubmission=(++Ticks%2)==0;
        const bool WasPending=GPU->HasPendingSubmission();
        const bool Finished=GPU->Tick(SH,AllowSubmission);
        if(!AllowSubmission && !WasPending)
            Test->TestFalse(TEXT("Paused solver cannot submit GPU work"),GPU->HasPendingSubmission());
        if(!Finished)
        { if(FPlatformTime::Seconds()-Started<30)return false;Test->AddError(TEXT("Cache analytic timeout"));return true; }
        const auto& Cache=GPU->CacheResultForTest();
        if(!Test->TestEqual(TEXT("Full diagnostic cache"),Cache.Num(),16) || !Test->TestEqual(TEXT("Probe SH"),SH.Num(),9))return true;
        const float Q=1-Alpha,Power=FMath::Pow(Q,float(Iterations));
        for(int32 C=0;C<3;++C)
        {
            const float L=Case==4 || Stage>=19?0:FVector3f(2,3,5)[C];
            const float Floor=UE_PI*L*(1-Power);
            const float Direct=Case==4?0:UE_PI;
            const float Receiver=Direct*(1-Power)+.5f*UE_PI*L*(1-Power-Iterations*Alpha*FMath::Pow(Q,float(Iterations-1)));
            Test->TestTrue(FString::Printf(TEXT("Stage %d E=pi L channel %d actual %g expected %g"),Stage,C,Cache[0][C],Floor),FMath::IsNearlyEqual(Cache[0][C],Floor,.0001f));
            for(int32 I=1;I<3;++I)Test->TestTrue(FString::Printf(TEXT("Stage %d Jacobi receiver %d channel %d actual %g expected %g"),Stage,I,C,Cache[I][C],Receiver),FMath::IsNearlyEqual(Cache[I][C],Receiver,.0002f));
            const float Radiance=(Direct+Floor*.5f)/UE_PI;
            const float Expected=Radiance*2*UE_PI*.2820947918f*UE_PI;
            Test->TestTrue(TEXT("Cache probe projection applies rho/pi exactly once"),FMath::IsNearlyEqual(SH[0][C],Expected,.004f));
        }
        for(int32 I=3;I<Cache.Num();++I)Test->TestTrue(TEXT("Inactive slots stay zero"),Cache[I]==FVector4f(0,0,0,0));
        if(Case==1)Previous=Cache;
        if(Case==2 || Case>=5)Test->TestTrue(TEXT("Jacobi results independent of batch/submission boundaries"),Cache==Previous);
        if(Stage<7){ExpectedCache.Add(Cache);ExpectedSH.Add(SH);}
        else if(Stage<19)
        {
            Test->TestTrue(TEXT("Transport reuse preserves analytic cache exactly"),Cache==ExpectedCache[Case]);
            Test->TestTrue(TEXT("Transport reuse preserves analytic probe SH exactly"),SH==ExpectedSH[Case]);
        }
        const bool ExpectedReuse=Stage>=7 && Case!=0 && Stage!=14;
        Test->TestTrue(TEXT("Single-sweep and budget fallback selection"),GPU->Progress().Contains(ExpectedReuse?TEXT("TransportReuse=1"):TEXT("TransportReuse=0")));
        if(Stage==8){GPU->RetainCacheAndReleaseScratch();History=GPU->CaptureTransportHistory();Test->TestTrue(TEXT("Completed query history retained"),History.IsValid());}
        if(Stage==15)Test->TestTrue(TEXT("Unchanged dependency queries all reused"),GPU->Progress().Contains(TEXT("QueriesReused=192 QueriesRebuilt=0")));
        if(Stage==16)Test->TestTrue(TEXT("Changed bounds invalidate all ray segments"),GPU->Progress().Contains(TEXT("QueriesReused=0 QueriesRebuilt=192")));
        if(Stage==17)Test->TestTrue(TEXT("Shadow segments invalidate hits outside the changed region"),GPU->Progress().Contains(TEXT("QueriesReused=0 QueriesRebuilt=192")));
        if(Stage==18)Test->TestTrue(TEXT("Geometry behind first hits does not invalidate queries"),GPU->Progress().Contains(TEXT("QueriesReused=192 QueriesRebuilt=0")));
        if(Stage==15 || Stage==18 || Stage==20 || Stage==23)
            Test->TestTrue(TEXT("Unchanged second sweep reuses all irradiance"),GPU->Progress().Contains(TEXT("IrradianceReused=3 IrradianceRebuilt=3")));
        if(Stage==21 || Stage==22)
        {
            Test->TestTrue(TEXT("Budget fallback drops old irradiance ownership"),GPU->Progress().Contains(TEXT("HeldIrradianceHistoryBytes=0")));
            Test->TestTrue(TEXT("Budget fallback still reuses queries"),GPU->Progress().Contains(TEXT("QueriesReused=192 QueriesRebuilt=0")));
            Test->TestTrue(TEXT("Budget fallback evaluates all sweeps"),GPU->Progress().Contains(TEXT("IrradianceReused=0 IrradianceRebuilt=0")) || GPU->Progress().Contains(TEXT("IrradianceReused=0 IrradianceRebuilt=6")));
            Test->TestTrue(TEXT("Budget selects only new history when it fits"),GPU->Progress().Contains(Stage==21?TEXT("IrradianceHistoryBytes=0"):TEXT("IrradianceHistoryBytes=512")));
        }
        if(Stage==19)
        {
            Test->TestTrue(TEXT("Receiver queries reused while moved source retraces"),GPU->Progress().Contains(TEXT("QueriesReused=128 QueriesRebuilt=64")));
            Test->TestTrue(TEXT("Changed indirect dependency recomputes second sweep"),GPU->Progress().Contains(TEXT("IrradianceReused=0 IrradianceRebuilt=6")));
            GPU->RetainCacheAndReleaseScratch();History=GPU->CaptureTransportHistory();
        }
        GPU.Reset();++Stage;return Stage==24;
    }
private:
    FAutomationTestBase* Test;
    TUniquePtr<PRTGI::FRuntimeSurfelPool> Pool;
    TSharedPtr<PRTGI::FRuntimeSurfelSnapshot,ESPMode::ThreadSafe> Snapshot;
    TUniquePtr<PRTGI::FRuntimeGPU> GPU;
    TArray<FVector4f> Previous;
    TArray<TArray<FVector4f>> ExpectedCache,ExpectedSH;
    TSharedPtr<PRTGI::FRuntimeTransportHistory,ESPMode::ThreadSafe> History;
    int32 Stage=0,Iterations=0,Ticks=0;float Alpha=1;double Started=0;
};
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPRTCacheAnalytic,"PRTGI.Runtime.SurfelCacheAnalytic",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter|EAutomationTestFlags::NonNullRHI)
bool FPRTCacheAnalytic::RunTest(const FString&)
{
    if(GUsingNullRHI){AddError(TEXT("GPU required"));return false;}
    FAutomationTestFramework::Get().EnqueueLatentCommand(MakeShared<FPRTCacheAnalyticCommand>(this));return true;
}
#endif
