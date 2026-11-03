#include "PRTGIRuntime.h"
#include "PRTGIRuntimeSurfel.h"
#include "PRTGIMultiBounce.h"
#include "GlobalShader.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "RenderingThread.h"
#include "ShaderParameterStruct.h"
#include "RHIGPUReadback.h"

namespace
{
class FPRTRuntimeCS : public FGlobalShader
{
    DECLARE_GLOBAL_SHADER(FPRTRuntimeCS);
    SHADER_USE_PARAMETER_STRUCT(FPRTRuntimeCS,FGlobalShader);
    BEGIN_SHADER_PARAMETER_STRUCT(FParameters,)
        SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>,Triangles)
        SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>,Nodes)
        SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>,Materials)
        SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>,Lights)
        SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>,Probes)
        SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<float4>,OutSH)
        SHADER_PARAMETER(uint32,NodeCount)
        SHADER_PARAMETER(uint32,LightCount)
        SHADER_PARAMETER(uint32,ProbeOffset)
        SHADER_PARAMETER(uint32,ProbeCount)
        SHADER_PARAMETER(uint32,RayCount)
        SHADER_PARAMETER(uint32,BounceCount)
        SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>,SurfacePool)
        SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>,ActiveSlots)
        SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>,SurfelBindings)
        SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>,PreviousIrradiance)
        SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<float4>,NextIrradiance)
        SHADER_PARAMETER(uint32,CacheEnabled)
        SHADER_PARAMETER(uint32,CacheOffset)
        SHADER_PARAMETER(uint32,CacheActiveCount)
        SHADER_PARAMETER(uint32,CacheRayCount)
        SHADER_PARAMETER(uint32,CacheIteration)
        SHADER_PARAMETER(float,CacheRelaxation)
        SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<float4>,TransportQueries)
        SHADER_PARAMETER(uint32,ReuseTransport)
        SHADER_PARAMETER(uint32,QueryStride)
        SHADER_PARAMETER(uint32,RegionalReuse)
        SHADER_PARAMETER(uint32,HasChangedBounds)
        SHADER_PARAMETER(FVector3f,ChangedMin)
        SHADER_PARAMETER(FVector3f,ChangedMax)
        SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>,OldQueries)
        SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>,OldSurfacePool)
        SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>,OldToNewTriangles)
        SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>,QueryStats)
        SHADER_PARAMETER(FVector3f,AnalyticEnvironment)
    END_SHADER_PARAMETER_STRUCT()
};
class FPRTCacheCS : public FGlobalShader
{
    DECLARE_GLOBAL_SHADER(FPRTCacheCS);
    SHADER_USE_PARAMETER_STRUCT(FPRTCacheCS,FGlobalShader);
    using FParameters=FPRTRuntimeCS::FParameters;
};
IMPLEMENT_GLOBAL_SHADER(FPRTCacheCS,"/Plugin/PRTGI/Private/PRTGIRuntime.usf","CacheCS",SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FPRTRuntimeCS,"/Plugin/PRTGI/Private/PRTGIRuntime.usf","RuntimeCS",SF_Compute);
}
namespace PRTGI
{
struct FRuntimeGPU::FState
{
    TSharedPtr<const FRuntimeScene,ESPMode::ThreadSafe> Scene;
    TRefCountPtr<FRDGPooledBuffer> Triangles,Nodes,Materials,Lights,Probes,SH;
    TRefCountPtr<FRDGPooledBuffer> Bindings,Active,Irradiance[2];
    TRefCountPtr<FRDGPooledBuffer> TransportQueries,TriangleRemap,QueryStats;
    TSharedPtr<FRuntimeTransportHistory,ESPMode::ThreadSafe> History;
    TUniquePtr<FRHIGPUBufferReadback> StatsReadback;
    uint32 ReusedQueries=0,RebuiltQueries=0,QueryStride=2;
    bool RegionalReuse=false;
    uint64 TransportBytes=0;
    bool ReuseTransport=false,ScratchReleased=false;
    TUniquePtr<FRHIGPUBufferReadback> Readback,CacheReadback;
    FGPUFenceRHIRef Fence;
    int32 SubmittedSurfels=0,Submissions=0;
    TArray<FVector4f> CacheResult;
    int32 CacheOffset=0,Iteration=0,Current=0;
    bool CachePhase=false;
    TAtomic<bool> Ready{false},PollQueued{false};
    TArray<FVector4f> Result;
    int32 Offset=0,Budget=32;
    bool Pending=false;
};
FRuntimeTransportHistory::~FRuntimeTransportHistory()
{
    auto Buffer=MoveTemp(Queries);
    ENQUEUE_RENDER_COMMAND(PRTReleaseTransportHistory)([Buffer](FRHICommandListImmediate&){});
}
TSharedPtr<FRuntimeTransportHistory,ESPMode::ThreadSafe> FRuntimeGPU::CaptureTransportHistory() const
{
    auto S=State;
    if(!S->ScratchReleased || !S->ReuseTransport || !S->Scene->RegionalTransport)return nullptr;
    auto History=MakeShared<FRuntimeTransportHistory,ESPMode::ThreadSafe>();
    History->Surface=S->Scene->SurfelSnapshot;History->Rays=S->Scene->CacheRays;History->Bytes=S->TransportBytes;
    ENQUEUE_RENDER_COMMAND(PRTCaptureTransportHistory)([S,History](FRHICommandListImmediate&){History->Queries=S->TransportQueries;});
    return History;
}
FRuntimeGPU::FRuntimeGPU():State(MakeShared<FState,ESPMode::ThreadSafe>()){}
FRuntimeGPU::~FRuntimeGPU()
{
    auto S=MoveTemp(State);
    ENQUEUE_RENDER_COMMAND(PRTReleaseRuntime)([S](FRHICommandListImmediate&)
    {
        S->Fence.SafeRelease();S->Readback.Reset();S->CacheReadback.Reset();S->Triangles.SafeRelease();S->Nodes.SafeRelease();
        S->Bindings.SafeRelease();S->Active.SafeRelease();S->Irradiance[0].SafeRelease();S->Irradiance[1].SafeRelease();
        S->Materials.SafeRelease();S->Lights.SafeRelease();S->Probes.SafeRelease();S->SH.SafeRelease();
        S->TransportQueries.SafeRelease();S->TriangleRemap.SafeRelease();S->QueryStats.SafeRelease();S->StatsReadback.Reset();
    });
}
void FRuntimeGPU::Submit(TSharedPtr<const FRuntimeScene,ESPMode::ThreadSafe> Scene,int32 InProbeBudget,TSharedPtr<FRuntimeTransportHistory,ESPMode::ThreadSafe> History)
{
    check(!State->Scene);State->Scene=Scene;State->Budget=FMath::Clamp(InProbeBudget,1,256);
    State->CachePhase=Scene->SurfelSnapshot && !Scene->SurfelSnapshot->ActiveSlots.IsEmpty();
    State->QueryStride=Scene->RegionalTransport?3:2;
    const uint64 Bytes=State->CachePhase?uint64(Scene->SurfelSnapshot->ActiveSlots.Num())*Scene->CacheRays*State->QueryStride*16:0;
    State->ReuseTransport=Scene->ReuseTransport && Scene->CacheIterations>1 && Bytes>0 && Bytes<=Scene->TransportBudgetBytes;
    State->TransportBytes=State->ReuseTransport?Bytes:0;
    if(State->ReuseTransport && Scene->RegionalTransport && History && History->Rays==Scene->CacheRays &&
        History->Surface->Capacity==Scene->SurfelSnapshot->Capacity && History->Surface->ActiveSlots==Scene->SurfelSnapshot->ActiveSlots &&
        History->Bytes+Bytes<=Scene->TransportBudgetBytes && !Scene->OldToNewTriangles.IsEmpty())State->History=History;
    State->RegionalReuse=State->History.IsValid();
}
const TArray<FVector4f>& FRuntimeGPU::CacheResultForTest()const { return State->CacheResult; }
FString FRuntimeGPU::Progress()const
{
    const auto& S=*State;
    return FString::Printf(TEXT("CacheSweep=%d/%d CacheOffset=%d CachePhase=%d Submissions=%d TransportReuse=%d TransportBytes=%llu TransportRetained=%d RegionalReuse=%d QueriesReused=%u QueriesRebuilt=%u"),S.Iteration,S.Scene?S.Scene->CacheIterations:0,S.CacheOffset,S.CachePhase,S.Submissions,S.ReuseTransport,S.TransportBytes,S.ReuseTransport && (!S.ScratchReleased || S.Scene->RegionalTransport),S.RegionalReuse,S.ReusedQueries,S.RebuiltQueries);
}
void FRuntimeGPU::RetainCacheAndReleaseScratch()
{
    auto S=State;
    S->ScratchReleased=true;S->History.Reset();
    ENQUEUE_RENDER_COMMAND(PRTRetainSolvedCache)([S](FRHICommandListImmediate&)
    {
        // Preserve the final irradiance and immutable surface snapshot until
        // invalidation. Projection and solver inputs are no longer needed.
        S->Triangles.SafeRelease();S->Nodes.SafeRelease();S->Materials.SafeRelease();
        S->Lights.SafeRelease();S->Probes.SafeRelease();S->SH.SafeRelease();
        S->Bindings.SafeRelease();S->Active.SafeRelease();S->Irradiance[1-S->Current].SafeRelease();
        if(!S->Scene->RegionalTransport)S->TransportQueries.SafeRelease();
        S->TriangleRemap.SafeRelease();S->QueryStats.SafeRelease();
    });
}
bool FRuntimeGPU::HasPendingSubmission() const { return State->Pending; }
bool FRuntimeGPU::Tick(TArray<FVector4f>& Output,bool bAllowSubmission)
{
    auto S=State;if(!S->Scene)return false;
    if(S->Pending)
    {
        if(!S->Ready.Load())
        {
            if(!S->PollQueued.Exchange(true))
            {
                ENQUEUE_RENDER_COMMAND(PRTPollRuntime)([S](FRHICommandListImmediate&)
                {
                    if((S->Readback?S->Readback->IsReady():(S->Fence && S->Fence->Poll())) && (!S->CacheReadback || S->CacheReadback->IsReady()) && (!S->StatsReadback || S->StatsReadback->IsReady()))
                    {
                        if(S->Readback)
                        {
                            const uint32 Bytes=S->Scene->Probes.Num()*9*16;
                            if(const void* Ptr=S->Readback->Lock(Bytes))
                            { S->Result.SetNumUninitialized(S->Scene->Probes.Num()*9);FMemory::Memcpy(S->Result.GetData(),Ptr,Bytes);S->Readback->Unlock(); }
                        }
                        S->Fence.SafeRelease();
                        if(S->CacheReadback)
                        {
                            const uint32 Size=S->Scene->SurfelSnapshot->Capacity*16;
                            if(const void* Ptr=S->CacheReadback->Lock(Size))
                            { S->CacheResult.SetNumUninitialized(Size/16);FMemory::Memcpy(S->CacheResult.GetData(),Ptr,Size);S->CacheReadback->Unlock(); }
                            S->CacheReadback.Reset();
                        }
                        if(S->StatsReadback)
                        {
                            if(const auto* Ptr=static_cast<const uint32*>(S->StatsReadback->Lock(8)))
                            { S->ReusedQueries=Ptr[0];S->RebuiltQueries=Ptr[1];S->StatsReadback->Unlock(); }
                            S->StatsReadback.Reset();
                        }
                        S->Readback.Reset();S->Ready.Store(true);
                    }
                    S->PollQueued.Store(false);
                });
            }
            return false;
        }
        S->Pending=false;
        if(S->CachePhase)
        {
            S->CacheOffset+=S->SubmittedSurfels;
            if(S->CacheOffset>=S->Scene->SurfelSnapshot->ActiveSlots.Num())
            {
                S->CacheOffset=0;S->Current=1-S->Current;++S->Iteration;
                if(S->Iteration>=S->Scene->CacheIterations)S->CachePhase=false;
            }
        }
        else
        {
            S->Offset+=S->Budget;
            if(S->Offset>=S->Scene->Probes.Num()) { Output=MoveTemp(S->Result);return true; }
        }
    }
    if(!bAllowSubmission)return false;
    S->SubmittedSurfels=S->CachePhase?FMath::Min(S->Scene->CacheBatch*FMath::Clamp(S->Scene->CacheBatchesPerSubmit,1,32),
        S->Scene->SurfelSnapshot->ActiveSlots.Num()-S->CacheOffset):0;
    ++S->Submissions;S->Pending=true;S->Ready.Store(false);
    ENQUEUE_RENDER_COMMAND(PRTEvaluateRuntime)([S](FRHICommandListImmediate& RHICmdList)
    {
        FRDGBuilder Graph(RHICmdList);const auto& Scene=*S->Scene;
        if(!S->Triangles)
        {
            auto Upload=[&](const TCHAR* Name,const TArray<FVector4f>& Values,TRefCountPtr<FRDGPooledBuffer>& Buffer)
            {
                const TArray<FVector4f> Empty{FVector4f(0)};
                Buffer=Graph.ConvertToExternalBuffer(CreateStructuredBuffer(Graph,Name,Values.IsEmpty()?Empty:Values));
            };
            Upload(TEXT("PRT.Runtime.Triangles"),Scene.Geometry->Triangles,S->Triangles);
            Upload(TEXT("PRT.Runtime.Nodes"),Scene.Geometry->Nodes,S->Nodes);
            Upload(TEXT("PRT.Runtime.Materials"),Scene.Materials,S->Materials);
            Upload(TEXT("PRT.Runtime.Lights"),Scene.Lights,S->Lights);
            Upload(TEXT("PRT.Runtime.Probes"),Scene.Probes,S->Probes);
            Upload(TEXT("PRT.Cache.Bindings"),Scene.SurfelBindings,S->Bindings);
            const TArray<uint32> EmptySlots{0};
            S->Active=Graph.ConvertToExternalBuffer(CreateStructuredBuffer(Graph,TEXT("PRT.Cache.Active"),
                Scene.SurfelSnapshot && !Scene.SurfelSnapshot->ActiveSlots.IsEmpty()?Scene.SurfelSnapshot->ActiveSlots:EmptySlots));
            for(int32 I=0;I<2;++I)
            {
                auto B=Graph.CreateBuffer(FRDGBufferDesc::CreateStructuredDesc(16,Scene.SurfelSnapshot?Scene.SurfelSnapshot->Capacity:1),TEXT("PRT.Cache.Irradiance"));
                AddClearUAVPass(Graph,Graph.CreateUAV(B),0);S->Irradiance[I]=Graph.ConvertToExternalBuffer(B);
            }
            const TArray<uint32> NoRemap{MAX_uint32};
            S->TriangleRemap=Graph.ConvertToExternalBuffer(CreateStructuredBuffer(Graph,TEXT("PRT.Cache.TriangleRemap"),Scene.OldToNewTriangles.IsEmpty()?NoRemap:Scene.OldToNewTriangles));
            auto Stats=Graph.CreateBuffer(FRDGBufferDesc::CreateStructuredDesc(4,2),TEXT("PRT.Cache.QueryStats"));
            AddClearUAVPass(Graph,Graph.CreateUAV(Stats),0);S->QueryStats=Graph.ConvertToExternalBuffer(Stats);
            auto SH=Graph.CreateBuffer(FRDGBufferDesc::CreateStructuredDesc(16,Scene.Probes.Num()*9),TEXT("PRT.Runtime.SH"));
            // Every active/ray row is initialized in sweep zero, including misses
            // and rejected backfaces. Eligible regional history supplies rows
            // only after the first-sweep dependency checks.
            S->TransportQueries=Graph.ConvertToExternalBuffer(Graph.CreateBuffer(
                FRDGBufferDesc::CreateStructuredDesc(16,S->ReuseTransport?uint32(S->TransportBytes/16):1),TEXT("PRT.Cache.TransportQueries")));
            AddClearUAVPass(Graph,Graph.CreateUAV(SH),0);
            S->SH=Graph.ConvertToExternalBuffer(SH);
        }
        auto* P=Graph.AllocParameters<FPRTRuntimeCS::FParameters>();
        P->Triangles=Graph.CreateSRV(Graph.RegisterExternalBuffer(S->Triangles));
        P->Nodes=Graph.CreateSRV(Graph.RegisterExternalBuffer(S->Nodes));
        P->Materials=Graph.CreateSRV(Graph.RegisterExternalBuffer(S->Materials));
        P->Lights=Graph.CreateSRV(Graph.RegisterExternalBuffer(S->Lights));
        P->Probes=Graph.CreateSRV(Graph.RegisterExternalBuffer(S->Probes));
        auto SH=Graph.RegisterExternalBuffer(S->SH);P->OutSH=Graph.CreateUAV(SH);
        P->NodeCount=Scene.Geometry->Nodes.Num()/2;P->LightCount=Scene.Lights.Num()/2;
        P->ProbeOffset=S->Offset;P->ProbeCount=Scene.Probes.Num();P->RayCount=Scene.Rays;P->BounceCount=Scene.Bounces;
        P->SurfacePool=Graph.CreateSRV(Graph.RegisterExternalBuffer(Scene.SurfelSnapshot?Scene.SurfelSnapshot->Buffer:S->Probes));
        P->ActiveSlots=Graph.CreateSRV(Graph.RegisterExternalBuffer(S->Active));
        P->SurfelBindings=Graph.CreateSRV(Graph.RegisterExternalBuffer(S->Bindings));
        auto Previous=Graph.RegisterExternalBuffer(S->Irradiance[S->Current]);
        auto Next=Graph.RegisterExternalBuffer(S->Irradiance[1-S->Current]);
        P->PreviousIrradiance=Graph.CreateSRV(Previous);P->NextIrradiance=Graph.CreateUAV(Next);
        P->CacheEnabled=Scene.SurfelSnapshot?1:0;P->CacheOffset=S->CacheOffset;
        P->CacheActiveCount=Scene.SurfelSnapshot?Scene.SurfelSnapshot->ActiveSlots.Num():0;
        P->CacheRayCount=Scene.CacheRays;P->CacheIteration=S->Iteration;P->CacheRelaxation=Scene.CacheRelaxation;
        P->AnalyticEnvironment=Scene.AnalyticEnvironment;
        P->TransportQueries=Graph.CreateUAV(Graph.RegisterExternalBuffer(S->TransportQueries));
        P->ReuseTransport=S->ReuseTransport;P->QueryStride=S->QueryStride;P->RegionalReuse=S->RegionalReuse;
        P->HasChangedBounds=Scene.ChangedBounds.IsValid;
        P->ChangedMin=Scene.ChangedBounds.IsValid?Scene.ChangedBounds.Min:FVector3f(0);
        P->ChangedMax=Scene.ChangedBounds.IsValid?Scene.ChangedBounds.Max:FVector3f(0);
        P->OldQueries=Graph.CreateSRV(Graph.RegisterExternalBuffer(S->History?S->History->Queries:S->Probes));
        P->OldSurfacePool=Graph.CreateSRV(Graph.RegisterExternalBuffer(S->History?S->History->Surface->Buffer:S->Probes));
        P->OldToNewTriangles=Graph.CreateSRV(Graph.RegisterExternalBuffer(S->TriangleRemap));
        P->QueryStats=Graph.CreateUAV(Graph.RegisterExternalBuffer(S->QueryStats));
        const bool Last=!S->CachePhase && S->Offset+S->Budget>=Scene.Probes.Num();
        if(S->CachePhase)
        {
            TShaderMapRef<FPRTCacheCS> Shader(GetGlobalShaderMap(GMaxRHIShaderPlatform));
            // Do not cross a Jacobi sweep boundary in a submission. Every
            // dispatch reads the same previous buffer and writes disjoint slots.
            for(int32 Done=0;Done<S->SubmittedSurfels;Done+=Scene.CacheBatch)
            {
                auto* Batch=Graph.AllocParameters<FPRTCacheCS::FParameters>();*Batch=*P;
                Batch->CacheOffset=S->CacheOffset+Done;
                FComputeShaderUtils::AddPass(Graph,RDG_EVENT_NAME("PRT Surfel Irradiance Sweep Reuse=%d Build=%d",S->ReuseTransport,S->ReuseTransport && S->Iteration==0),Shader,Batch,
                    FIntVector(FMath::Min(Scene.CacheBatch,S->SubmittedSurfels-Done),1,1));
            }
        }
        else
        {
        TShaderMapRef<FPRTRuntimeCS> Shader(GetGlobalShaderMap(GMaxRHIShaderPlatform));
        FComputeShaderUtils::AddPass(Graph,RDG_EVENT_NAME("PRT Runtime Probe Transport Cache=%d",P->CacheEnabled),Shader,P,
            FIntVector(FMath::Min(S->Budget,Scene.Probes.Num()-S->Offset),1,1));
        }
        if(Last)
        {
            S->Readback=MakeUnique<FRHIGPUBufferReadback>(TEXT("PRT.Runtime.Readback"));
            AddEnqueueCopyPass(Graph,S->Readback.Get(),SH,Scene.Probes.Num()*9*16);
            if(Scene.RegionalTransport)
            { S->StatsReadback=MakeUnique<FRHIGPUBufferReadback>(TEXT("PRT.Cache.QueryStatsReadback"));AddEnqueueCopyPass(Graph,S->StatsReadback.Get(),Graph.RegisterExternalBuffer(S->QueryStats),8); }
        }
        if(Last && Scene.SurfelSnapshot && Scene.ReadCacheForTest)
        {
            S->CacheReadback=MakeUnique<FRHIGPUBufferReadback>(TEXT("PRT.Cache.TestReadback"));
            AddEnqueueCopyPass(Graph,S->CacheReadback.Get(),Previous,Scene.SurfelSnapshot->Capacity*16);
        }
        Graph.Execute();
        if(!Last)
        {
            S->Fence=RHICreateGPUFence(TEXT("PRT.Runtime.SubmissionComplete"));
            RHICmdList.WriteGPUFence(S->Fence);
        }
    });
    return false;
}
}
