#include "PRTGIRuntime.h"
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

DEFINE_LOG_CATEGORY_STATIC(LogPRTRuntime,Log,All);
namespace PRTGI
{
namespace
{
TAutoConsoleVariable<int32> Enable(TEXT("r.PRTGI.Runtime"),0,TEXT("Experimental PC live-transport reference. Prepare assets first with PRTGI.Runtime.Prepare."));
TAutoConsoleVariable<int32> Rays(TEXT("r.PRTGI.Runtime.Rays"),256,TEXT("Deterministic rays per probe, 64..1024. Reference quality, not a frame-time guarantee."));
TAutoConsoleVariable<int32> Bounces(TEXT("r.PRTGI.Runtime.Bounces"),6,TEXT("Maximum runtime diffuse path bounces, 1..12."));
TAutoConsoleVariable<int32> Budget(TEXT("r.PRTGI.Runtime.ProbesPerBatch"),32,TEXT("1..256 probes per GPU batch; at most one batch in flight."));
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
struct FAsset { TArray<FAssetTriangle> Triangles; };
struct FInstance
{
    UStaticMeshComponent* Component=nullptr;
    FString Key;
    FTransform Transform;
};
struct FWorldTriangle
{
    FVector3f A,B,C;
    bool Shadow;
    const FVector4f* Samples=nullptr;
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
TArray<FInstance> Instances(UWorld* World)
{
    TArray<FInstance> Result;
    for(TActorIterator<AActor> It(World);It;++It)
    {
        if(It->IsA<APRTProbeVolume>() || It->IsHidden())continue;
        TInlineComponentArray<UStaticMeshComponent*> Components(*It);
        for(auto* C:Components)
        {
            if(!C->IsVisible() || !C->IsRegistered() || !C->GetStaticMesh() || C->ComponentHasTag(TEXT("PRTGI.Ignore")))continue;
            const FString Key=AssetKey(C);
            if(auto* ISM=Cast<UInstancedStaticMeshComponent>(C))
            {
                for(int32 I=0;I<ISM->GetInstanceCount();++I)
                { FTransform T;if(ISM->GetInstanceTransform(I,T,true))Result.Add({C,Key,T}); }
            }
            else Result.Add({C,Key,C->GetComponentTransform()});
        }
    }
    return Result;
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
    return Asset;
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
bool ReadVolumes(UWorld* World,FRuntimeScene& Scene,FString& Error)
{
    for(TActorIterator<APRTProbeVolume> It(World);It;++It)
    {
        if(!It->bEnabled)continue;
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
uint32 Signature(const TArray<FInstance>& Items,const FRuntimeScene& Scene)
{
    FHash H;H.Add(Items.Num());
    for(const auto& I:Items) { H.String(I.Component->GetPathName());H.String(I.Key);H.Transform(I.Transform);H.Add(bool(I.Component->CastShadow)); }
    for(const auto& P:Scene.Probes)H.Add(P);
    for(const auto& V:Scene.Volumes) { H.Add(V.GridCounts);H.Add(V.ReceiverMin);H.Add(V.ReceiverMax);H.Add(V.Priority);H.Add(V.BlendFace);H.Add(V.BlendDistance); }
    for(const auto& L:Scene.Lights)H.Add(L);
    H.Add(Scene.Rays);H.Add(Scene.Bounces);return H.Value;
}
void SetVolumeStatus(UWorld* World,const FString& Message,const TArray<FPRTGIRenderVolume>& Published,int32 BounceCount)
{
    int32 Index=0;
    for(TActorIterator<APRTProbeVolume> It(World);It;++It)
    {
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
    TMap<FString,TSharedPtr<FAsset>> Assets;
    // Only transforms present in the current snapshot survive. Moving one
    // chair does not re-transform every sphere's 64 material samples/triangle.
    TMap<FString,TSharedPtr<TArray<FVector4f>>> WorldCharts;
    TSharedPtr<FRuntimeScene,ESPMode::ThreadSafe> Job;
    TUniquePtr<FRuntimeGPU> GPU;
    TArray<FPRTGIRenderVolume> Published;
    uint32 Requested=0,Completed=0;
    int32 Generations=0,AssetBuilds=0,Instances=0,Triangles=0,Discarded=0,ChartUpdates=0;
    double NextScan=0,Started=0,LastSeconds=0,SnapshotMs=0;
    FString Error;
    bool Invalidated=true;
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
    auto& S=*State;
    if(S.World.Get()!=World) { Reset();State->World=World;return true; }
    const double Now=FPlatformTime::Seconds();
    const bool Invalidated=S.Invalidated;S.Invalidated=false;
    TArray<FVector4f> Output;const bool Finished=S.GPU && S.GPU->Tick(Output);
    if(Now<S.NextScan && !Finished)return false;
    S.NextScan=Now+.25;
    auto Scene=MakeShared<FRuntimeScene,ESPMode::ThreadSafe>();Scene->Rays=FMath::Clamp(Rays.GetValueOnGameThread(),64,1024);
    Scene->Bounces=FMath::Clamp(Bounces.GetValueOnGameThread(),1,12);Scene->Lights=ReadLights(World);
    FString Error;
    if(!ReadVolumes(World,*Scene,Error))
    { const bool Dirty=Invalidated || !S.Published.IsEmpty();S.Error=Error;S.GPU.Reset();S.Job.Reset();S.Published.Reset();S.Requested=0;
      SetVolumeStatus(World,TEXT("Runtime: ")+Error,S.Published,0);return Dirty; }
    const auto Items=Instances(World);const uint32 Current=Signature(Items,*Scene);
    if(Current!=S.Requested || !S.Job)
    {
        const bool HadOutput=Invalidated || !S.Published.IsEmpty();S.Published.Reset();
        if(S.GPU) { ++S.Discarded;S.GPU.Reset(); }
        S.Job.Reset();S.Requested=Current;S.Error.Reset();S.Instances=Items.Num();
        TArray<FWorldTriangle> Triangles;
        TMap<FString,TSharedPtr<TArray<FVector4f>>> NextCharts;S.ChartUpdates=0;
        const double Start=FPlatformTime::Seconds();
        for(const auto& I:Items)
        {
            auto* Asset=S.Assets.Find(I.Key);
            if(!Asset) { S.Error=TEXT("Unprepared or changed asset: ")+I.Key+TEXT(". Run PRTGI.Runtime.Prepare.");
                SetVolumeStatus(World,S.Error,S.Published,0);return HadOutput; }
            if(I.Transform.GetScale3D().GetAbsMin()<1.e-6)continue;
            if(Triangles.Num()+(*Asset)->Triangles.Num()>100000) { S.Error=TEXT("100000-triangle PC reference budget exceeded");return HadOutput; }
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
            Scene->Materials.Append(T.Samples,128);
        }
        S.WorldCharts=MoveTemp(NextCharts);
        S.Triangles=Triangles.Num();S.SnapshotMs=(FPlatformTime::Seconds()-Start)*1000;
        S.Job=Scene;S.GPU=MakeUnique<FRuntimeGPU>();S.GPU->Submit(Scene,Budget.GetValueOnGameThread());
        S.Started=Now;++S.Generations;
        SetVolumeStatus(World,TEXT("Runtime reference: updating (no scene precompute)"),S.Published,0);
        UE_LOG(LogPRTRuntime,Display,TEXT("Generation %d: %d instances, %d triangles, %d probes, CPU snapshot %.2f ms"),S.Generations,S.Instances,S.Triangles,Scene->Probes.Num(),S.SnapshotMs);
        return true;
    }
    if(Finished)
    {
        if(Output.Num()!=S.Job->Probes.Num()*9 || Output.ContainsByPredicate([](const auto& C){return C.ContainsNaN();}))
        { S.Error=TEXT("GPU readback failed or returned non-finite SH");S.GPU.Reset();SetVolumeStatus(World,S.Error,S.Published,0);return false; }
        int32 Offset=0;
        for(auto V:S.Job->Volumes)
        {
            const int32 Count=V.GridCounts.X*V.GridCounts.Y*V.GridCounts.Z*9;
            V.Coefficients.Append(Output.GetData()+Offset,Count);Offset+=Count;V.Geometry=S.Job->Geometry;
            S.Published.Add(MoveTemp(V));
        }
        S.Completed=Current;S.LastSeconds=Now-S.Started;S.GPU.Reset();
        SetVolumeStatus(World,TEXT("Runtime reference: ready (triangle BVH)"),S.Published,S.Job->Bounces);
        UE_LOG(LogPRTRuntime,Display,TEXT("Published generation %d in %.3f seconds (amortized convergence, not GPU frame time)"),S.Generations,S.LastSeconds);
        return true;
    }
    return false;
}
FString FRuntimeGI::Status()const
{
    const auto& S=*State;
    return FString::Printf(TEXT("RuntimeReference=%d Assets=%d AssetBuilds=%d Instances=%d Triangles=%d Probes=%d Generation=%d Discarded=%d Pending=%d Published=%d ChartUpdates=%d SnapshotMs=%.3f ConvergenceSeconds=%.3f Error=%s"),
        IsEnabled(),S.Assets.Num(),S.AssetBuilds,S.Instances,S.Triangles,S.Job?S.Job->Probes.Num():0,S.Generations,S.Discarded,S.GPU?1:0,S.Published.Num(),S.ChartUpdates,S.SnapshotMs,S.LastSeconds,*S.Error);
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
    return FFileHelper::SaveStringToFile(CSV,*Path)?Path:TEXT("Failed to write probe dump");
}
}
