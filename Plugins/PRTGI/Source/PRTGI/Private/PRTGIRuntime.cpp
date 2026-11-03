#include "PRTGIRuntime.h"
#include "PRTGIRuntimeSurfel.h"
#include "PRTGIMultiBounce.h"
#include "PRTGIMaterialAtlas.h"
#include "PRTProbeVolume.h"
#include "Components/StaticMeshComponent.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/DirectionalLightComponent.h"
#include "Components/PointLightComponent.h"
#include "Components/SpotLightComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Materials/MaterialInterface.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "StaticMeshResources.h"
#include "StaticMeshComponentLODInfo.h"
#include "HAL/IConsoleManager.h"
#include "Misc/Crc.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
#include "UObject/ObjectKey.h"
#include "Elements/SMInstance/SMInstanceElementId.h"

DEFINE_LOG_CATEGORY_STATIC(LogPRTRuntime,Log,All);
namespace PRTGI
{
namespace
{
TAutoConsoleVariable<int32> Enable(TEXT("r.PRTGI.Runtime"),0,TEXT("Experimental PC live-transport reference. Prepare assets first with PRTGI.Runtime.Prepare."));
TAutoConsoleVariable<int32> Rays(TEXT("r.PRTGI.Runtime.Rays"),256,TEXT("Deterministic rays per probe, 64..1024. Reference quality, not a frame-time guarantee."));
TAutoConsoleVariable<int32> Bounces(TEXT("r.PRTGI.Runtime.Bounces"),6,TEXT("Maximum runtime diffuse path bounces, 1..12."));
TAutoConsoleVariable<int32> Budget(TEXT("r.PRTGI.Runtime.ProbesPerBatch"),32,TEXT("1..256 probes per GPU batch; at most one batch in flight."));
TAutoConsoleVariable<int32> SurfelPoolEnabled(TEXT("r.PRTGI.Runtime.SurfelPool"),0,TEXT("Stage-one GPU surface pool and spatial index alongside the reference lighting; does not replace GI."));
TAutoConsoleVariable<float> SeedSpacing(TEXT("r.PRTGI.Runtime.SurfelSeedSpacingCm"),50.f,TEXT("Asset-local triangle subdivision edge target in cm; run Runtime.Prepare after changing. Instance scale changes world density."));
TAutoConsoleVariable<int32> UseSurfelCache(TEXT("r.PRTGI.Runtime.SurfelCache"),0,TEXT("Opt-in iterative GPU irradiance cache; automatically enables surface pool. Atomic publication after cache sweeps and probe projection."));
TAutoConsoleVariable<int32> CacheIterations(TEXT("r.PRTGI.Runtime.CacheIterations"),5,TEXT("1..32 Jacobi sweeps, default 5 plus final probe hit: up to six reflected surfaces at relaxation 1."));
TAutoConsoleVariable<int32> CacheRays(TEXT("r.PRTGI.Runtime.CacheRays"),64,TEXT("16..256 fixed cosine samples per surfel and sweep."));
TAutoConsoleVariable<int32> CacheBatch(TEXT("r.PRTGI.Runtime.CacheBatch"),256,TEXT("32..1024 surfels per GPU batch; not a millisecond budget."));
TAutoConsoleVariable<float> CacheRelaxation(TEXT("r.PRTGI.Runtime.CacheRelaxation"),1.f,TEXT("0.1..1 Jacobi relaxation. Values below 1 converge more slowly and do not represent a fixed bounce count."));
TAutoConsoleVariable<int32> CacheBatchesPerSubmit(TEXT("r.PRTGI.Runtime.CacheBatchesPerSubmit"),8,TEXT("1..32 cache dispatches per GPU submission, never crossing a Jacobi sweep. Fixed work count, not ms."));
TAutoConsoleVariable<int32> KeepLighting(TEXT("r.PRTGI.Runtime.KeepLightingWhileUpdating"),1,TEXT("Keep published cache lighting while geometry/probe layout is unchanged. Geometry edits still clear old GI."));
TAutoConsoleVariable<int32> RoomsEnabled(TEXT("r.PRTGI.Runtime.Rooms"),0,TEXT("Opt-in independent room jobs. Each enabled volume requires explicit Runtime Included Actors or Participant Tags. Shared lights affect all rooms."));
TAutoConsoleVariable<int32> ReuseTransport(TEXT("r.PRTGI.Runtime.ReuseTransport"),0,TEXT("Cache ray hits and direct irradiance within one immutable lighting generation. Retrace after geometry/light changes. Experimental PC memory/performance tradeoff."));
TAutoConsoleVariable<int32> TransportBudgetMB(TEXT("r.PRTGI.Runtime.TransportBudgetMB"),64,TEXT("0..512 MiB per room/job for transport queries; 32 bytes per ray, or 48 for regional history. Regional reuse also budgets the old table. Over-budget jobs fall back; regional histories remain after projection."));
TAutoConsoleVariable<int32> RegionalTransport(TEXT("r.PRTGI.Runtime.RegionalTransport"),0,TEXT("Opt-in retained transport queries. Retrace rays intersecting changed instance bounds or their shadow segments. Same participants/materials/lights and stable surface slots required. Irradiance still recomputed globally within each affected room."));
bool NeedsSurfelPool() { return SurfelPoolEnabled.GetValueOnGameThread()!=0 || UseSurfelCache.GetValueOnGameThread()!=0; }
struct FHash
{
    uint32 Value=0;
    template<class T> void Add(const T& V) { Value=FCrc::MemCrc32(&V,sizeof(V),Value); }
    void String(const FString& V) { Add(V.Len());Value=FCrc::StrCrc32(*V,Value); }
    void Transform(const FTransform& T) { Add(T.GetLocation());Add(T.GetRotation());Add(T.GetScale3D()); }
};
struct FAssetTriangle
{
    FVector3f P[3],N[3],T[3],B[3];
    FVector3f Rho[64],Normal[64];
    bool Shadow=true;
};
struct FAsset
{
    TArray<FAssetTriangle> Triangles;
    TSharedPtr<FRuntimeSurfelAsset,ESPMode::ThreadSafe> Surfels;
    FString SurfelError;
    FBox Bounds{ForceInit};
};
struct FInstance
{
    UStaticMeshComponent* Component=nullptr;
    FString Key;
    FTransform Transform;
    FSMInstanceElementId Element;
};
struct FSurfelOwnerKey
{
    FObjectKey Component;
    FSMInstanceElementId Element;
    bool operator==(const FSurfelOwnerKey& R)const { return Component==R.Component && Element==R.Element; }
    friend uint32 GetTypeHash(const FSurfelOwnerKey& K) { return HashCombine(GetTypeHash(K.Component),GetTypeHash(K.Element)); }
};
struct FInstanceRecord
{
    FSurfelOwnerKey Owner;
    FString AssetKey;
    FTransform Transform;
    FBox Bounds;
    bool Shadow=false;
};
struct FWorldTriangle
{
    FVector3f A,B,C;
    bool Shadow;
    const FVector4f* Samples=nullptr;
    FVector4f SurfelBinding=FVector4f(0);
};

TSharedPtr<TArray<FVector4f>> TransformCharts(const FAsset& Asset,const FTransform& Transform)
{
    auto Charts=MakeShared<TArray<FVector4f>>();Charts->Reserve(Asset.Triangles.Num()*128);
    const FMatrix NormalTransform=Transform.ToMatrixWithScale().Inverse().GetTransposed();
    for(const auto& Local:Asset.Triangles)
    {
        for(int32 Y=0;Y<8;++Y)for(int32 X=0;X<8;++X)
        {
            float U=(X+.5f)/8,V=(Y+.5f)/8;if(U+V>1) { U=1-U;V=1-V; }
            const FVector3f Bary(1-U-V,U,V);FVector3f N(0),TX(0),TY(0);
            for(int32 K=0;K<3;++K) { N+=Local.N[K]*Bary[K];TX+=Local.T[K]*Bary[K];TY+=Local.B[K]*Bary[K]; }
            N=FVector3f(FVector(NormalTransform.TransformVector(FVector(N)))).GetSafeNormal();
            TX=FVector3f(Transform.TransformVector(FVector(TX)));TX=(TX-N*FVector3f::DotProduct(TX,N)).GetSafeNormal();
            TY=FVector3f(Transform.TransformVector(FVector(TY)));
            TY=FVector3f::CrossProduct(N,TX)*(FVector3f::DotProduct(FVector3f::CrossProduct(N,TX),TY)<0?-1.f:1.f);
            const auto NM=Local.Normal[Y*8+X];const FVector3f Shading=(TX*NM.X+TY*NM.Y+N*NM.Z).GetSafeNormal();
            Charts->Add(FVector4f(Local.Rho[Y*8+X],0));Charts->Add(FVector4f(Shading,0));
        }
    }
    return Charts;
}

FString AssetKey(UStaticMeshComponent* C)
{
    FHash H;H.String(C->GetStaticMesh()->GetPathName());H.Add(C->GetStaticMesh()->GetLightingGuid());
    for(int32 I=0;I<C->GetNumMaterials();++I)
    {
        const auto* M=C->GetMaterial(I);H.String(M?M->GetPathName():TEXT("none"));
        if(M) { TArray<FGuid> Guids;M->GetLightingGuidChain(true,Guids);for(const auto& G:Guids)H.Add(G); }
    }
    // Per-component vertex paint must not alias another instance's charts.
    if(!C->LODData.IsEmpty() && C->LODData[0].OverrideVertexColors)H.String(C->GetPathName());
    return FString::Printf(TEXT("%s:%08x"),*C->GetStaticMesh()->GetPathName(),H.Value);
}
TArray<FInstance> Instances(UWorld* World,const APRTProbeVolume* Room=nullptr)
{
    TArray<FInstance> Result;
    for(TActorIterator<AActor> It(World);It;++It)
    {
        if(It->IsA<APRTProbeVolume>() || It->IsHidden())continue;
        TInlineComponentArray<UStaticMeshComponent*> Components(*It);
        for(auto* C:Components)
        {
            if(!C->IsVisible() || !C->IsRegistered() || !C->GetStaticMesh() || C->ComponentHasTag(TEXT("PRTGI.Ignore")))continue;
            if(Room)
            {
                bool Included=Room->RuntimeIncludedActors.Contains(*It);
                for(const FName Tag:Room->RuntimeParticipantTags)
                    Included|=It->ActorHasTag(Tag) || C->ComponentHasTag(Tag);
                if(!Included)continue;
            }
            const FString Key=AssetKey(C);
            if(auto* ISM=Cast<UInstancedStaticMeshComponent>(C))
            {
                for(int32 I=0;I<ISM->GetInstanceCount();++I)
                {
                    FTransform T;if(ISM->GetInstanceTransform(I,T,true))
                    {
                        FSMInstanceElementId Id;
                        if(NeedsSurfelPool())Id=FSMInstanceElementIdMap::Get().GetSMInstanceElementIdFromSMInstanceId({ISM,I});
                        Result.Add({C,Key,T,Id});
                    }
                }
            }
            else Result.Add({C,Key,C->GetComponentTransform(),{}});
        }
    }
    return Result;
}
void PrepareSurfelSeeds(FAsset& Asset)
{
    auto Out=MakeShared<FRuntimeSurfelAsset,ESPMode::ThreadSafe>();
    const float Spacing=FMath::Clamp(SeedSpacing.GetValueOnGameThread(),1.f,1000.f);
    for(const auto& T:Asset.Triangles)
    {
        FVector3f GN=FVector3f::CrossProduct(T.P[1]-T.P[0],T.P[2]-T.P[0]).GetSafeNormal();
        if(GN.IsNearlyZero()){Out->TriangleRanges.Add(FIntPoint(0,0));continue;}
        if(FVector3f::DotProduct(GN,T.N[0]+T.N[1]+T.N[2])<0)GN=-GN;
        const float Edge=FMath::Max3((T.P[1]-T.P[0]).Size(),(T.P[2]-T.P[0]).Size(),(T.P[2]-T.P[1]).Size());
        const int32 N=FMath::Max(1,FMath::CeilToInt(Edge/Spacing));
        if(N>32 || Out->Num()+N*N>1048576)
        { Asset.SurfelError=TEXT("Surfel seed budget exceeded; increase asset-local seed spacing and Prepare again");return; }
        Out->TriangleRanges.Add(FIntPoint(Out->Num(),N));
        auto Position=[&](FVector2f UV){return T.P[0]*(1-UV.X-UV.Y)+T.P[1]*UV.X+T.P[2]*UV.Y;};
        auto Add=[&](FVector2f A,FVector2f B,FVector2f C)
        {
            const FVector2f UV=(A+B+C)/3;const FVector3f Bary(1-UV.X-UV.Y,UV.X,UV.Y);
            FVector3f Normal(0),TX(0),TY(0);
            for(int32 K=0;K<3;++K){Normal+=T.N[K]*Bary[K];TX+=T.T[K]*Bary[K];TY+=T.B[K]*Bary[K];}
            Normal=Normal.GetSafeNormal();TX=(TX-Normal*FVector3f::DotProduct(TX,Normal)).GetSafeNormal();
            TY=FVector3f::CrossProduct(Normal,TX)*(FVector3f::DotProduct(FVector3f::CrossProduct(Normal,TX),TY)<0?-1.f:1.f);
            const int32 Sample=FMath::Min(7,FMath::FloorToInt(UV.Y*8))*8+FMath::Min(7,FMath::FloorToInt(UV.X*8));
            const auto NM=T.Normal[Sample];const auto Shading=(TX*NM.X+TY*NM.Y+Normal*NM.Z).GetSafeNormal();
            const auto Center=(Position(A)+Position(B)+Position(C))/3;
            Out->Seeds.Append({FVector4f(Center,0),FVector4f(Shading,0),FVector4f(Position(A)-Center,0),
                FVector4f(Position(B)-Center,0),FVector4f(T.Rho[Sample],0),FVector4f(GN,0)});
        };
        for(int32 Y=0;Y<N;++Y)for(int32 X=0;X<N-Y;++X)
        {
            const FVector2f A(float(X)/N,float(Y)/N),B(float(X+1)/N,float(Y)/N),C(float(X)/N,float(Y+1)/N);
            Add(A,B,C);if(X+Y<N-1)Add(B,FVector2f(float(X+1)/N,float(Y+1)/N),C);
        }
    }
    Asset.Surfels=Out;
}
TSharedPtr<FAsset> PrepareAsset(UStaticMeshComponent* C,FString& Error)
{
#if WITH_EDITOR
    const auto* Render=C->GetStaticMesh()->GetRenderData();
    if(!Render || Render->LODResources.IsEmpty()) { Error=TEXT("Missing LOD0 CPU geometry");return nullptr; }
    const auto& LOD=Render->LODResources[0];const auto Indices=LOD.IndexBuffer.GetArrayView();
    if(Indices.Num()==0) { Error=TEXT("LOD0 index data is not CPU accessible");return nullptr; }
    if(Indices.Num()/3>100000) { Error=TEXT("Asset exceeds the 100000-triangle PC reference budget");return nullptr; }
    if(!C->LODData.IsEmpty() && C->LODData[0].OverrideVertexColors)
    { Error=TEXT("Per-instance vertex paint is not supported by the shared asset reference yet");return nullptr; }
    auto Asset=MakeShared<FAsset>();
    for(const auto& Section:LOD.Sections)
    {
        if(!Section.NumTriangles)continue;
        auto* M=C->GetMaterial(Section.MaterialIndex);
        if(!M || M->GetBlendMode()!=BLEND_Opaque || Cast<UMaterialInstanceDynamic>(M))
        { Error=TEXT("Only opaque, non-MID asset materials are supported");return nullptr; }
        auto Atlas=BakeMaterialAtlas(C,LOD,Section,FTransform::Identity,Error);if(!Atlas)return nullptr;
        for(uint32 I=Section.FirstIndex;I<Section.FirstIndex+Section.NumTriangles*3;I+=3)
        {
            auto& Tri=Asset->Triangles.AddDefaulted_GetRef();Tri.Shadow=Section.bCastShadow;
            for(int32 V=0;V<3;++V)
            {
                const uint32 Index=Indices[I+V];Tri.P[V]=LOD.VertexBuffers.PositionVertexBuffer.VertexPosition(Index);
                Asset->Bounds+=FVector(Tri.P[V]);
                Tri.N[V]=LOD.VertexBuffers.StaticMeshVertexBuffer.VertexTangentZ(Index);
                Tri.T[V]=LOD.VertexBuffers.StaticMeshVertexBuffer.VertexTangentX(Index);
                Tri.B[V]=LOD.VertexBuffers.StaticMeshVertexBuffer.VertexTangentY(Index);
            }
            for(int32 Y=0;Y<8;++Y)for(int32 X=0;X<8;++X)
            {
                float U=(X+.5f)/8,V=(Y+.5f)/8;if(U+V>1) { U=1-U;V=1-V; }
                if(!Atlas->Sample((I-Section.FirstIndex)/3,FVector3f(1-U-V,U,V),Tri.Rho[Y*8+X],Tri.Normal[Y*8+X]))
                { Error=TEXT("Uncovered material chart sample");return nullptr; }
            }
        }
    }
    PrepareSurfelSeeds(*Asset);return Asset;
#else
    Error=TEXT("PC editor reference requires editor-prepared material charts. Cooked asset serialization is not implemented.");return nullptr;
#endif
}
void BuildBVH(TArray<FWorldTriangle>& Triangles,TArray<FVector4f>& Nodes,int32 First,int32 Count)
{
    FBox3f Bounds(ForceInit),Centers(ForceInit);
    for(int32 I=First;I<First+Count;++I) { const auto& T=Triangles[I];Bounds+=T.A;Bounds+=T.B;Bounds+=T.C;Centers+=(T.A+T.B+T.C)/3; }
    const int32 Node=Nodes.Num();Nodes.Add(FVector4f(Bounds.Min,float(First)));Nodes.Add(FVector4f(Bounds.Max,float(Count)));
    if(Count<=8)return;
    const FVector3f Extent=Centers.GetSize();const int32 Axis=Extent.X>Extent.Y?(Extent.X>Extent.Z?0:2):(Extent.Y>Extent.Z?1:2);
    MakeArrayView(Triangles.GetData()+First,Count).Sort([Axis](const auto& A,const auto& B){return A.A[Axis]+A.B[Axis]+A.C[Axis]<B.A[Axis]+B.B[Axis]+B.C[Axis];});
    const int32 Half=Count/2;BuildBVH(Triangles,Nodes,First,Half);BuildBVH(Triangles,Nodes,First+Half,Count-Half);
    Nodes[Node].W=-1;Nodes[Node+1].W=Nodes.Num()/2;
}
TArray<FVector4f> ReadLights(UWorld* World)
{
    TArray<FVector4f> Result;
    for(TActorIterator<AActor> It(World);It;++It)
    {
        if(It->IsHidden())continue;
        TInlineComponentArray<ULightComponent*> Components(*It);
        for(auto* L:Components)
        {
            if(!L->IsVisible() || !L->bAffectsWorld || L->Intensity<=0)continue;
            FLinearColor Color=L->GetLightColor();if(L->bUseTemperature)Color*=L->GetColorTemperature();
            Color*=L->DiffuseScale*L->IndirectLightingIntensity;
            if(auto* D=Cast<UDirectionalLightComponent>(L))
            { Result.Add(FVector4f(FVector3f(-D->GetForwardVector()),0));Result.Add(FVector4f(FVector3f(Color)*D->Intensity,0)); }
            else if(auto* P=Cast<UPointLightComponent>(L);P && !Cast<USpotLightComponent>(P) && P->bUseInverseSquaredFalloff)
            { Result.Add(FVector4f(FVector3f(P->GetComponentLocation()),1));Result.Add(FVector4f(FVector3f(Color)*(P->ComputeLightBrightness()*.0001f),P->AttenuationRadius)); }
        }
    }
    return Result;
}
bool ReadVolumes(UWorld* World,FRuntimeScene& Scene,FString& Error,const APRTProbeVolume* Room)
{
    for(TActorIterator<APRTProbeVolume> It(World);It;++It)
    {
        if(!It->bEnabled || (Room && *It!=Room))continue;
        FBox Grid,Receiver;FIntVector Counts;if(!It->GetLayout(Grid,Receiver,Counts,Error))return false;
        const int64 Count=int64(Counts.X)*Counts.Y*Counts.Z;
        if(Count<1 || Count+Scene.Probes.Num()>32768) { Error=TEXT("PC reference supports at most 32768 probes; reduce volume density");return false; }
        auto& V=Scene.Volumes.AddDefaulted_GetRef();V.GridMin=FVector3f(Grid.Min);V.GridMax=FVector3f(Grid.Max);V.GridCounts=Counts;
        V.ReceiverMin=FVector3f(Receiver.Min);V.ReceiverMax=FVector3f(Receiver.Max);V.Priority=It->Priority;
        V.BlendFace=It->BlendFace;V.BlendDistance=It->BlendDistance;V.bRuntimeGeometry=true;
        for(int32 Z=0;Z<Counts.Z;++Z)for(int32 Y=0;Y<Counts.Y;++Y)for(int32 X=0;X<Counts.X;++X)
            Scene.Probes.Add(FVector4f(V.GridMin+(V.GridMax-V.GridMin)*FVector3f(float(X)/(Counts.X-1),float(Y)/(Counts.Y-1),float(Z)/(Counts.Z-1)),1));
    }
    if(Scene.Probes.IsEmpty()) { Error=TEXT("Add an enabled PRT Probe Volume (no scene precomputation required)");return false; }
    return true;
}
uint32 Signature(const TArray<FInstance>& Items,const FRuntimeScene& Scene,uint32& GeometrySignature)
{
    FHash H;H.Add(Items.Num());
    for(const auto& I:Items) { H.String(I.Component->GetPathName());H.String(I.Key);H.Transform(I.Transform);H.Add(bool(I.Component->CastShadow)); }
    for(const auto& P:Scene.Probes)H.Add(P);
    for(const auto& V:Scene.Volumes) { H.Add(V.GridCounts);H.Add(V.ReceiverMin);H.Add(V.ReceiverMax);H.Add(V.Priority);H.Add(V.BlendFace);H.Add(V.BlendDistance); }
    GeometrySignature=H.Value;
    for(const auto& L:Scene.Lights)H.Add(L);
    H.Add(Scene.Rays);H.Add(Scene.Bounces);H.Add(UseSurfelCache.GetValueOnGameThread());
    H.Add(Scene.CacheIterations);H.Add(Scene.CacheRays);H.Add(Scene.CacheBatch);H.Add(Scene.CacheRelaxation);H.Add(Scene.CacheBatchesPerSubmit);H.Add(KeepLighting.GetValueOnGameThread());
    H.Add(Scene.RegionalTransport);H.Add(Scene.ReuseTransport);H.Add(Scene.TransportBudgetBytes);return H.Value;
}
void SetVolumeStatus(UWorld* World,const FString& Message,const TArray<FPRTGIRenderVolume>& Published,int32 BounceCount,const APRTProbeVolume* Room)
{
    int32 Index=0;
    for(TActorIterator<APRTProbeVolume> It(World);It;++It)
    {
        if(Room && *It!=Room)continue;
        const bool Ready=It->bEnabled && Published.IsValidIndex(Index);
        It->bGIActive=Ready;It->CacheStatus=Message;It->EffectiveBounces=Ready?BounceCount:0;
        It->ValidProbes=It->InvalidProbes=0;
        if(Ready)
        {
            const auto& V=Published[Index];It->ProbeCounts=V.GridCounts;
            for(int32 P=0;P<V.Coefficients.Num();P+=9)
                if(V.Coefficients[P].W<0)++It->InvalidProbes;else ++It->ValidProbes;
        }
        if(It->bEnabled)++Index;
    }
}
}

struct FRuntimeGI::FState
{
    TWeakObjectPtr<UWorld> World;
    bool RoomsActive=false;
    TMap<FObjectKey,TUniquePtr<FRuntimeGI>> Rooms;
    TArray<FObjectKey> RoomOrder;
    int32 NextRoom=0;
    uint32 SurfelCapacity=0;
    TMap<FString,TSharedPtr<FAsset>> Assets;
    // Only transforms present in the current snapshot survive. Moving one
    // chair does not re-transform every sphere's 64 material samples/triangle.
    TMap<FString,TSharedPtr<TArray<FVector4f>>> WorldCharts;
    TSharedPtr<FRuntimeScene,ESPMode::ThreadSafe> Job;
    TArray<FInstanceRecord> JobInstances;
    TUniquePtr<FRuntimeGPU> GPU,SolvedCache;
    TArray<FPRTGIRenderVolume> Published;
    uint32 Requested=0,Completed=0,PublishedGeometry=0;
    bool PublishedCache=false,HoldingLighting=false;
    int32 Generations=0,AssetBuilds=0,Instances=0,Triangles=0,Discarded=0,ChartUpdates=0;
    double NextScan=0,Started=0,LastSeconds=0,SnapshotMs=0;
    FString Error;
    bool Invalidated=true;
    TUniquePtr<FRuntimeSurfelPool> Surfels;
    TMap<FSurfelOwnerKey,uint32> SurfelOwners;
    uint32 NextSurfelOwner=1;
    FString SurfelError,LastSurfelAudit;
};
FRuntimeGI::FRuntimeGI():State(MakeUnique<FState>()){}
FRuntimeGI::~FRuntimeGI()=default;
bool FRuntimeGI::IsEnabled() { return Enable.GetValueOnGameThread()!=0; }
void FRuntimeGI::Reset(bool bKeepPreparedAssets)
{
    // Editor -> PIE changes world ownership, not mesh/material asset identity.
    // Keep UObject-free charts so entering play never starts an editor bake.
    auto Assets=bKeepPreparedAssets?MoveTemp(State->Assets):TMap<FString,TSharedPtr<FAsset>>();
    const int32 Builds=bKeepPreparedAssets?State->AssetBuilds:0;
    State=MakeUnique<FState>();State->Assets=MoveTemp(Assets);State->AssetBuilds=Builds;
}
const TArray<FPRTGIRenderVolume>& FRuntimeGI::GetVolumes()const { return State->Published; }
FString FRuntimeGI::Prepare(UWorld* World)
{
    if(!World)return TEXT("No world");
    // Explicit preparation also invalidates charts after edited material inputs.
    Reset(false);State->World=World;const auto Items=Instances(World);
    for(const auto& I:Items)if(!State->Assets.Contains(I.Key))
    {
        FString Error;auto Asset=PrepareAsset(I.Component,Error);
        if(!Asset) { State->Error=I.Component->GetPathName()+TEXT(": ")+Error;UE_LOG(LogPRTRuntime,Error,TEXT("%s"),*State->Error);return State->Error; }
        State->Assets.Add(I.Key,Asset);++State->AssetBuilds;
    }
    return FString::Printf(TEXT("Prepared %d asset/material variants for %d instances. No scene transfer bake. Enable: r.PRTGI.Runtime 1"),State->Assets.Num(),Items.Num());
}
bool FRuntimeGI::Tick(UWorld* World)
{
    const bool Rooms=RoomsEnabled.GetValueOnGameThread()!=0;
    if(State->World.Get()!=World || State->RoomsActive!=Rooms)
    { Reset();State->World=World;State->RoomsActive=Rooms; }
    return Rooms?TickRooms(World):TickInternal(World,true);
}
bool FRuntimeGI::TickRooms(UWorld* World)
{
    auto& S=*State;if(!World)return false;
    TArray<APRTProbeVolume*> Volumes;
    for(TActorIterator<APRTProbeVolume> It(World);It;++It)
        if(It->bEnabled)Volumes.Add(*It);
        else { It->bGIActive=false;It->ValidProbes=It->InvalidProbes=0;It->EffectiveBounces=0;It->CacheStatus=TEXT("Runtime room: disabled"); }
    Volumes.Sort([](const APRTProbeVolume& A,const APRTProbeVolume& B){return A.GetPathName()<B.GetPathName();});
    TArray<FObjectKey> Order;for(auto* V:Volumes)Order.Add(FObjectKey(V));
    bool Dirty=S.Invalidated || Order!=S.RoomOrder;S.Invalidated=false;
    for(auto It=S.Rooms.CreateIterator();It;++It)if(!Order.Contains(It.Key()))It.RemoveCurrent();
    S.RoomOrder=Order;
    for(auto* V:Volumes)
    {
        const FObjectKey Key(V);
        if(!S.Rooms.Contains(Key))
        {
            auto Worker=MakeUnique<FRuntimeGI>();Worker->bRoomWorker=true;Worker->RoomVolume=V;
            Worker->State->World=World;Worker->State->Assets=S.Assets;
            S.Rooms.Add(Key,MoveTemp(Worker));
        }
    }
    // One shared in-flight lighting submission, round-robin across rooms.
    // Always retire completed work, even when this room has no submission grant.
    bool Pending=false;
    for(const auto& Pair:S.Rooms)Pending|=Pair.Value->State->GPU && Pair.Value->State->GPU->HasPendingSubmission();
    int32 Selected=INDEX_NONE;
    if(!Pending && !Order.IsEmpty())for(int32 I=0;I<Order.Num();++I)
    {
        const int32 Index=(S.NextRoom+I)%Order.Num();
        if(S.Rooms.FindChecked(Order[Index])->State->GPU)
        { Selected=Index;S.NextRoom=(Index+1)%Order.Num();break; }
    }
    for(int32 I=0;I<Order.Num();++I)Dirty|=S.Rooms.FindChecked(Order[I])->TickInternal(World,I==Selected);
    if(Dirty)
    {
        S.Published.Reset();
        for(const auto& Key:Order)S.Published.Append(S.Rooms.FindChecked(Key)->GetVolumes());
    }
    return Dirty;
}
bool FRuntimeGI::TickInternal(UWorld* World,bool bAllowSubmission)
{
    auto& S=*State;
    if(S.World.Get()!=World) { Reset();State->World=World;return true; }
    const APRTProbeVolume* Room=bRoomWorker?RoomVolume.Get():nullptr;
    if(bRoomWorker && !Room)return false;
    if(S.Surfels)
    {
        FString Audit;bool Passed=false;
        if(S.Surfels->PollAudit(Audit,Passed))
        {
            S.LastSurfelAudit=Audit;UE_LOG(LogPRTRuntime,Display,TEXT("%s"),*Audit);
            const FString Path=FPaths::ProjectSavedDir()/(Room?FString::Printf(TEXT("PRTGIRuntime/room-%s-surfel-audit.txt"),*Room->VolumeId.ToString()):TEXT("PRTGIRuntime/surfel-audit.txt"));
            IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path),true);FFileHelper::SaveStringToFile(Audit,*Path);
        }
    }
    const double Now=FPlatformTime::Seconds();
    const bool Invalidated=S.Invalidated;S.Invalidated=false;
    TArray<FVector4f> Output;const bool Finished=S.GPU && S.GPU->Tick(Output,bAllowSubmission);
    if(Now<S.NextScan && !Finished)return false;
    S.NextScan=Now+.25;
    auto Scene=MakeShared<FRuntimeScene,ESPMode::ThreadSafe>();Scene->Rays=FMath::Clamp(Rays.GetValueOnGameThread(),64,1024);
    Scene->Bounces=FMath::Clamp(Bounces.GetValueOnGameThread(),1,12);Scene->Lights=ReadLights(World);
    Scene->CacheIterations=FMath::Clamp(CacheIterations.GetValueOnGameThread(),1,32);
    Scene->CacheRays=FMath::Clamp(CacheRays.GetValueOnGameThread(),16,256);
    Scene->CacheBatch=FMath::Clamp(CacheBatch.GetValueOnGameThread(),32,1024);
    Scene->CacheBatchesPerSubmit=FMath::Clamp(CacheBatchesPerSubmit.GetValueOnGameThread(),1,32);
    Scene->CacheRelaxation=FMath::Clamp(CacheRelaxation.GetValueOnGameThread(),.1f,1.f);
    Scene->RegionalTransport=RegionalTransport.GetValueOnGameThread()!=0;
    Scene->ReuseTransport=ReuseTransport.GetValueOnGameThread()!=0 || Scene->RegionalTransport;
    Scene->TransportBudgetBytes=uint64(FMath::Clamp(TransportBudgetMB.GetValueOnGameThread(),0,512))*1024*1024;
    FString Error;
    if(!ReadVolumes(World,*Scene,Error,Room))
    { const bool Dirty=Invalidated || !S.Published.IsEmpty();S.Error=Error;S.GPU.Reset();S.SolvedCache.Reset();S.Job.Reset();S.Published.Reset();S.HoldingLighting=false;S.Requested=0;
      SetVolumeStatus(World,TEXT("Runtime: ")+Error,S.Published,0,Room);return Dirty; }
    const auto Items=Instances(World,Room);
    if(Room && Items.IsEmpty())
    {
        const bool Dirty=Invalidated || !S.Published.IsEmpty();
        S.Error=TEXT("Runtime room has no eligible geometry: assign Runtime Included Actors or Participant Tags");
        S.GPU.Reset();S.SolvedCache.Reset();S.Job.Reset();S.Published.Reset();S.Surfels.Reset();S.SurfelCapacity=0;
        S.HoldingLighting=false;S.Requested=0;S.Instances=S.Triangles=0;
        SetVolumeStatus(World,S.Error,S.Published,0,Room);return Dirty;
    }
    uint32 GeometrySignature=0;const uint32 Current=Signature(Items,*Scene,GeometrySignature);
    if(NeedsSurfelPool())
    {
        uint32 Required=65536;
        if(Room)
        {
            uint64 Seeds=0;for(const auto& I:Items)
                if(const auto Asset=S.Assets.FindRef(I.Key);Asset && Asset->Surfels)Seeds+=Asset->Surfels->Num();
            Required=FMath::RoundUpToPowerOfTwo(uint32(FMath::Clamp<uint64>(Seeds,256,65536)));
        }
        if(!S.Surfels || Required>S.SurfelCapacity)
        { S.SurfelCapacity=Required;S.Surfels=MakeUnique<FRuntimeSurfelPool>(Required,FMath::Clamp(Required/4,256u,16384u)); }
        TArray<FRuntimeSurfelInstance> SurfelItems;S.SurfelError.Reset();TSet<FSurfelOwnerKey> Seen;
        for(const auto& I:Items)
        {
            if(I.Transform.GetScale3D().GetAbsMin()<1.e-6)continue;
            const auto Asset=S.Assets.FindRef(I.Key);
            if(!Asset || !Asset->Surfels)
            { S.SurfelError=Asset?Asset->SurfelError:TEXT("Unprepared Surfel asset; run Runtime.Prepare");break; }
            if(Cast<UInstancedStaticMeshComponent>(I.Component) && !I.Element)
            { S.SurfelError=TEXT("ISM instance has no stable element identity");break; }
            const FSurfelOwnerKey Key{FObjectKey(I.Component),I.Element};Seen.Add(Key);
            uint32* Id=S.SurfelOwners.Find(Key);
            if(!Id)Id=&S.SurfelOwners.Add(Key,S.NextSurfelOwner++);
            SurfelItems.Add({*Id,Asset->Surfels,I.Transform});
        }
        if(S.SurfelError.IsEmpty())
        {
            S.Surfels->Update(SurfelItems,S.SurfelError);
            for(auto It=S.SurfelOwners.CreateIterator();It;++It)if(!Seen.Contains(It.Key()))It.RemoveCurrent();
        }
    }
    else if(S.Surfels){S.Surfels.Reset();S.SurfelOwners.Reset();S.NextSurfelOwner=1;S.SurfelError.Reset();}
    if(UseSurfelCache.GetValueOnGameThread()!=0 && !S.SurfelError.IsEmpty())
    {
        S.Error=S.SurfelError;S.GPU.Reset();S.SolvedCache.Reset();S.Job.Reset();S.Published.Reset();S.HoldingLighting=false;
        SetVolumeStatus(World,S.Error,S.Published,0,Room);return true;
    }
    if(Current!=S.Requested || !S.Job)
    {
        const bool HadOutput=Invalidated || !S.Published.IsEmpty();
        S.HoldingLighting=KeepLighting.GetValueOnGameThread()!=0 && UseSurfelCache.GetValueOnGameThread()!=0 &&
            S.PublishedCache && S.PublishedGeometry==GeometrySignature && !S.Published.IsEmpty();
        if(!S.HoldingLighting)S.Published.Reset();
        const auto OldScene=S.Job;
        TSharedPtr<FRuntimeTransportHistory,ESPMode::ThreadSafe> History;
        TArray<FInstanceRecord> Records;
        bool Compatible=Scene->RegionalTransport && S.SolvedCache && OldScene && OldScene->RegionalTransport &&
            OldScene->Lights==Scene->Lights && OldScene->CacheRays==Scene->CacheRays && Items.Num()==S.JobInstances.Num();
        FBox Changed(ForceInit);
        for(int32 I=0;I<Items.Num();++I)
        {
            const auto& Item=Items[I];const auto Asset=S.Assets.FindRef(Item.Key);
            if(!Asset){Compatible=false;continue;}
            FInstanceRecord Record{{FObjectKey(Item.Component),Item.Element},Item.Key,Item.Transform,
                Asset->Bounds.TransformBy(Item.Transform),bool(Item.Component->CastShadow)};
            if(Compatible)
            {
                const auto& Old=S.JobInstances[I];
                // Scale changes currently fail the strict full-trace comparison in
                // the Showcase fixture. Keep them on the conservative full path.
                if(!(Old.Owner==Record.Owner) || Old.AssetKey!=Record.AssetKey || Old.Shadow!=Record.Shadow ||
                    !Old.Transform.GetScale3D().Equals(Record.Transform.GetScale3D(),0))Compatible=false;
                else if(!Old.Transform.Equals(Record.Transform,0)) { Changed+=Old.Bounds;Changed+=Record.Bounds; }
            }
            Records.Add(MoveTemp(Record));
        }
        if(Compatible)
        {
            History=S.SolvedCache->CaptureTransportHistory();
            if(Changed.IsValid)
            {
                // Conservative float-position allowance, including old hit endpoints.
                const double Margin=1.+8*FLT_EPSILON*FMath::Max(Changed.Min.GetAbsMax(),Changed.Max.GetAbsMax());
                const auto Padded=Changed.ExpandBy(Margin);
                Scene->ChangedBounds=FBox3f(FVector3f(Padded.Min),FVector3f(Padded.Max));
            }
        }
        S.JobInstances=MoveTemp(Records);
        if(S.GPU) { ++S.Discarded;S.GPU.Reset(); }
        S.SolvedCache.Reset();S.Job.Reset();S.Requested=Current;S.Error.Reset();S.Instances=Items.Num();
        TArray<FWorldTriangle> Triangles;
        TMap<FString,TSharedPtr<TArray<FVector4f>>> NextCharts;S.ChartUpdates=0;
        const double Start=FPlatformTime::Seconds();
        for(const auto& I:Items)
        {
            auto* Asset=S.Assets.Find(I.Key);
            if(!Asset) { S.Error=TEXT("Unprepared or changed asset: ")+I.Key+TEXT(". Run PRTGI.Runtime.Prepare.");
                S.Published.Reset();S.HoldingLighting=false;SetVolumeStatus(World,S.Error,S.Published,0,Room);return HadOutput; }
            if(I.Transform.GetScale3D().GetAbsMin()<1.e-6)continue;
            if(Triangles.Num()+(*Asset)->Triangles.Num()>100000) { S.Error=TEXT("100000-triangle PC reference budget exceeded");S.Published.Reset();S.HoldingLighting=false;SetVolumeStatus(World,S.Error,S.Published,0,Room);return HadOutput; }
            FHash TransformHash;TransformHash.Transform(I.Transform);
            const FString ChartKey=I.Key+FString::Printf(TEXT(":%08x"),TransformHash.Value);
            auto Charts=S.WorldCharts.FindRef(ChartKey);
            if(!Charts)Charts=NextCharts.FindRef(ChartKey);
            if(!Charts) { Charts=TransformCharts(**Asset,I.Transform);++S.ChartUpdates; }
            NextCharts.Add(ChartKey,Charts);
            int32 TriangleIndex=0;
            for(const auto& T:(*Asset)->Triangles)
            {
                FWorldTriangle Tri{FVector3f(I.Transform.TransformPosition(FVector(T.P[0]))),FVector3f(I.Transform.TransformPosition(FVector(T.P[1]))),
                    FVector3f(I.Transform.TransformPosition(FVector(T.P[2]))),T.Shadow && bool(I.Component->CastShadow),Charts->GetData()+TriangleIndex*128};
                if(UseSurfelCache.GetValueOnGameThread()!=0)
                {
                    const FSurfelOwnerKey Key{FObjectKey(I.Component),I.Element};
                    Tri.SurfelBinding=S.Surfels->TriangleBinding(S.SurfelOwners.FindChecked(Key),TriangleIndex);
                    if(Tri.SurfelBinding.Y==0 && FVector3f::CrossProduct(Tri.B-Tri.A,Tri.C-Tri.A).SizeSquared()>1.e-8)
                    { S.Error=TEXT("Missing triangle-to-surfel binding; Prepare again");S.Published.Reset();S.HoldingLighting=false;SetVolumeStatus(World,S.Error,S.Published,0,Room);return HadOutput; }
                }
                if(FVector3f::CrossProduct(Tri.B-Tri.A,Tri.C-Tri.A).SizeSquared()>1.e-8)Triangles.Add(Tri);
                ++TriangleIndex;
            }
        }
        Scene->Geometry=MakeShared<FMultiBounceData,ESPMode::ThreadSafe>();
        if(!Triangles.IsEmpty())BuildBVH(Triangles,Scene->Geometry->Nodes,0,Triangles.Num());
        Scene->Materials.Reserve(Triangles.Num()*128);
        for(const auto& T:Triangles)
        {
            Scene->Geometry->Triangles.Append({FVector4f(T.A,T.Shadow?1:0),FVector4f(T.B-T.A,0),FVector4f(T.C-T.A,0)});
            Scene->Materials.Append(T.Samples,128);Scene->SurfelBindings.Add(T.SurfelBinding);
        }
        if(UseSurfelCache.GetValueOnGameThread()!=0)Scene->SurfelSnapshot=S.Surfels->CaptureLightingSnapshot();
        if(History && OldScene)
        {
            // BVH sorting changes triangle indices. Stable owner/first-slot keys
            // identify the same local triangle without retaining UObject pointers.
            auto Key=[](const FVector4f& B){return (uint64(uint32(B.W))<<32)|uint32(B.X);};
            TMap<uint64,uint32> NewIndices;
            for(int32 I=0;I<Scene->SurfelBindings.Num();++I)if(Scene->SurfelBindings[I].Y>0)NewIndices.Add(Key(Scene->SurfelBindings[I]),I);
            for(const auto& Binding:OldScene->SurfelBindings)
            {
                const auto* Index=Binding.Y>0?NewIndices.Find(Key(Binding)):nullptr;
                Scene->OldToNewTriangles.Add(Index?*Index:MAX_uint32);
            }
        }
        S.WorldCharts=MoveTemp(NextCharts);
        S.Triangles=Triangles.Num();S.SnapshotMs=(FPlatformTime::Seconds()-Start)*1000;
        S.Job=Scene;S.GPU=MakeUnique<FRuntimeGPU>();S.GPU->Submit(Scene,Budget.GetValueOnGameThread(),History);
        S.Started=Now;++S.Generations;
        SetVolumeStatus(World,TEXT("Runtime reference: updating (no scene precompute)"),S.Published,0,Room);
        UE_LOG(LogPRTRuntime,Display,TEXT("Generation %d: %d instances, %d triangles, %d probes, CPU snapshot %.2f ms"),S.Generations,S.Instances,S.Triangles,Scene->Probes.Num(),S.SnapshotMs);
        return true;
    }
    if(Finished)
    {
        if(Output.Num()!=S.Job->Probes.Num()*9 || Output.ContainsByPredicate([](const auto& C){return C.ContainsNaN();}))
        { S.Error=TEXT("GPU readback failed or returned non-finite SH");S.GPU.Reset();S.Published.Reset();S.HoldingLighting=false;SetVolumeStatus(World,S.Error,S.Published,0,Room);return true; }
        S.Published.Reset();S.HoldingLighting=false;
        int32 Offset=0;
        for(auto V:S.Job->Volumes)
        {
            const int32 Count=V.GridCounts.X*V.GridCounts.Y*V.GridCounts.Z*9;
            V.Coefficients.Append(Output.GetData()+Offset,Count);Offset+=Count;V.Geometry=S.Job->Geometry;
            S.Published.Add(MoveTemp(V));
        }
        S.PublishedGeometry=GeometrySignature;S.PublishedCache=S.Job->SurfelSnapshot.IsValid();
        S.Completed=Current;S.LastSeconds=Now-S.Started;
        if(S.Job->SurfelSnapshot) { S.GPU->RetainCacheAndReleaseScratch();S.SolvedCache=MoveTemp(S.GPU); }
        else S.GPU.Reset();
        SetVolumeStatus(World,S.Job->SurfelSnapshot?TEXT("Runtime cache: ready (iterative surfel irradiance)"):TEXT("Runtime reference: ready (triangle BVH)"),S.Published,S.Job->SurfelSnapshot?0:S.Job->Bounces,Room);
        UE_LOG(LogPRTRuntime,Display,TEXT("Published generation %d in %.3f seconds (amortized convergence, not GPU frame time)"),S.Generations,S.LastSeconds);
        return true;
    }
    return false;
}
FString FRuntimeGI::Status()const
{
    const auto& S=*State;
    if(S.RoomsActive)
    {
        FString Result=FString::Printf(TEXT("RuntimeRooms=1 Rooms=%d Assets=%d AssetBuilds=%d Published=%d"),S.Rooms.Num(),S.Assets.Num(),S.AssetBuilds,S.Published.Num());
        for(const auto& Key:S.RoomOrder)
        {
            const auto& Worker=*S.Rooms.FindChecked(Key);
            Result+=FString::Printf(TEXT("\nRoom=%s %s"),*GetPathNameSafe(Worker.RoomVolume.Get()),*Worker.Status());
        }
        return Result;
    }
    return FString::Printf(TEXT("RuntimeReference=%d Assets=%d AssetBuilds=%d Instances=%d Triangles=%d Probes=%d Generation=%d Discarded=%d Pending=%d Published=%d ChartUpdates=%d SnapshotMs=%.3f ConvergenceSeconds=%.3f Error=%s %s SurfelError=%s LastAudit=[%s] SurfelCache=%d CacheIterations=%d CacheRays=%d CacheRelaxation=%.3f CacheRetained=%d HoldingLighting=%d CacheBatchesPerSubmit=%d %s"),
        IsEnabled(),S.Assets.Num(),S.AssetBuilds,S.Instances,S.Triangles,S.Job?S.Job->Probes.Num():0,S.Generations,S.Discarded,S.GPU?1:0,S.Published.Num(),S.ChartUpdates,S.SnapshotMs,S.LastSeconds,*S.Error,
        S.Surfels?*S.Surfels->Status():TEXT("SurfelPool=disabled"),*S.SurfelError,*S.LastSurfelAudit,UseSurfelCache.GetValueOnGameThread(),CacheIterations.GetValueOnGameThread(),CacheRays.GetValueOnGameThread(),CacheRelaxation.GetValueOnGameThread(),S.SolvedCache?1:0,S.HoldingLighting?1:0,CacheBatchesPerSubmit.GetValueOnGameThread(),S.GPU?*S.GPU->Progress():(S.SolvedCache?*S.SolvedCache->Progress():TEXT("")));
}
FString FRuntimeGI::AuditSurfels()
{
    if(State->RoomsActive)
    {
        FString Result;for(const auto& Key:State->RoomOrder)
        { auto& Worker=*State->Rooms.FindChecked(Key);Result+=GetPathNameSafe(Worker.RoomVolume.Get())+TEXT(": ")+Worker.AuditSurfels()+TEXT("\n"); }
        return Result;
    }
    FString Error;
    if(!State->Surfels)return TEXT("Enable r.PRTGI.Runtime.SurfelPool 1 and runtime GI first");
    return State->Surfels->RequestAudit(Error)?TEXT("GPU Surfel audit queued; result appears in Runtime.Status and Saved/PRTGIRuntime/*surfel-audit.txt"):Error;
}
FString FRuntimeGI::Dump()const
{
    FString CSV=TEXT("volume,probe,x,y,z,valid,c0r,c0g,c0b,c1r,c1g,c1b,c2r,c2g,c2b,c3r,c3g,c3b,c4r,c4g,c4b,c5r,c5g,c5b,c6r,c6g,c6b,c7r,c7g,c7b,c8r,c8g,c8b\n");
    for(int32 I=0;I<State->Published.Num();++I)
    {
        const auto& V=State->Published[I];const auto N=V.GridCounts;
        for(int32 P=0;P<V.Coefficients.Num()/9;++P)
        {
            const FVector3f Position=V.GridMin+(V.GridMax-V.GridMin)*FVector3f(float(P%N.X)/(N.X-1),float((P/N.X)%N.Y)/(N.Y-1),float(P/(N.X*N.Y))/(N.Z-1));
            CSV+=FString::Printf(TEXT("%d,%d,%.6g,%.6g,%.6g,%.0f"),I,P,Position.X,Position.Y,Position.Z,V.Coefficients[P*9].W);
            for(int32 C=0;C<9;++C) { const auto A=V.Coefficients[P*9+C];CSV+=FString::Printf(TEXT(",%.9g,%.9g,%.9g"),A.X,A.Y,A.Z); }
            CSV+=TEXT("\n");
        }
    }
    const FString Path=FPaths::ProjectSavedDir()/TEXT("PRTGIRuntime/probes.csv");IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path),true);
    if(State->RoomsActive)
    {
        FString Mapping;int32 Index=0;
        for(const auto& Key:State->RoomOrder)
        {
            const auto& Worker=*State->Rooms.FindChecked(Key);
            if(Worker.RoomVolume.IsValid() && !Worker.GetVolumes().IsEmpty())Mapping+=FString::Printf(TEXT("%d\t%s\t%s\n"),Index++,*Worker.RoomVolume->VolumeId.ToString(),*Worker.RoomVolume->GetPathName());
        }
        FFileHelper::SaveStringToFile(Mapping,*(FPaths::GetPath(Path)/TEXT("rooms.tsv")));
    }
    return FFileHelper::SaveStringToFile(CSV,*Path)?Path:TEXT("Failed to write probe dump");
}
}
