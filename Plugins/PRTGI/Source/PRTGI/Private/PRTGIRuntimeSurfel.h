#pragma once
#include "CoreMinimal.h"

namespace PRTGI
{
// Six float4s per local seed: position, shading normal, two centroid-relative
// corners (third is -A-B), reflectance, geometric normal. Asset data is shared.
struct FRuntimeSurfelAsset
{
    TArray<FVector4f> Seeds;
    int32 Num() const { return Seeds.Num()/6; }
};
struct FRuntimeSurfelInstance
{
    uint32 Owner=0; // stable, nonzero world-local ID; never reuse for a new object
    TSharedPtr<const FRuntimeSurfelAsset,ESPMode::ThreadSafe> Asset;
    FTransform Transform;
};
// Stage one: persistent GPU surface pool and hashed multiresolution cell index.
// No irradiance cache or lighting replacement yet. Readback is diagnostic only.
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
private:
    struct FImpl;
    TUniquePtr<FImpl> Impl;
};
}
