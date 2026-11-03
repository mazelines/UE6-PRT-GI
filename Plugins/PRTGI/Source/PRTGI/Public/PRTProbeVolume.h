#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "PRTProbeVolume.generated.h"

class UBoxComponent;
class UInstancedStaticMeshComponent;
class UMaterialInstanceDynamic;
class UTexture2D;

UENUM(BlueprintType)
enum class EPRTProbeDisplay : uint8 { Hidden, Positions, Lighting };

/** 独立 PRT 预计算体积，轴对齐；不需要全局 Volume。 */
UCLASS(BlueprintType, meta=(DisplayName="PRT Probe Volume", ToolTip="An independent, axis-aligned PRT precomputation volume. A global volume is not required."))
class PRTGI_API APRTProbeVolume : public AActor
{
    GENERATED_BODY()
public:
    APRTProbeVolume();
    virtual void PostActorCreated() override;
    virtual void PostLoad() override;
    virtual void PostDuplicate(EDuplicateMode::Type DuplicateMode) override;
    virtual void OnConstruction(const FTransform& Transform) override;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="PRT|Volume")
    TObjectPtr<UBoxComponent> VolumeBounds;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="PRT|Volume")
    bool bEnabled = true;
    // Explicit transport membership, independent of receiver bounds. Shared
    // walls and external blockers may belong to more than one room.
    UPROPERTY(EditInstanceOnly, BlueprintReadWrite, Category="PRT|Runtime Room", meta=(ToolTip="Used when r.PRTGI.Runtime.Rooms=1. Include actors whose static mesh components participate in this room's transport. Empty membership does not fall back to the whole world."))
    TArray<TObjectPtr<AActor>> RuntimeIncludedActors;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="PRT|Runtime Room", meta=(ToolTip="Include actors or static mesh components with any of these tags, in addition to Included Actors. PRTGI.Ignore excludes a component. ISM/HISM membership covers the entire component."))
    TArray<FName> RuntimeParticipantTags;
    /** 半尺寸（cm），也可通过 Actor Scale 调整范围。第一版不支持旋转。 */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="PRT|Volume", meta=(ToolTip="Half-size in cm. Actor Scale also affects the bounds. Rotation is not supported.", ClampMin="1", Units="cm"))
    FVector Extent = FVector(200.0);
    /** 目标最大间距，自动均匀填满内缩后的边界。 */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="PRT|Volume", meta=(ToolTip="Target maximum probe spacing. The grid uniformly fills the inset bounds.", ClampMin="1", Units="cm"))
    FVector ProbeSpacing = FVector(100.0);
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="PRT|Volume", meta=(ClampMin="0", Units="cm"))
    FVector ProbeInset = FVector(5.0);
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="PRT|Volume", meta=(ClampMin="-100000", ClampMax="100000"))
    int32 Priority = 0;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="PRT|Build", meta=(ClampMin="0.01", ClampMax="10", Units="cm"))
    float NearClipCm = 1.0f;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="PRT|Build", meta=(ClampMin="4", ClampMax="32"))
    int32 CaptureResolution = 16;
    /** 静态表面聚合尺寸；越小越准确，缓存和 GPU 工作量越大。 */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="PRT|Build", meta=(ToolTip="Static surface aggregation size. Smaller values increase detail, cache size, and GPU work.", ClampMin="1", ClampMax="100", Units="cm"))
    float TransportCellSize = 2.0f;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="PRT|Build", meta=(ToolTip="After the first four bounces, stop paths whose maximum RGB throughput falls below this value. 0 disables this biased truncation; rebuild transport after changing it. This is not a lighting-error bound.", ClampMin="0", ClampMax="0.25"))
    float TransportMinThroughput = .01f;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="PRT|Build", meta=(ToolTip="Production paths per captured sample (average when adaptive). 1 preserves legacy sampling. Changing this requires rebuilding transport.", ClampMin="1", ClampMax="16"))
    int32 TransportPathsPerSample = 1;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="PRT|Build", meta=(ToolTip="Experimental light-independent allocation from two separate pilot paths per capture sample. Pilots add build cost and are discarded; production weights are normalized by each sample's path count. Requires Paths Per Sample >= 2 and a transport rebuild."))
    bool bAdaptiveTransportSampling = false;
    /** 0=无过渡，1/2=-/+X，3/4=-/+Y，5/6=-/+Z。仅在开口设置过渡。 */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="PRT|Blending", meta=(ToolTip="0=none, 1/2=-/+X, 3/4=-/+Y, 5/6=-/+Z. Configure transitions only at openings.", ClampMin="0", ClampMax="6"))
    int32 BlendFace = 0;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="PRT|Blending", meta=(ClampMin="0", Units="cm"))
    float BlendDistance = 0.0f;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="PRT|Display")
    EPRTProbeDisplay DisplayMode = EPRTProbeDisplay::Lighting;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="PRT|Display", meta=(ClampMin="0.5", ClampMax="50", Units="cm"))
    float ProbeRadius = 3.0f;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="PRT|Status", Transient)
    FIntVector ProbeCounts = FIntVector::ZeroValue;
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="PRT|Status", Transient)
    FVector ActualSpacing = FVector::ZeroVector;
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="PRT|Status", Transient)
    int32 ValidProbes = 0;
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="PRT|Status", Transient)
    int32 InvalidProbes = 0;
    /** 本次构建的总进度；跨几何采集和传输预计算连续增长，保存成功后才为 1。 */
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="PRT|Build", Transient, meta=(ToolTip="Overall progress across geometry capture and transport precomputation. Reaches 1 only after saving succeeds.", DisplayName="Build Progress", ClampMin="0", ClampMax="1"))
    float BuildProgress = 0.0f;
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="PRT|Build", Transient, meta=(DisplayName="Build Stage"))
    FString BuildStage = TEXT("Not built");
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="PRT|Status", Transient)
    FString CacheStatus = TEXT("Not built");
    /** 来自几何缓存的持久化校验摘要；异常 Probe 已从传输与插值排除。 */
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="PRT|Status", Transient, meta=(ToolTip="Audit summary restored from the geometry cache. Rejected probes are excluded from transport and interpolation.", DisplayName="Build Warnings"))
    FString BuildWarnings;
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="PRT|Status", Transient)
    bool bGIActive = false;
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="PRT|Status", Transient)
    int32 EffectiveBounces = 0;
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="PRT|Status", Transient)
    int32 TransportSources = 0;
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="PRT|Status", Transient)
    FString CacheFile;
    UPROPERTY(VisibleAnywhere, Category="PRT|Identity", AdvancedDisplay)
    FGuid VolumeId;

    // 迁移时仅只读使用原缓存；新构建始终写 Volume 自己的 GUID 文件。
    UPROPERTY(VisibleAnywhere, Category="PRT|Legacy Import", AdvancedDisplay)
    FString LegacyCacheBase;
    UPROPERTY(VisibleAnywhere, Category="PRT|Legacy Import", AdvancedDisplay)
    bool bLegacyGlobal = false;
    UPROPERTY(VisibleAnywhere, Category="PRT|Legacy Import", AdvancedDisplay)
    FVector LegacyGridMin = FVector::ZeroVector;
    UPROPERTY(VisibleAnywhere, Category="PRT|Legacy Import", AdvancedDisplay)
    FVector LegacyGridMax = FVector::ZeroVector;
    UPROPERTY(VisibleAnywhere, Category="PRT|Legacy Import", AdvancedDisplay)
    FIntVector LegacyCounts = FIntVector::ZeroValue;

    UFUNCTION(CallInEditor, BlueprintCallable, Category="PRT|Build", meta=(DisplayName="Build Precomputation"))
    void BuildPrecomputation();
    UFUNCTION(CallInEditor, BlueprintCallable, Category="PRT|Build", meta=(DisplayName="Cancel Build"))
    void CancelBuild();

    bool GetLayout(FBox& Grid, FBox& Receiver, FIntVector& Counts, FString& Error) const;
    FString GetCachePath() const;
    void UpdateProbeSpheres(const FBox& Grid, FIntVector Counts, const TArray<FVector4f>& SH,
        const TArray<float>& Validity, bool bShow);
    void ClearProbeSpheres();

private:
    UPROPERTY(Transient, DuplicateTransient) TObjectPtr<UInstancedStaticMeshComponent> ProbeSpheres;
    UPROPERTY(Transient, DuplicateTransient) TObjectPtr<UMaterialInstanceDynamic> ProbeMaterial;
    UPROPERTY(Transient, DuplicateTransient) TObjectPtr<UTexture2D> ProbeSHTexture;
};
