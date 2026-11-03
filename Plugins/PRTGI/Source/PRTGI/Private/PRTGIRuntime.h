#pragma once
#include "CoreMinimal.h"
#include "PRTGIVolumeData.h"

class UWorld;
namespace PRTGI
{
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
    struct FState;
    TUniquePtr<FState> State;
};

struct FRuntimeSurfelSnapshot;
struct FRuntimeScene
{
    TSharedPtr<FMultiBounceData, ESPMode::ThreadSafe> Geometry;
    TArray<FVector4f> Materials; // 8x8 barycentric charts: reflectance, world normal
    TArray<FVector4f> Lights; // direction/position+type, radiance/intensity+radius
    TArray<FVector4f> Probes;
    TArray<FPRTGIRenderVolume> Volumes;
    TSharedPtr<FRuntimeSurfelSnapshot,ESPMode::ThreadSafe> SurfelSnapshot;
    TArray<FVector4f> SurfelBindings; // sorted BVH triangle -> first slot, N, count, owner
    int32 CacheIterations=5,CacheRays=64,CacheBatch=256;
    float CacheRelaxation=1.f;
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
    void Submit(TSharedPtr<const FRuntimeScene, ESPMode::ThreadSafe> Scene, int32 Budget);
    // Schedules at most one bounded batch per tick. Publication is atomic for
    // the whole scene; a caller must reject obsolete layout/light generations.
    bool Tick(TArray<FVector4f>& Output);
    const TArray<FVector4f>& CacheResultForTest() const;
    FString Progress() const;
    // Call only after Tick has returned the completed probe output.
    void RetainCacheAndReleaseScratch();
private:
    struct FState;
    TSharedPtr<FState, ESPMode::ThreadSafe> State;
};
}
