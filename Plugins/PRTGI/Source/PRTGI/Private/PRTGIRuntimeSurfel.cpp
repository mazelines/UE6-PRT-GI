#include "PRTGIRuntimeSurfel.h"
#include "GlobalShader.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "RenderingThread.h"
#include "ShaderParameterStruct.h"
#include "RHIGPUReadback.h"

namespace
{
BEGIN_SHADER_PARAMETER_STRUCT(FSurfelParameters,)
    SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>,Seeds)
    SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<float4>,Pool)
    SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint4>,CellKeys)
    SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>,Counts)
    SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>,Offsets)
    SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>,BlockSums)
    SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>,BlockOffsets)
    SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>,Cursors)
    SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>,Slots)
    SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>,Queries)
    SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint2>,QueryResults)
    SHADER_PARAMETER(uint32,Capacity)
    SHADER_PARAMETER(uint32,BucketCount)
    SHADER_PARAMETER(uint32,SeedCount)
    SHADER_PARAMETER(uint32,SlotOffset)
    SHADER_PARAMETER(uint32,Owner)
    SHADER_PARAMETER(uint32,Revision)
    SHADER_PARAMETER(uint32,ClearCount)
    SHADER_PARAMETER(uint32,QueryCount)
    SHADER_PARAMETER(float,BaseCellCm)
    SHADER_PARAMETER(FVector4f,TransformX)
    SHADER_PARAMETER(FVector4f,TransformY)
    SHADER_PARAMETER(FVector4f,TransformZ)
    SHADER_PARAMETER(FVector4f,NormalX)
    SHADER_PARAMETER(FVector4f,NormalY)
    SHADER_PARAMETER(FVector4f,NormalZ)
END_SHADER_PARAMETER_STRUCT()
#define SURFEL_SHADER(Name,Entry) \
class Name : public FGlobalShader { \
    DECLARE_GLOBAL_SHADER(Name); SHADER_USE_PARAMETER_STRUCT(Name,FGlobalShader); \
    using FParameters=FSurfelParameters; \
    static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& P) \
    { return IsFeatureLevelSupported(P.Platform,ERHIFeatureLevel::SM6); } }; \
IMPLEMENT_GLOBAL_SHADER(Name,"/Plugin/PRTGI/Private/PRTGIRuntimeSurfel.usf",Entry,SF_Compute);
SURFEL_SHADER(FSurfelTransformCS,"TransformCS")
SURFEL_SHADER(FSurfelClearCS,"ClearSlotsCS")
SURFEL_SHADER(FSurfelCountCS,"CountCS")
SURFEL_SHADER(FSurfelScanCS,"ScanCS")
SURFEL_SHADER(FSurfelScanBlocksCS,"ScanBlocksCS")
SURFEL_SHADER(FSurfelAddBlocksCS,"AddBlocksCS")
SURFEL_SHADER(FSurfelScatterCS,"ScatterCS")
SURFEL_SHADER(FSurfelQueryCS,"QueryCS")
#undef SURFEL_SHADER

struct FRange { uint32 Start=0,Count=0; };
struct FOwnerRange : FRange
{
    PRTGI::FRuntimeSurfelInstance Instance;
    uint32 Epoch=0;
};
struct FAssetInfo { FBox Bounds{ForceInit};float Radius=0; };
struct FPoolRenderState
{
    TRefCountPtr<FRDGPooledBuffer> Pool,Keys,Offsets,Slots;
    TMap<TSharedPtr<const PRTGI::FRuntimeSurfelAsset,ESPMode::ThreadSafe>,TRefCountPtr<FRDGPooledBuffer>> Assets;
};
struct FAudit
{
    TUniquePtr<FRHIGPUBufferReadback> Reads[5];
    TArray<uint8> Bytes[5];
    uint32 Sizes[5]{};
    TArray<FVector4f> Expected,Queries;
    TAtomic<bool> Ready{false},Polling{false};
    uint32 Revision=0;
    bool Failed=false;
};
template<class Shader> void Dispatch(FRDGBuilder& Graph,const TCHAR* Name,const FSurfelParameters& In,uint32 Groups)
{
    auto* P=Graph.AllocParameters<FSurfelParameters>();*P=In;
    TShaderMapRef<Shader> S(GetGlobalShaderMap(GMaxRHIShaderPlatform));
    FComputeShaderUtils::AddPass(Graph,FRDGEventName(Name),S,P,FIntVector(Groups,1,1));
}
void Rows(const FMatrix& M,FVector4f& X,FVector4f& Y,FVector4f& Z)
{
    X=FVector4f(M.M[0][0],M.M[1][0],M.M[2][0],M.M[3][0]);
    Y=FVector4f(M.M[0][1],M.M[1][1],M.M[2][1],M.M[3][1]);
    Z=FVector4f(M.M[0][2],M.M[1][2],M.M[2][2],M.M[3][2]);
}
uint32 HashCell(const FIntVector& C,uint32 Level,uint32 Buckets)
{
    uint32 H=uint32(C.X)*73856093u ^ uint32(C.Y)*19349663u ^ uint32(C.Z)*83492791u ^ Level*2654435761u;
    H^=H>>16;return H&(Buckets-1);
}
void MergeFree(TArray<FRange>& Free)
{
    Free.Sort([](const FRange& A,const FRange& B){return A.Start<B.Start;});
    for(int32 I=Free.Num()-1;I>0;--I)if(Free[I-1].Start+Free[I-1].Count==Free[I].Start)
    { Free[I-1].Count+=Free[I].Count;Free.RemoveAt(I); }
}
}
namespace PRTGI
{
struct FRuntimeSurfelPool::FImpl
{
    uint32 Capacity,Buckets,Epoch=0,Live=0,Updated=0,Removed=0;
    float BaseCell;
    TMap<uint32,FOwnerRange> Owners;
    TMap<TSharedPtr<const FRuntimeSurfelAsset,ESPMode::ThreadSafe>,FAssetInfo> AssetInfo;
    TArray<FRange> Free;
    TSharedPtr<FPoolRenderState,ESPMode::ThreadSafe> Render=MakeShared<FPoolRenderState,ESPMode::ThreadSafe>();
    TSharedPtr<FAudit,ESPMode::ThreadSafe> Audit;
};
FRuntimeSurfelPool::FRuntimeSurfelPool(uint32 Capacity,uint32 Buckets,float BaseCellCm):Impl(MakeUnique<FImpl>())
{
    check(Capacity>0 && Capacity<=1048576 && Buckets>=256 && Buckets<=65536 && FMath::IsPowerOfTwo(Buckets) && BaseCellCm>0);
    Impl->Capacity=Capacity;Impl->Buckets=Buckets;Impl->BaseCell=BaseCellCm;Impl->Free.Add({0,Capacity});
}
FRuntimeSurfelPool::~FRuntimeSurfelPool()
{
    auto S=MoveTemp(Impl->Render);auto A=MoveTemp(Impl->Audit);
    ENQUEUE_RENDER_COMMAND(PRTReleaseSurfelPool)([S,A](FRHICommandListImmediate&){});
}
uint32 FRuntimeSurfelPool::Revision()const { return Impl->Epoch; }
bool FRuntimeSurfelPool::Update(TConstArrayView<FRuntimeSurfelInstance> Items,FString& Error)
{
    auto& S=*Impl;Error.Reset();TSet<uint32> Seen;
    if(S.Epoch>=16777215) { Error=TEXT("Surfel revision exhausted; reset the pool");return false; }
    // Validate the whole transaction before changing ownership or GPU state.
    for(const auto& I:Items)
    {
        if(I.Owner==0 || I.Owner>16777215 || Seen.Contains(I.Owner) || !I.Asset || I.Asset->Seeds.IsEmpty() || I.Asset->Seeds.Num()%6)
        { Error=TEXT("Invalid/duplicate owner or malformed Surfel asset");return false; }
        Seen.Add(I.Owner);
        if(!I.Transform.IsValid() || I.Transform.GetScale3D().GetAbsMin()<1.e-6)
        { Error=TEXT("Nonfinite or singular Surfel instance transform");return false; }
        auto* Info=S.AssetInfo.Find(I.Asset);
        if(!Info)
        {
            FAssetInfo New;
            for(int32 K=0;K<I.Asset->Num();++K)
            {
                const auto* P=I.Asset->Seeds.GetData()+K*6;
                for(int32 C=0;C<6;++C)if(P[C].ContainsNaN()) { Error=TEXT("Nonfinite Surfel seed");return false; }
                if(FVector3f(P[1]).SizeSquared()<1.e-8 || FVector3f(P[5]).SizeSquared()<1.e-8)
                { Error=TEXT("Zero Surfel normal");return false; }
                const float R=FMath::Max3(FVector3f(P[2]).Size(),FVector3f(P[3]).Size(),(FVector3f(P[2])+FVector3f(P[3])).Size());
                if(R<1.e-6) { Error=TEXT("Zero Surfel radius");return false; }
                New.Radius=FMath::Max(New.Radius,R);New.Bounds+=FVector(FVector3f(P[0]));
            }
            Info=&S.AssetInfo.Add(I.Asset,New);
        }
        const auto Bounds=Info->Bounds.TransformBy(I.Transform.ToMatrixWithScale());
        if(Info->Radius*I.Transform.GetScale3D().GetAbsMax()>S.BaseCell*16384 || Bounds.Min.GetAbsMax()>2.e7 || Bounds.Max.GetAbsMax()>2.e7)
        { Error=TEXT("Surfel exceeds the 16-level grid radius or PC float-position bounds");return false; }
    }
    auto Next=S.Owners;auto Free=S.Free;TArray<FRange> Clear;TArray<FOwnerRange> Dirty;
    for(auto It=Next.CreateIterator();It;++It)
    {
        const auto* Item=Items.FindByPredicate([&](const auto& I){return I.Owner==It.Key();});
        if(!Item || Item->Asset!=It.Value().Instance.Asset)
        { Clear.Add({It.Value().Start,It.Value().Count});Free.Add(Clear.Last());It.RemoveCurrent(); }
    }
    MergeFree(Free);
    for(const auto& I:Items)
    {
        auto* Existing=Next.Find(I.Owner);
        if(Existing && Existing->Instance.Transform.Equals(I.Transform,0))continue;
        FOwnerRange Range;
        if(Existing)Range=*Existing;
        else
        {
            const int32 F=Free.IndexOfByPredicate([&](const FRange& R){return R.Count>=uint32(I.Asset->Num());});
            if(F==INDEX_NONE) { Error=TEXT("Surfel pool capacity/contiguous allocation exhausted; existing pool unchanged");return false; }
            Range.Start=Free[F].Start;Range.Count=I.Asset->Num();Free[F].Start+=Range.Count;Free[F].Count-=Range.Count;
            if(!Free[F].Count)Free.RemoveAt(F);
        }
        Range.Instance=I;Range.Epoch=S.Epoch+1;Next.Add(I.Owner,Range);Dirty.Add(Range);
    }
    if(Dirty.IsEmpty() && Clear.IsEmpty() && S.Epoch)return true;
    S.Owners=MoveTemp(Next);S.Free=MoveTemp(Free);S.Updated=Dirty.Num();S.Removed=Clear.Num();++S.Epoch;S.Live=0;
    for(const auto& Pair:S.Owners)S.Live+=Pair.Value.Count;
    auto Render=S.Render;const uint32 Cap=S.Capacity,Buckets=S.Buckets;const float Base=S.BaseCell;
    TSet<TSharedPtr<const FRuntimeSurfelAsset,ESPMode::ThreadSafe>> Used;
    for(const auto& I:Items)Used.Add(I.Asset);
    for(auto It=S.AssetInfo.CreateIterator();It;++It)if(!Used.Contains(It.Key()))It.RemoveCurrent();
    ENQUEUE_RENDER_COMMAND(PRTUpdateSurfelPool)([Render,Dirty=MoveTemp(Dirty),Clear=MoveTemp(Clear),Used=MoveTemp(Used),Cap,Buckets,Base](FRHICommandListImmediate& RHICmdList)
    {
        FRDGBuilder Graph(RHICmdList);
        auto Make=[&](const TCHAR* Name,uint32 Stride,uint32 Num,TRefCountPtr<FRDGPooledBuffer>& Out,bool Zero)
        {
            if(Out)return Graph.RegisterExternalBuffer(Out);
            auto B=Graph.CreateBuffer(FRDGBufferDesc::CreateStructuredDesc(Stride,Num),Name);
            if(Zero)AddClearUAVPass(Graph,Graph.CreateUAV(B),0);Out=Graph.ConvertToExternalBuffer(B);return B;
        };
        auto Pool=Make(TEXT("PRT.Surfels.Pool"),16,Cap*4,Render->Pool,true);
        auto Keys=Make(TEXT("PRT.Surfels.Keys"),16,Cap,Render->Keys,false);
        auto Offsets=Make(TEXT("PRT.Surfels.Offsets"),4,Buckets+1,Render->Offsets,false);
        auto Slots=Make(TEXT("PRT.Surfels.Slots"),4,Cap,Render->Slots,false);
        FSurfelParameters P{};P.Pool=Graph.CreateUAV(Pool);P.CellKeys=Graph.CreateUAV(Keys);
        P.Offsets=Graph.CreateUAV(Offsets);P.Slots=Graph.CreateUAV(Slots);P.Capacity=Cap;P.BucketCount=Buckets;P.BaseCellCm=Base;
        for(const auto& R:Clear)
        { P.SlotOffset=R.Start;P.ClearCount=R.Count;Dispatch<FSurfelClearCS>(Graph,TEXT("PRT Surfel Retire"),P,FMath::DivideAndRoundUp(R.Count,64u)); }
        for(const auto& R:Dirty)
        {
            auto& Asset=Render->Assets.FindOrAdd(R.Instance.Asset);
            if(!Asset)Asset=Graph.ConvertToExternalBuffer(CreateStructuredBuffer(Graph,TEXT("PRT.Surfels.LocalSeeds"),R.Instance.Asset->Seeds));
            P.Seeds=Graph.CreateSRV(Graph.RegisterExternalBuffer(Asset));P.SeedCount=R.Count;P.SlotOffset=R.Start;P.Owner=R.Instance.Owner;P.Revision=R.Epoch;
            Rows(R.Instance.Transform.ToMatrixWithScale(),P.TransformX,P.TransformY,P.TransformZ);
            Rows(R.Instance.Transform.ToMatrixWithScale().Inverse().GetTransposed(),P.NormalX,P.NormalY,P.NormalZ);
            Dispatch<FSurfelTransformCS>(Graph,TEXT("PRT Surfel Transform Instance"),P,FMath::DivideAndRoundUp(R.Count,64u));
        }
        for(auto It=Render->Assets.CreateIterator();It;++It)if(!Used.Contains(It.Key()))It.RemoveCurrent();
        auto Temp=[&](const TCHAR* Name,uint32 Num){return Graph.CreateBuffer(FRDGBufferDesc::CreateStructuredDesc(4,Num),Name);};
        auto Counts=Temp(TEXT("PRT.Surfels.Counts"),Buckets);AddClearUAVPass(Graph,Graph.CreateUAV(Counts),0);
        P.Counts=Graph.CreateUAV(Counts);P.Cursors=Graph.CreateUAV(Temp(TEXT("PRT.Surfels.Cursors"),Buckets));
        P.BlockSums=Graph.CreateUAV(Temp(TEXT("PRT.Surfels.BlockSums"),Buckets/256));
        P.BlockOffsets=Graph.CreateUAV(Temp(TEXT("PRT.Surfels.BlockOffsets"),Buckets/256));
        Dispatch<FSurfelCountCS>(Graph,TEXT("PRT Surfel Grid Count"),P,FMath::DivideAndRoundUp(Cap,64u));
        Dispatch<FSurfelScanCS>(Graph,TEXT("PRT Surfel Grid Scan"),P,Buckets/256);
        Dispatch<FSurfelScanBlocksCS>(Graph,TEXT("PRT Surfel Grid Scan Blocks"),P,1);
        Dispatch<FSurfelAddBlocksCS>(Graph,TEXT("PRT Surfel Grid Add Blocks"),P,Buckets/256);
        Dispatch<FSurfelScatterCS>(Graph,TEXT("PRT Surfel Grid Scatter"),P,FMath::DivideAndRoundUp(Cap,64u));
        Graph.Execute();
    });
    return true;
}
FRuntimeSurfelSnapshot::~FRuntimeSurfelSnapshot()
{
    auto B=MoveTemp(Buffer);
    ENQUEUE_RENDER_COMMAND(PRTReleaseSurfelSnapshot)([B](FRHICommandListImmediate&){});
}
FVector4f FRuntimeSurfelPool::TriangleBinding(uint32 Owner,int32 Triangle)const
{
    const auto* R=Impl->Owners.Find(Owner);
    if(!R || !R->Instance.Asset->TriangleRanges.IsValidIndex(Triangle))return FVector4f(0);
    const auto Range=R->Instance.Asset->TriangleRanges[Triangle];
    return FVector4f(float(R->Start+Range.X),float(Range.Y),float(Range.Y*Range.Y),float(Owner));
}
TSharedPtr<FRuntimeSurfelSnapshot,ESPMode::ThreadSafe> FRuntimeSurfelPool::CaptureLightingSnapshot()const
{
    auto Out=MakeShared<FRuntimeSurfelSnapshot,ESPMode::ThreadSafe>();Out->Capacity=Impl->Capacity;
    for(const auto& Pair:Impl->Owners)for(uint32 I=0;I<Pair.Value.Count;++I)Out->ActiveSlots.Add(Pair.Value.Start+I);
    Out->ActiveSlots.Sort();auto Render=Impl->Render;
    ENQUEUE_RENDER_COMMAND(PRTCopySurfelSnapshot)([Out,Render](FRHICommandListImmediate& RHICmdList)
    {
        FRDGBuilder Graph(RHICmdList);auto Source=Graph.RegisterExternalBuffer(Render->Pool);
        auto Copy=Graph.CreateBuffer(Source->Desc,TEXT("PRT.Cache.SurfaceSnapshot"));
        AddCopyBufferPass(Graph,Copy,Source);Out->Buffer=Graph.ConvertToExternalBuffer(Copy);Graph.Execute();
    });
    return Out;
}
FString FRuntimeSurfelPool::Status()const
{
    const auto& S=*Impl;
    return FString::Printf(TEXT("SurfelRevision=%u SurfelOwners=%d SurfelLive=%u SurfelCapacity=%u SurfelUpdated=%u SurfelRetired=%u GridBuckets=%u AuditPending=%d"),
        S.Epoch,S.Owners.Num(),S.Live,S.Capacity,S.Updated,S.Removed,S.Buckets,S.Audit.IsValid());
}
bool FRuntimeSurfelPool::RequestAudit(FString& Error)
{
    auto& S=*Impl;Error.Reset();
    if(!S.Epoch || S.Audit) { Error=TEXT("No submitted pool or audit already pending");return false; }
    auto A=MakeShared<FAudit,ESPMode::ThreadSafe>();A->Revision=S.Epoch;A->Expected.SetNumZeroed(S.Capacity*4);
    for(const auto& Pair:S.Owners)
    {
        const auto& R=Pair.Value;const auto& T=R.Instance.Transform;const FMatrix N=T.ToMatrixWithScale().Inverse().GetTransposed();
        for(uint32 I=0;I<R.Count;++I)
        {
            const auto* L=R.Instance.Asset->Seeds.GetData()+I*6;auto* P=A->Expected.GetData()+(R.Start+I)*4;
            const FVector X=T.TransformVector(FVector(FVector3f(L[2]))),Y=T.TransformVector(FVector(FVector3f(L[3])));
            P[0]=FVector4f(FVector3f(T.TransformPosition(FVector(FVector3f(L[0])))),float(FMath::Max3(X.Length(),Y.Length(),(X+Y).Length())));
            P[1]=FVector4f(FVector3f(FVector(N.TransformVector(FVector(FVector3f(L[1])))).GetSafeNormal()),float(Pair.Key));
            P[2]=FVector4f(FVector3f(L[4]),float(I));
            P[3]=FVector4f(FVector3f(FVector(N.TransformVector(FVector(FVector3f(L[5])))).GetSafeNormal()),float(R.Epoch));
            if(A->Queries.Num()<64)A->Queries.Add(P[0]+FVector4f(.137f,-.271f,.419f,0));
        }
    }
    A->Queries.Append({FVector4f(0),FVector4f(-31.9f,-32.1f,-.1f,0),FVector4f(12345,-12345,10000,0)});
    S.Audit=A;auto Render=S.Render;const uint32 Cap=S.Capacity,Buckets=S.Buckets;const float Base=S.BaseCell;
    ENQUEUE_RENDER_COMMAND(PRTAuditSurfelPool)([A,Render,Cap,Buckets,Base](FRHICommandListImmediate& RHICmdList)
    {
        FRDGBuilder Graph(RHICmdList);FRDGBufferRef Buffers[5]={Graph.RegisterExternalBuffer(Render->Pool),Graph.RegisterExternalBuffer(Render->Keys),
            Graph.RegisterExternalBuffer(Render->Offsets),Graph.RegisterExternalBuffer(Render->Slots),nullptr};
        auto Query=CreateStructuredBuffer(Graph,TEXT("PRT.Surfels.AuditQueries"),A->Queries);
        Buffers[4]=Graph.CreateBuffer(FRDGBufferDesc::CreateStructuredDesc(8,A->Queries.Num()),TEXT("PRT.Surfels.AuditResults"));
        FSurfelParameters P{};P.Pool=Graph.CreateUAV(Buffers[0]);P.CellKeys=Graph.CreateUAV(Buffers[1]);P.Offsets=Graph.CreateUAV(Buffers[2]);P.Slots=Graph.CreateUAV(Buffers[3]);
        P.Queries=Graph.CreateSRV(Query);P.QueryResults=Graph.CreateUAV(Buffers[4]);P.QueryCount=A->Queries.Num();P.BucketCount=Buckets;P.BaseCellCm=Base;
        Dispatch<FSurfelQueryCS>(Graph,TEXT("PRT Surfel Audit Queries"),P,FMath::DivideAndRoundUp(P.QueryCount,64u));
        for(int32 I=0;I<5;++I)
        { A->Sizes[I]=Buffers[I]->Desc.GetSize();A->Reads[I]=MakeUnique<FRHIGPUBufferReadback>(TEXT("PRT.Surfels.Audit"));AddEnqueueCopyPass(Graph,A->Reads[I].Get(),Buffers[I],A->Sizes[I]); }
        Graph.Execute();
    });
    return true;
}
bool FRuntimeSurfelPool::PollAudit(FString& Result,bool& Passed)
{
    auto& S=*Impl;auto A=S.Audit;if(!A)return false;
    if(!A->Ready.Load())
    {
        if(!A->Polling.Exchange(true))
        {
        ENQUEUE_RENDER_COMMAND(PRTPollSurfelAudit)([A](FRHICommandListImmediate&)
        {
            bool Ready=true;for(const auto& R:A->Reads)Ready&=R && R->IsReady();
            if(Ready)
            {
                for(int32 I=0;I<5;++I)
                {
                    if(const void* Data=A->Reads[I]->Lock(A->Sizes[I]))
                    { A->Bytes[I].Append(static_cast<const uint8*>(Data),A->Sizes[I]);A->Reads[I]->Unlock(); }
                    else A->Failed=true;
                    A->Reads[I].Reset();
                }
                A->Ready.Store(true);
            }
            A->Polling.Store(false);
        });
        }
        return false;
    }
    uint32 Failures=A->Failed?1:0,Live=0,MaxBucket=0;
    if(!Failures)
    {
        const auto* Pool=reinterpret_cast<const FVector4f*>(A->Bytes[0].GetData());
        const auto* Keys=reinterpret_cast<const uint32*>(A->Bytes[1].GetData());
        const auto* Offsets=reinterpret_cast<const uint32*>(A->Bytes[2].GetData());
        const auto* Slots=reinterpret_cast<const uint32*>(A->Bytes[3].GetData());
        const auto* Queries=reinterpret_cast<const uint32*>(A->Bytes[4].GetData());
        TBitArray<> Found(false,S.Capacity);
        for(uint32 I=0;I<S.Capacity*4;++I)for(uint32 C=0;C<4;++C)
            if(!FMath::IsFinite(Pool[I][C]) || FMath::Abs(Pool[I][C]-A->Expected[I][C])>.002f+FMath::Abs(A->Expected[I][C])*2.e-6f)++Failures;
        for(uint32 I=0;I<S.Capacity;++I)if(Pool[I*4+1].W>0)++Live;
        if(Offsets[0]!=0 || Offsets[S.Buckets]!=Live)++Failures;
        for(uint32 B=0;B<S.Buckets;++B)
        {
            const uint32 Start=Offsets[B],End=Offsets[B+1];
            if(Start>End || End>S.Capacity) { ++Failures;continue; }
            MaxBucket=FMath::Max(MaxBucket,End-Start);
            for(uint32 I=Start;I<End;++I)
            {
                const uint32 Slot=Slots[I];if(Slot>=S.Capacity) { ++Failures;continue; }
                if(Found[Slot] || Pool[Slot*4+1].W==0)++Failures;Found[Slot]=true;
                const auto* K=Keys+Slot*4;const auto P=Pool[Slot*4];
                const uint32 Level=uint32(FMath::Max(0.,FMath::CeilToDouble(FMath::Log2(FMath::Max(1.f,2*P.W/S.BaseCell)))));
                const float Width=S.BaseCell*float(1u<<Level);
                const FIntVector Cell(FMath::FloorToInt(P.X/Width),FMath::FloorToInt(P.Y/Width),FMath::FloorToInt(P.Z/Width));
                if(K[0]!=uint32(Cell.X) || K[1]!=uint32(Cell.Y) || K[2]!=uint32(Cell.Z) || K[3]!=Level || HashCell(Cell,Level,S.Buckets)!=B)++Failures;
            }
        }
        for(uint32 I=0;I<S.Capacity;++I)if(Found[I]!=(Pool[I*4+1].W>0))++Failures;
        for(int32 Q=0;Q<A->Queries.Num();++Q)
        {
            uint32 Count=0,Checksum=0;
            for(uint32 I=0;I<S.Capacity;++I)if(Pool[I*4+1].W>0 && FVector3f::Distance(FVector3f(A->Queries[Q]),FVector3f(Pool[I*4]))<=Pool[I*4].W)
            { ++Count;Checksum^=I+1; }
            if(Queries[Q*2]!=Count || Queries[Q*2+1]!=Checksum)++Failures;
        }
    }
    Passed=Failures==0;
    Result=FString::Printf(TEXT("SurfelAudit=%s Revision=%u CurrentRevision=%u Live=%u Queries=%d MaxBucket=%u Failures=%u"),
        Passed?TEXT("PASS"):TEXT("FAIL"),A->Revision,S.Epoch,Live,A->Queries.Num(),MaxBucket,Failures);
    S.Audit.Reset();return true;
}
}
