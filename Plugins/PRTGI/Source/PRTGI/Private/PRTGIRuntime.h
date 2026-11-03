#pragma once
#include "CoreMinimal.h"
#include "PRTGIVolumeData.h"

class UWorld;
namespace PRTGI
{
// PC reference implementation for live transport. Asset material charts are
// prepared once; no scene transfer matrix or evaluated light is cached on disk.
// Triangle traversal deliberately provides a watertight reference for a future
// disk-surfel / radiance-cascade implementation.
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
    const TArray<FPRTGIRenderVolume>& GetVolumes() const;
private:
    struct FState;
    TUniquePtr<FState> State;
};

struct FRuntimeScene
{
    TSharedPtr<FMultiBounceData, ESPMode::ThreadSafe> Geometry;
    TArray<FVector4f> Materials; // 8x8 barycentric charts: reflectance, world normal
    TArray<FVector4f> Lights; // direction/position+type, radiance/intensity+radius
    TArray<FVector4f> Probes;
    TArray<FPRTGIRenderVolume> Volumes;
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
private:
    struct FState;
    TSharedPtr<FState, ESPMode::ThreadSafe> State;
};
}
