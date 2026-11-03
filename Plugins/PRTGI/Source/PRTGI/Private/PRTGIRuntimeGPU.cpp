#include "PRTGIRuntime.h"
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
    END_SHADER_PARAMETER_STRUCT()
};
IMPLEMENT_GLOBAL_SHADER(FPRTRuntimeCS,"/Plugin/PRTGI/Private/PRTGIRuntime.usf","RuntimeCS",SF_Compute);
}
namespace PRTGI
{
struct FRuntimeGPU::FState
{
    TSharedPtr<const FRuntimeScene,ESPMode::ThreadSafe> Scene;
    TRefCountPtr<FRDGPooledBuffer> Triangles,Nodes,Materials,Lights,Probes,SH;
    TUniquePtr<FRHIGPUBufferReadback> Readback;
    TAtomic<bool> Ready{false},PollQueued{false};
    TArray<FVector4f> Result;
    int32 Offset=0,Budget=32;
    bool Pending=false;
};
FRuntimeGPU::FRuntimeGPU():State(MakeShared<FState,ESPMode::ThreadSafe>()){}
FRuntimeGPU::~FRuntimeGPU()
{
    auto S=MoveTemp(State);
    ENQUEUE_RENDER_COMMAND(PRTReleaseRuntime)([S](FRHICommandListImmediate&)
    {
        S->Readback.Reset();S->Triangles.SafeRelease();S->Nodes.SafeRelease();
        S->Materials.SafeRelease();S->Lights.SafeRelease();S->Probes.SafeRelease();S->SH.SafeRelease();
    });
}
void FRuntimeGPU::Submit(TSharedPtr<const FRuntimeScene,ESPMode::ThreadSafe> Scene,int32 InProbeBudget)
{
    check(!State->Scene);State->Scene=Scene;State->Budget=FMath::Clamp(InProbeBudget,1,256);
}
bool FRuntimeGPU::Tick(TArray<FVector4f>& Output)
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
                    if(S->Readback && S->Readback->IsReady())
                    {
                        const bool Last=S->Offset+S->Budget>=S->Scene->Probes.Num();
                        const uint32 Bytes=Last?S->Scene->Probes.Num()*9*16:16;
                        if(const void* Ptr=S->Readback->Lock(Bytes))
                        {
                            if(Last) { S->Result.SetNumUninitialized(S->Scene->Probes.Num()*9);FMemory::Memcpy(S->Result.GetData(),Ptr,Bytes); }
                            S->Readback->Unlock();
                        }
                        S->Readback.Reset();S->Ready.Store(true);
                    }
                    S->PollQueued.Store(false);
                });
            }
            return false;
        }
        S->Pending=false;S->Offset+=S->Budget;
        if(S->Offset>=S->Scene->Probes.Num()) { Output=MoveTemp(S->Result);return true; }
    }
    S->Pending=true;S->Ready.Store(false);
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
            auto SH=Graph.CreateBuffer(FRDGBufferDesc::CreateStructuredDesc(16,Scene.Probes.Num()*9),TEXT("PRT.Runtime.SH"));
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
        TShaderMapRef<FPRTRuntimeCS> Shader(GetGlobalShaderMap(GMaxRHIShaderPlatform));
        FComputeShaderUtils::AddPass(Graph,RDG_EVENT_NAME("PRT Runtime Reference Transport"),Shader,P,
            FIntVector(FMath::Min(S->Budget,Scene.Probes.Num()-S->Offset),1,1));
        S->Readback=MakeUnique<FRHIGPUBufferReadback>(TEXT("PRT.Runtime.Readback"));
        // A tiny readback also bounds queued GPU work to one batch. Only the
        // last batch reads all coefficients (reference validation / CPU resolve).
        AddEnqueueCopyPass(Graph,S->Readback.Get(),SH,S->Offset+S->Budget>=Scene.Probes.Num()?Scene.Probes.Num()*9*16:16);
        Graph.Execute();
    });
    return false;
}
}
