#pragma once
#include "CoreMinimal.h"
#include "PRTGIVolumeData.h"
#include "RenderGraphResources.h"

class UWorld;
class APRTProbeVolume;
namespace PRTGI
{
// Preserve CPU SH only for an identical probe layout. Resolve visibility always
// uses current geometry; old/new edited bounds conservatively reject probes.
bool RetainRuntimeLighting(TArray<FPRTGIRenderVolume>& Published,
    const TArray<FPRTGIRenderVolume>& Layout,
    TSharedPtr<const FMultiBounceData,ESPMode::ThreadSafe> Geometry,
    const TArray<FBox>& ChangedBounds,int32& NewlyMasked);
bool PublishRuntimeProbeRange(TArray<FPRTGIRenderVolume>& Published,const TArray<FPRTGIRenderVolume>& Layout,
    TSharedPtr<const FMultiBounceData,ESPMode::ThreadSafe> Geometry,int32 First,TConstArrayView<FVector4f> Coefficients);
// PC live transport with reference path tracing and an optional iterative
// surface irradiance cache. Both use triangle BVH traversal. Asset charts are
// prepared once; no scene transfer matrix or evaluated light is stored on disk.
class FRuntimeGI
{
public:
    FRuntimeGI();
    ~FRuntimeGI();
    static bool IsEnabled();
    bool Tick(UWorld* World);
    void Reset(bool bKeepPreparedAssets=true);
    FString Prepare(UWorld* World);
    FString Status() const;
    FString Dump() const;
    FString AuditSurfels();
    const TArray<FPRTGIRenderVolume>& GetVolumes() const;
private:
    bool TickInternal(UWorld* World, bool bAllowSubmission);
    bool TickRooms(UWorld* World);
    TWeakObjectPtr<APRTProbeVolume> RoomVolume;
    bool bRoomWorker=false;
    struct FState;
    TUniquePtr<FState> State;
};

struct FRuntimeSurfelSnapshot;
struct FRuntimeTransportHistory
{
    TRefCountPtr<FRDGPooledBuffer> Queries;
    TRefCountPtr<FRDGPooledBuffer> IrradianceHistory;
    TSharedPtr<FRuntimeSurfelSnapshot,ESPMode::ThreadSafe> Surface;
    uint64 Bytes=0;
    int32 Rays=0;
    uint64 IrradianceBytes=0;
    int32 Iterations=0;
    float Relaxation=1;
    FVector3f Environment=FVector3f::ZeroVector;
    ~FRuntimeTransportHistory();
};
struct FRuntimeScene
{
    TSharedPtr<FMultiBounceData, ESPMode::ThreadSafe> Geometry;
    TArray<FVector4f> Materials; // 8x8 barycentric charts: reflectance, world normal
    TArray<FVector4f> Lights; // direction/position+type, radiance/intensity+radius
    TArray<FVector4f> Probes;
    TArray<FPRTGIRenderVolume> Volumes;
    TSharedPtr<FRuntimeSurfelSnapshot,ESPMode::ThreadSafe> SurfelSnapshot;
    TArray<FVector4f> SurfelBindings; // sorted BVH triangle -> first slot, N, count, owner
    int32 CacheIterations=5,CacheRays=64,CacheBatch=256,CacheBatchesPerSubmit=8;
    float CacheRelaxation=1.f;
    bool ReuseTransport=false;
    bool RegionalTransport=false;
    bool RegionalIrradiance=false;
    bool ProgressiveProbes=false;
    uint64 IrradianceBudgetBytes=16ull*1024*1024;
    FBox3f ChangedBounds{ForceInit};
    TArray<uint32> OldToNewTriangles;
    uint64 TransportBudgetBytes=64ull*1024*1024;
    // Isolated analytic fixtures only; live scene input stays black.
    FVector3f AnalyticEnvironment=FVector3f::ZeroVector;
    bool ReadCacheForTest=false;
    int32 Bounces = 6;
    int32 Rays = 256;
};

class FRuntimeGPU
{
public:
    FRuntimeGPU();
    ~FRuntimeGPU();
    void Submit(TSharedPtr<const FRuntimeScene, ESPMode::ThreadSafe> Scene, int32 Budget,
        TSharedPtr<FRuntimeTransportHistory,ESPMode::ThreadSafe> History=nullptr);
    TSharedPtr<FRuntimeTransportHistory,ESPMode::ThreadSafe> CaptureTransportHistory() const;
    // Schedules at most one bounded submission per tick. Publication is atomic for
    // the whole scene; a caller must reject obsolete layout/light generations.
    bool Tick(TArray<FVector4f>& Output, bool bAllowSubmission=true);
    bool HasPendingSubmission() const;
    bool TakePartial(TArray<FVector4f>& Output,int32& First);
    const TArray<FVector4f>& CacheResultForTest() const;
    FString Progress() const;
    // Call only after Tick has returned the completed probe output.
    void RetainCacheAndReleaseScratch();
private:
    struct FState;
    TSharedPtr<FState, ESPMode::ThreadSafe> State;
};
}
