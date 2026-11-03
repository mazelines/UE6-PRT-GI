#pragma once
#include "CoreMinimal.h"
#include "RenderGraphResources.h"

namespace PRTGI
{
// Six float4s per local seed: position, shading normal, two centroid-relative
// corners (third is -A-B), reflectance, geometric normal. Asset data is shared.
struct FRuntimeSurfelAsset
{
    TArray<FVector4f> Seeds;
    TArray<FIntPoint> TriangleRanges; // local first seed, subdivision N (N*N seeds)
    int32 Num() const { return Seeds.Num()/6; }
};
struct FRuntimeSurfelInstance
{
    uint32 Owner=0; // stable, nonzero world-local ID; never reuse for a new object
    TSharedPtr<const FRuntimeSurfelAsset,ESPMode::ThreadSafe> Asset;
    FTransform Transform;
};
// Immutable GPU geometry snapshot for a lighting generation. The copy command
// precedes lighting work, so edits to the live pool cannot mix generations.
struct FRuntimeSurfelSnapshot
{
    TRefCountPtr<FRDGPooledBuffer> Buffer;
    TArray<uint32> ActiveSlots;
    uint32 Capacity=0;
    ~FRuntimeSurfelSnapshot();
};
// Stage one: persistent GPU surface pool and hashed multiresolution cell index.
// Surface data only; FRuntimeGPU owns lighting. Readback here is diagnostic only.
class FRuntimeSurfelPool
{
public:
    explicit FRuntimeSurfelPool(uint32 Capacity=65536,uint32 Buckets=16384,float BaseCellCm=32.f);
    ~FRuntimeSurfelPool();
    bool Update(TConstArrayView<FRuntimeSurfelInstance> Instances,FString& Error);
    FString Status() const;
    bool RequestAudit(FString& Error);
    // False while pending. Result contains a CPU oracle vs GPU pool/grid/query audit.
    bool PollAudit(FString& Result,bool& Passed);
    uint32 Revision() const;
    FVector4f TriangleBinding(uint32 Owner,int32 Triangle) const;
    TSharedPtr<FRuntimeSurfelSnapshot,ESPMode::ThreadSafe> CaptureLightingSnapshot() const;
private:
    struct FImpl;
    TUniquePtr<FImpl> Impl;
};
}
