#pragma once
#include "CoreMinimal.h"
#include "RenderGraphResources.h"
struct FPRTGIProbeVisibility;
namespace PRTGI { struct FMultiBounceData; }

struct FPRTGIGPUCoefficients
{
    ~FPRTGIGPUCoefficients();
    // Immutable publication. Initialized on the render thread before the
    // release/acquire Ready flag makes it visible to the game thread.
    TRefCountPtr<FRDGPooledBuffer> Buffer;
};

struct FPRTGITransportData
{
    FIntVector GridCounts = FIntVector::ZeroValue;
    FVector3f GridMin = FVector3f::ZeroVector;
    FVector3f GridMax = FVector3f::ZeroVector;
    uint64 SceneSignature = 0;
    TArray<FVector4f> TransportCoefficients;
};

struct FPRTGIRenderVolume
{
    FIntVector GridCounts = FIntVector::ZeroValue;
    FVector3f GridMin = FVector3f::ZeroVector;
    FVector3f GridMax = FVector3f::ZeroVector;
    float ReceiverBoundsPadding = 0.0f;
    FVector3f ReceiverMin = FVector3f::ZeroVector;
    FVector3f ReceiverMax = FVector3f::ZeroVector;
    int32 BlendFace = 0;
    float BlendDistance = 0.0f;
    int32 Priority = 0;
    bool bTrilinear = false;
    // Runtime geometry must use current ray visibility instead of baked planes.
    bool bRuntimeGeometry = false;
    TSharedPtr<const FPRTGIProbeVisibility, ESPMode::ThreadSafe> Visibility;
    TSharedPtr<const PRTGI::FMultiBounceData, ESPMode::ThreadSafe> Geometry;
    TSharedPtr<const FPRTGIGPUCoefficients, ESPMode::ThreadSafe> GPUCoefficients;
    bool bCPUReferenceReady=true;
    TArray<FVector4f> Coefficients;
};
