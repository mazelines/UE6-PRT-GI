#include "PRTGI.h"

#include "PRTGIMath.h"
#include "PRTGISurfels.h"
#include "PRTGIProbeVisibility.h"
#include "PRTProbeVolume.h"
#include "PRTGIVolumeData.h"
#include "PRTGIVolumeManager.h"
#include "PRTGIMultiBounce.h"
#include "PRTGIRuntime.h"
#include "PRTGISettings.h"
#include "PRTGIViewExtension.h"
#include "PostProcess/PostProcessInputs.h"

#include "Components/DirectionalLightComponent.h"
#include "Components/LineBatchComponent.h"
#include "Components/SceneCaptureComponentCube.h"
#include "Components/StaticMeshComponent.h"
#include "Containers/Ticker.h"
#include "DeferredShadingRenderer.h"
#include "Engine/DirectionalLight.h"
#include "Engine/Engine.h"
#include "Engine/StaticMesh.h"
#include "Engine/TextureRenderTargetCube.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Framework/Application/IInputProcessor.h"
#include "Framework/Application/SlateApplication.h"
#include "GlobalShader.h"
#include "GameFramework/Pawn.h"
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "Interfaces/IPluginManager.h"
#include "Input/Events.h"
#include "InputCoreTypes.h"
#include "Materials/MaterialInterface.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "RenderingThread.h"
#include "SceneRendering.h"
#include "Serialization/MemoryReader.h"
#include "Serialization/MemoryWriter.h"
#include "ShaderCore.h"
#include "ShaderParameterStruct.h"
#include "TextureResource.h"
#include "UObject/UObjectIterator.h"
#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#endif

DEFINE_LOG_CATEGORY_STATIC(LogPRTGI, Log, All);

namespace
{
    TAutoConsoleVariable<int32> CVarPRTRuntimeVisibilityAnyHit(TEXT("r.PRTGI.Runtime.VisibilityAnyHit"),1,
        TEXT("Runtime probe visibility: 0 closest-hit reference, 1 stop at the first blocker. Same binary visibility; contact distances remain closest-hit."),ECVF_RenderThreadSafe);
    TAutoConsoleVariable<int32> CVarPRTRuntimeResolveHalfResolution(TEXT("r.PRTGI.Runtime.ResolveHalfResolution"),0,
        TEXT("Experimental runtime-only irradiance gather: 0 full resolution, 1 half resolution with depth/normal/volume checks and full-resolution fallback. Material response and contact occlusion stay full resolution. No temporal history."),ECVF_RenderThreadSafe);
    TAutoConsoleVariable<int32> CVarPRTProbeVisibility(TEXT("r.PRTGI.ProbeVisibility"),1,
        TEXT("Probe visibility: 0 disabled, 1 first-hit planes, 2 depth moments, 3 exact static BVH segments (higher cost)."),ECVF_RenderThreadSafe);
    TAutoConsoleVariable<float> CVarPRTVisibilityBias(TEXT("r.PRTGI.VisibilityBiasCm"),1.f,
        TEXT("First-hit plane / depth moment receiver bias in cm. Keep fixed for controlled A/B captures."),ECVF_RenderThreadSafe);
    TAutoConsoleVariable<float> CVarPRTHemisphereTransition(TEXT("r.PRTGI.HemisphereTransition"),.2f,
        TEXT("Positive-hemisphere smooth transition as a fraction of the smallest probe spacing (0..0.5, default 0.2). 0 restores the legacy hard gate and can show rings on curved receivers. No rebake required."),ECVF_RenderThreadSafe);
    TAutoConsoleVariable<int32> CVarPRTDebug(TEXT("r.PRTGI.Debug"),0,
        TEXT("0 lighting, 1 visibility acceptance, 2 interpolation support, 3 hemisphere acceptance, 4 contact visibility, 5 accepted probes / 16, 6 effective probes / 16, 7 one minus dominant normalized weight. 5-7 show the selected volume before boundary blending; magenta=legacy trilinear. Red=low support/high concentration."),ECVF_RenderThreadSafe);
    TAutoConsoleVariable<int32> CVarPRTContactHalfResolution(TEXT("r.PRTGI.ContactHalfResolution"),0,
        TEXT("Experimental contact occlusion: 0 full-resolution reference, 1 half-resolution trace with depth/normal-aware reconstruction and full-resolution edge fallback. No history and no rebake."),ECVF_RenderThreadSafe);
    TAutoConsoleVariable<int32> CVarPRTContactSamples(TEXT("r.PRTGI.ContactSamples"),0,
        TEXT("Short-range static diffuse contact occlusion rays per pixel, 0 disables, maximum 16."),ECVF_RenderThreadSafe);
    TAutoConsoleVariable<float> CVarPRTContactRadius(TEXT("r.PRTGI.ContactRadiusCm"),20.f,
        TEXT("Maximum range of the diffuse contact correction in cm."),ECVF_RenderThreadSafe);
    TAutoConsoleVariable<float> CVarPRTContactBias(TEXT("r.PRTGI.ContactBiasCm"),.1f,
        TEXT("Contact ray origin and minimum intersection distance in cm."),ECVF_RenderThreadSafe);
    constexpr uint32 PRTGIDataMagic = 0x49475450; // "PTGI"
    constexpr int32 PRTGIDataVersion = 2;
    constexpr int32 PRTGIMaxProbeCount = 32 * 32 * 32;
    constexpr int32 PRTGITransportCoefficientCount =
        PRTGI::Math::CoefficientCount * PRTGI::Math::CoefficientCount;

    class FScopedCaptureConsoleInt
    {
    public:
        FScopedCaptureConsoleInt(const TCHAR* Name, int32 Value)
            : Variable(IConsoleManager::Get().FindConsoleVariable(Name))
        {
            if (Variable)
            {
                Previous=Variable->GetInt();
                Priority=static_cast<EConsoleVariableFlags>(Variable->GetFlags() & ECVF_SetByMask);
                Variable->Set(Value,Priority);
            }
        }
        ~FScopedCaptureConsoleInt() { if (Variable) Variable->Set(Previous,Priority); }
        FScopedCaptureConsoleInt(const FScopedCaptureConsoleInt&) = delete;
        FScopedCaptureConsoleInt& operator=(const FScopedCaptureConsoleInt&) = delete;
    private:
        IConsoleVariable* Variable=nullptr;
        int32 Previous=0;
        EConsoleVariableFlags Priority=ECVF_SetByConstructor;
    };

    static TAutoConsoleVariable<int32> CVarPRTGILocalVolumes(
        TEXT("r.PRTGI.LocalVolumes"), 1,
        TEXT("0: global probes only; 1: prefer ready local refinements. Does not change bake data."),
        ECVF_RenderThreadSafe);

    static TAutoConsoleVariable<int32> CVarPRTGIShowProbes(
        TEXT("r.PRTGI.ShowProbes"), 0,
        TEXT("Probe markers: 0 off, 1 all, 2 local only, 3 global only. Cyan=global, orange=local, red=invalid, gray=no cache."),
        ECVF_Default);
    static TAutoConsoleVariable<float> CVarPRTGIProbeRadius(
        TEXT("r.PRTGI.ProbeRadius"), 3.0f, TEXT("Probe marker radius in cm (0.5 to 50)."), ECVF_Default);
    static TAutoConsoleVariable<int32> CVarPRTGIProbeXRay(
        TEXT("r.PRTGI.ProbeXRay"), 0, TEXT("1: show probe markers through walls, including invalid probes inside solids."), ECVF_Default);

    static TAutoConsoleVariable<int32> CVarPRTGISurfel(
        TEXT("r.PRTGI.Surfel"), 1,
        TEXT("Prefer valid local Surfel caches (CPU one-bounce correctness backend). 0 uses directional matrices."),
        ECVF_Default);

    static TAutoConsoleVariable<int32> CVarPRTGIEnable(
        TEXT("r.PRTGI.Enable"),
        1,
        TEXT("Enable the PRTGI diffuse indirect pass. Requires the Plugin GI method (use PRTGI.SetMode PRT)."),
        ECVF_RenderThreadSafe);

    static TAutoConsoleVariable<float> CVarPRTGIIntensity(
        TEXT("r.PRTGI.Intensity"),
        1.0f,
        TEXT("PRTGI indirect diffuse intensity multiplier."),
        ECVF_RenderThreadSafe);

    static TAutoConsoleVariable<int32> CVarPRTGIIndirectOnly(
        TEXT("r.PRTGI.IndirectOnly"), 0,
        TEXT("1: display only PRT indirect diffuse before post processing. Requires PRT GI mode."),
        ECVF_RenderThreadSafe);

    static TAutoConsoleVariable<int32> CVarPRTGIAutoRelight(
        TEXT("r.PRTGI.AutoRelight"),
        1,
        TEXT("Evaluate directional transport when the directional light direction, color, or intensity changes."),
        ECVF_Default);

    struct FPRTGILightState
    {
        FVector3f Direction = FVector3f::ZeroVector;
        FVector3f Color = FVector3f::ZeroVector;
        float Intensity = 0.0f;
        float IndirectLightingIntensity = 1.0f;

        bool IsNearlyEqual(const FPRTGILightState& Other) const
        {
            const float IntensityTolerance = FMath::Max(1.0f, FMath::Max(FMath::Abs(Intensity), FMath::Abs(Other.Intensity)) * 1.0e-5f);
            return Direction.Equals(Other.Direction, 1.0e-4f)
                && Color.Equals(Other.Color, 1.0e-4f)
                && FMath::IsNearlyEqual(Intensity, Other.Intensity, IntensityTolerance)
                && FMath::IsNearlyEqual(IndirectLightingIntensity, Other.IndirectLightingIntensity, 1.0e-4f);
        }
    };

    struct FPRTGIRenderData : FPRTGIRenderVolume
    {
        const FScene* Scene = nullptr;
        TArray<FPRTGIRenderVolume> LocalVolumes;
    };

    struct FPRTGIMobilityRestoreState
    {
        TWeakObjectPtr<USceneComponent> Component;
        EComponentMobility::Type Mobility = EComponentMobility::Movable;
    };

    BEGIN_SHADER_PARAMETER_STRUCT(FPRTGIApplyParameters, )
        SHADER_PARAMETER_RDG_TEXTURE(Texture2D, NormalTexture)
        SHADER_PARAMETER_RDG_TEXTURE(Texture2D, BaseColorTexture)
        SHADER_PARAMETER_RDG_TEXTURE(Texture2D, MetallicTexture)
        SHADER_PARAMETER_RDG_TEXTURE(Texture2D, DepthTexture)
        SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, ProbeCoefficients)
        SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, ProbeVolumes)
        SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, ProbeVisibilityPlanes)
        SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float2>, ProbeDepthMoments)
        SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, ProbeVisibilityVolumes)
        SHADER_PARAMETER(uint32, ProbeVisibilityEnabled)
        SHADER_PARAMETER(uint32, RuntimeVisibilityAnyHit)
        SHADER_PARAMETER(uint32, RuntimeResolveHalfResolution)
        SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float4>, RuntimeIrradianceTexture)
        SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, RuntimeIrradianceOutput)
        SHADER_PARAMETER(float, VisibilityBiasCm)
        SHADER_PARAMETER(float, HemisphereTransition)
        SHADER_PARAMETER(uint32, DebugMode)
        SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, ContactTriangles)
        SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, ContactNodes)
        SHADER_PARAMETER(uint32, ContactNodeCount)
        SHADER_PARAMETER(uint32, ContactSamples)
        SHADER_PARAMETER(float, ContactRadius)
        SHADER_PARAMETER(float, ContactBias)
        SHADER_PARAMETER(FVector3f, ContactWorldTranslation)
        SHADER_PARAMETER(uint32, ContactHalfResolution)
        SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float4>, ContactTexture)
        SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, ContactOutput)
        SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, SceneColorOutput)
        SHADER_PARAMETER_SAMPLER(SamplerState, PointClampSampler)
        SHADER_PARAMETER(uint32, VolumeCount)
        SHADER_PARAMETER(FIntPoint, ViewRectMin)
        SHADER_PARAMETER(FIntPoint, ViewSize)
        SHADER_PARAMETER(float, IndirectIntensity)
        SHADER_PARAMETER(uint32, ShouldUsePreExposure)
        SHADER_PARAMETER(uint32, OverwriteSceneColor)
        SHADER_PARAMETER(uint32, HasTransport)
        SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, View)
    END_SHADER_PARAMETER_STRUCT()

    class FPRTGIApplyCS : public FGlobalShader
    {
    public:
        DECLARE_GLOBAL_SHADER(FPRTGIApplyCS);
        SHADER_USE_PARAMETER_STRUCT(FPRTGIApplyCS, FGlobalShader);

        using FParameters = FPRTGIApplyParameters;

        static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
        {
            return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM6);
        }

        static void ModifyCompilationEnvironment(
            const FGlobalShaderPermutationParameters& Parameters,
            FShaderCompilerEnvironment& OutEnvironment)
        {
            FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
            OutEnvironment.CompilerFlags.Add(CFLAG_AllowTypedUAVLoads);
        }
    };

    IMPLEMENT_GLOBAL_SHADER(FPRTGIApplyCS, "/Plugin/PRTGI/Private/PRTGIApply.usf", "MainCS", SF_Compute);

    class FPRTGIContactCS : public FGlobalShader
    {
    public:
        DECLARE_GLOBAL_SHADER(FPRTGIContactCS);
        SHADER_USE_PARAMETER_STRUCT(FPRTGIContactCS,FGlobalShader);
        using FParameters=FPRTGIApplyParameters;
        static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
        { return IsFeatureLevelSupported(Parameters.Platform,ERHIFeatureLevel::SM6); }
    };
    IMPLEMENT_GLOBAL_SHADER(FPRTGIContactCS,"/Plugin/PRTGI/Private/PRTGIApply.usf","ContactCS",SF_Compute);

    class FPRTGIRuntimeResolveCS : public FGlobalShader
    {
    public:
        DECLARE_GLOBAL_SHADER(FPRTGIRuntimeResolveCS);
        SHADER_USE_PARAMETER_STRUCT(FPRTGIRuntimeResolveCS,FGlobalShader);
        using FParameters=FPRTGIApplyParameters;
        static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
        { return IsFeatureLevelSupported(Parameters.Platform,ERHIFeatureLevel::SM6); }
    };
    IMPLEMENT_GLOBAL_SHADER(FPRTGIRuntimeResolveCS,"/Plugin/PRTGI/Private/PRTGIApply.usf","RuntimeResolveCS",SF_Compute);

    class FPRTGIPublishCS : public FGlobalShader
    {
        DECLARE_GLOBAL_SHADER(FPRTGIPublishCS);
        SHADER_USE_PARAMETER_STRUCT(FPRTGIPublishCS,FGlobalShader);
        BEGIN_SHADER_PARAMETER_STRUCT(FParameters,)
            SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>,SourceSH)
            SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<float4>,DestinationSH)
            SHADER_PARAMETER(uint32,CoefficientCount)
            SHADER_PARAMETER(uint32,DestinationOffset)
        END_SHADER_PARAMETER_STRUCT()
    };
    IMPLEMENT_GLOBAL_SHADER(FPRTGIPublishCS,"/Plugin/PRTGI/Private/PRTGIPublish.usf","PublishCS",SF_Compute);

    ADirectionalLight* FindPrimaryDirectionalLight(UWorld* World)
    {
        if (!World)
        {
            return nullptr;
        }

        ADirectionalLight* Fallback = nullptr;
        for (TActorIterator<ADirectionalLight> It(World); It; ++It)
        {
            ADirectionalLight* Light = *It;
            if (!IsValid(Light) || !IsValid(Light->GetLightComponent()))
            {
                continue;
            }
            if (Light->ActorHasTag(TEXT("PRTGI.PrimaryLight")))
            {
                return Light;
            }
            if (!Fallback && Light->GetLightComponent()->IsVisible())
            {
                Fallback = Light;
            }
        }
        return Fallback;
    }

    FPRTGILightState ReadLightState(const ADirectionalLight& Light)
    {
        const ULightComponent* Component = Light.GetLightComponent();
        FLinearColor Color = Component ? Component->GetLightColor() : FLinearColor::Black;
        if (Component && Component->bUseTemperature)
        {
            // The renderer multiplies the authored light color by the black-body
            // temperature tint in ULightComponent::GetColoredLightBrightness().
            Color *= Component->GetColorTemperature();
        }
        FPRTGILightState State;
        State.Direction = FVector3f(Light.GetActorForwardVector());
        State.Color = FVector3f(Color.R, Color.G, Color.B);
        State.Intensity = Component && Component->IsVisible() && Component->bAffectsWorld
            ? Component->Intensity * Component->DiffuseScale
            : 0.0f;
        State.IndirectLightingIntensity = Component ? Component->IndirectLightingIntensity : 1.0f;
        return State;
    }

    UWorld* FindPreferredWorld()
    {
        if (!GEngine)
        {
            return nullptr;
        }

        UWorld* EditorWorld = nullptr;
        for (const FWorldContext& Context : GEngine->GetWorldContexts())
        {
            UWorld* World = Context.World();
            if (!World)
            {
                continue;
            }
            if (Context.WorldType == EWorldType::PIE || Context.WorldType == EWorldType::Game)
            {
                return World;
            }
            if (Context.WorldType == EWorldType::Editor)
            {
                EditorWorld = World;
            }
        }
        return EditorWorld;
    }

    bool IsRuntimeInputWorld(const UWorld* World)
    {
        return World
            && (World->WorldType == EWorldType::PIE || World->WorldType == EWorldType::Game);
    }

    class FPRTGIRuntimeInputProcessor final : public IInputProcessor
    {
    public:
        virtual void Tick(
            const float DeltaTime,
            FSlateApplication& SlateApp,
            TSharedRef<ICursor> Cursor) override
        {
        }

        virtual bool HandleKeyDownEvent(
            FSlateApplication& SlateApp,
            const FKeyEvent& KeyEvent) override
        {
            if (!IsRuntimeInputWorld(FindPreferredWorld())
                || !KeyEvent.IsControlDown()
                || !KeyEvent.IsAltDown())
            {
                return false;
            }

            const FKey Key = KeyEvent.GetKey();
            const bool bShiftDown = KeyEvent.IsShiftDown();
            const bool bSupportedShortcut = Key == EKeys::LeftBracket
                || Key == EKeys::RightBracket
                || (!bShiftDown && (Key == EKeys::One
                    || Key == EKeys::Two
                    || Key == EKeys::Three
                    || Key == EKeys::Four
                    || Key == EKeys::P
                    || Key == EKeys::G
                    || Key == EKeys::B));
            if (KeyEvent.IsRepeat())
            {
                return bSupportedShortcut;
            }

            bool bHandled = true;

            if (!bShiftDown && Key == EKeys::One)
            {
                PRTGI::SetMode(PRTGI::EMode::Offline);
            }
            else if (!bShiftDown && Key == EKeys::Two)
            {
                PRTGI::SetMode(PRTGI::EMode::PRT);
            }
            else if (!bShiftDown && Key == EKeys::Three)
            {
                PRTGI::SetMode(PRTGI::EMode::Lumen);
            }
            else if (!bShiftDown && Key == EKeys::Four)
            {
                PRTGI::ToggleIndirectOnly();
            }
            else if (!bShiftDown && Key == EKeys::P)
            {
                PRTGI::ToggleProbes();
            }
            else if (!bShiftDown && Key == EKeys::G)
            {
                PRTGI::CycleBounces();
            }
            else if (Key == EKeys::LeftBracket)
            {
                const float Step = GetDefault<UPRTGISettings>()->RotationStepDegrees;
                PRTGI::RotatePrimaryDirectionalLight(
                    bShiftDown ? 0.0f : -Step,
                    bShiftDown ? -Step : 0.0f);
            }
            else if (Key == EKeys::RightBracket)
            {
                const float Step = GetDefault<UPRTGISettings>()->RotationStepDegrees;
                PRTGI::RotatePrimaryDirectionalLight(
                    bShiftDown ? 0.0f : Step,
                    bShiftDown ? Step : 0.0f);
            }
            else if (!bShiftDown && Key == EKeys::B)
            {
                PRTGI::RequestBake();
            }
            else
            {
                bHandled = false;
            }

            return bHandled;
        }

        virtual const TCHAR* GetDebugName() const override
        {
            return TEXT("PRTGI Runtime Shortcuts");
        }
    };

    TSharedPtr<IInputProcessor> GRuntimeInputProcessor;
    TSharedPtr<FPRTGIViewExtension, ESPMode::ThreadSafe> GPRTGIViewExtension;

    bool IsPRTGIMode()
    {
        // 每个 Editor tick 都会查询；缓存指针可避免 ConsoleManager 的高频查找警告。
        static const IConsoleVariable* Method =
            IConsoleManager::Get().FindConsoleVariable(TEXT("r.DynamicGlobalIlluminationMethod"));
        return Method && Method->GetInt() == static_cast<int32>(EDynamicGlobalIlluminationMethod::Plugin)
            && CVarPRTGIEnable.GetValueOnGameThread() != 0;
    }

    bool IsCapturedStaticMeshComponent(
        const UStaticMeshComponent* Component,
        const UWorld* World)
    {
        return IsValid(Component)
            && Component->GetWorld() == World
            && Component->IsRegistered()
            && Component->IsVisible()
            && !(Component->GetOwner() && Component->GetOwner()->IsA<APawn>())
            && !(Component->GetOwner() && Component->GetOwner()->IsA<APRTProbeVolume>())
            && IsValid(Component->GetStaticMesh());
    }

    bool IsProbeGeometryComponent(
        const UStaticMeshComponent* Component,
        const UWorld* World,
        const UPRTGISettings* Settings)
    {
        if (!IsCapturedStaticMeshComponent(Component, World))
        {
            return false;
        }

        const FString AssetPath = Component->GetStaticMesh()->GetPathName();
        return Settings->GeometryAssetPathPrefix.IsEmpty()
            || AssetPath.StartsWith(Settings->GeometryAssetPathPrefix);
    }

    class FStableHash64
    {
    public:

        template <typename ValueType>
        void AddValue(const ValueType& InValue)
        {
            AddBytes(&InValue, sizeof(ValueType));
        }

        void AddString(const FString& Value)
        {
            const FTCHARToUTF8 Utf8(*Value);
            const int32 Length = Utf8.Length();
            AddValue(Length);
            AddBytes(Utf8.Get(), Length);
        }

        void AddGuid(const FGuid& Guid)
        {
            AddValue(Guid.A);
            AddValue(Guid.B);
            AddValue(Guid.C);
            AddValue(Guid.D);
        }

        uint64 Get() const
        {
            return Hash;
        }

    private:
        void AddBytes(const void* Data, const int64 NumBytes)
        {
            const uint8* Bytes = static_cast<const uint8*>(Data);
            for (int64 Index = 0; Index < NumBytes; ++Index)
            {
                Hash ^= Bytes[Index];
                Hash *= 1099511628211ull;
            }
        }

        uint64 Hash = 1469598103934665603ull;
    };

    uint64 ComputeSceneSignature(
        UWorld* World,
        const ADirectionalLight* Light,
        const FBox& Bounds,
        const FIntVector& GridCounts,
        bool bLocalVolume = false,
        bool bGeometryOnly = false,
        float NearClipOverride = -1.0f,
        int32 ResolutionOverride = -1)
    {
        if (!World || (!Light && !bGeometryOnly))
        {
            return 0;
        }

        const UPRTGISettings* Settings = GetDefault<UPRTGISettings>();
        FStableHash64 Hash;
        Hash.AddString(TEXT("PRTGI.CaptureNearClip.v1"));
        Hash.AddValue(NearClipOverride > 0 ? NearClipOverride : Settings->GetCaptureNearClipCm());
        // Local caches additionally encode collision-based probe rejection.
        if (bLocalVolume) Hash.AddString(TEXT("PRTGI.LocalVolume.Validity.v1"));
        Hash.AddString(Settings->GeometryAssetPathPrefix);
        Hash.AddValue(GridCounts.X);
        Hash.AddValue(GridCounts.Y);
        Hash.AddValue(GridCounts.Z);
        if (bGeometryOnly)
        {
            Hash.AddString(TEXT("PRTGI.Surfel.GBuffer.RawDiffuseColor.Float32.v4"));
            Hash.AddValue(ResolutionOverride > 0 ? ResolutionOverride : FMath::Clamp(Settings->SurfelCaptureResolution, 4, 32));
        }
        else
        {
            Hash.AddString(TEXT("PRTGI.Radiance.SeparateCubeFaces.v1"));
            Hash.AddValue(Settings->CaptureResolution);
            Hash.AddValue(Settings->DirectionalSampleCount);
            Hash.AddValue(Settings->TransportReferenceIntensity);
        }
        Hash.AddValue(Bounds.Min.X);
        Hash.AddValue(Bounds.Min.Y);
        Hash.AddValue(Bounds.Min.Z);
        Hash.AddValue(Bounds.Max.X);
        Hash.AddValue(Bounds.Max.Y);
        Hash.AddValue(Bounds.Max.Z);

        const UDirectionalLightComponent* DirectionalComponent =
            Light && !bGeometryOnly ? Cast<UDirectionalLightComponent>(Light->GetLightComponent()) : nullptr;
        if (DirectionalComponent)
        {
            Hash.AddValue(DirectionalComponent->LightSourceAngle);
            Hash.AddValue(DirectionalComponent->LightSourceSoftAngle);
            Hash.AddValue(static_cast<uint8>(DirectionalComponent->CastShadows));
            Hash.AddValue(static_cast<uint8>(DirectionalComponent->CastStaticShadows));
            Hash.AddValue(static_cast<uint8>(DirectionalComponent->CastDynamicShadows));
            Hash.AddValue(static_cast<uint8>(DirectionalComponent->bAffectTranslucentLighting));
            Hash.AddValue(static_cast<uint8>(DirectionalComponent->bTransmission));
            Hash.AddValue(DirectionalComponent->ShadowResolutionScale);
            Hash.AddValue(DirectionalComponent->ShadowBias);
            Hash.AddValue(DirectionalComponent->ShadowSlopeBias);
            Hash.AddValue(DirectionalComponent->ShadowSharpen);
            Hash.AddValue(DirectionalComponent->ContactShadowLength);
            Hash.AddValue(static_cast<uint8>(DirectionalComponent->ContactShadowLengthInWS));
            Hash.AddValue(DirectionalComponent->ContactShadowCastingIntensity);
            Hash.AddValue(DirectionalComponent->ContactShadowNonCastingIntensity);
            Hash.AddValue(static_cast<uint8>(DirectionalComponent->LightingChannels.bChannel0));
            Hash.AddValue(static_cast<uint8>(DirectionalComponent->LightingChannels.bChannel1));
            Hash.AddValue(static_cast<uint8>(DirectionalComponent->LightingChannels.bChannel2));

            Hash.AddString(DirectionalComponent->LightFunctionMaterial
                ? DirectionalComponent->LightFunctionMaterial->GetPathName()
                : FString());
            if (DirectionalComponent->LightFunctionMaterial)
            {
                TArray<FGuid> LightFunctionGuids;
                DirectionalComponent->LightFunctionMaterial->GetLightingGuidChain(
                    true,
                    LightFunctionGuids);
                Hash.AddValue(LightFunctionGuids.Num());
                for (const FGuid& Guid : LightFunctionGuids)
                {
                    Hash.AddGuid(Guid);
                }
            }
            const FVector LightFunctionScale = DirectionalComponent->LightFunctionScale;
            Hash.AddValue(LightFunctionScale.X);
            Hash.AddValue(LightFunctionScale.Y);
            Hash.AddValue(LightFunctionScale.Z);
            Hash.AddValue(DirectionalComponent->LightFunctionFadeDistance);
            Hash.AddValue(DirectionalComponent->DisabledBrightness);

            Hash.AddValue(DirectionalComponent->ShadowCascadeBiasDistribution);
            Hash.AddValue(DirectionalComponent->DynamicShadowDistanceMovableLight);
            Hash.AddValue(DirectionalComponent->DynamicShadowCascades);
            Hash.AddValue(DirectionalComponent->CascadeDistributionExponent);
            Hash.AddValue(DirectionalComponent->CascadeTransitionFraction);
            Hash.AddValue(DirectionalComponent->ShadowDistanceFadeoutFraction);
            Hash.AddValue(DirectionalComponent->FarShadowCascadeCount);
            Hash.AddValue(DirectionalComponent->FarShadowDistance);
            Hash.AddValue(DirectionalComponent->DistanceFieldShadowDistance);
            Hash.AddValue(DirectionalComponent->TraceDistance);
        }

        TArray<UStaticMeshComponent*> Components;
        for (TObjectIterator<UStaticMeshComponent> It; It; ++It)
        {
            // SceneCapture renders the complete scene. GeometryAssetPathPrefix only
            // chooses the probe volume, so every visible StaticMesh affecting the
            // captured radiance must participate in cache invalidation.
            if (IsCapturedStaticMeshComponent(*It, World))
            {
                Components.Add(*It);
            }
        }
        Components.Sort([](const UStaticMeshComponent& A, const UStaticMeshComponent& B)
        {
            return UWorld::RemovePIEPrefix(A.GetPathName()) < UWorld::RemovePIEPrefix(B.GetPathName());
        });

        Hash.AddValue(Components.Num());
        for (const UStaticMeshComponent* Component : Components)
        {
            Hash.AddString(UWorld::RemovePIEPrefix(Component->GetPathName(World)));
            Hash.AddString(Component->GetStaticMesh()->GetPathName());
            Hash.AddGuid(Component->GetStaticMesh()->GetLightingGuid());

            const FTransform Transform = Component->GetComponentTransform();
            const FVector Location = Transform.GetLocation();
            const FQuat Rotation = Transform.GetRotation();
            const FVector Scale = Transform.GetScale3D();
            Hash.AddValue(Location.X);
            Hash.AddValue(Location.Y);
            Hash.AddValue(Location.Z);
            Hash.AddValue(Rotation.X);
            Hash.AddValue(Rotation.Y);
            Hash.AddValue(Rotation.Z);
            Hash.AddValue(Rotation.W);
            Hash.AddValue(Scale.X);
            Hash.AddValue(Scale.Y);
            Hash.AddValue(Scale.Z);

            const int32 MaterialCount = Component->GetNumMaterials();
            Hash.AddValue(MaterialCount);
            for (int32 MaterialIndex = 0; MaterialIndex < MaterialCount; ++MaterialIndex)
            {
                const UMaterialInterface* Material = Component->GetMaterial(MaterialIndex);
                Hash.AddString(Material ? Material->GetPathName() : FString());
                if (Material)
                {
                    TArray<FGuid> LightingGuids;
                    Material->GetLightingGuidChain(true, LightingGuids);
                    Hash.AddValue(LightingGuids.Num());
                    for (const FGuid& Guid : LightingGuids)
                    {
                        Hash.AddGuid(Guid);
                    }
                }
            }
        }
        return Hash.Get();
    }
}

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPRTGICaptureConsoleScopeTest,"PRTGI.Capture.ConsoleScope",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FPRTGICaptureConsoleScopeTest::RunTest(const FString&)
{
    auto* Variable=IConsoleManager::Get().FindConsoleVariable(TEXT("r.SceneCapture.CubeSinglePass"));
    if (!TestNotNull(TEXT("Cube capture switch exists"),Variable)) return false;
    const int32 Value=Variable->GetInt();
    const uint32 Flags=Variable->GetFlags();
    {
        FScopedCaptureConsoleInt Guard(TEXT("r.SceneCapture.CubeSinglePass"),Value?0:1);
        TestEqual(TEXT("Scoped override takes effect even at constructor priority"),Variable->GetInt(),Value?0:1);
    }
    TestEqual(TEXT("Capture switch value restored"),Variable->GetInt(),Value);
    TestEqual(TEXT("Capture switch priority restored"),Variable->GetFlags(),Flags);
    return !HasAnyErrors();
}

bool PRTGI::RunCacheIdentityChecks(FAutomationTestBase& Test)
{
    UWorld* EditorWorld = nullptr;
    UWorld* PlayWorld = nullptr;
    if (GEngine)
    {
        for (const FWorldContext& Context : GEngine->GetWorldContexts())
        {
            if (Context.WorldType == EWorldType::Editor) EditorWorld = Context.World();
            if (Context.WorldType == EWorldType::PIE) PlayWorld = Context.World();
        }
    }
    if (!EditorWorld || !PlayWorld)
    {
        Test.AddError(TEXT("Start an unmodified PIE session before running this integration test."));
        return false;
    }
    const FBox Bounds(FVector(-1000.0), FVector(1000.0));
    const FIntVector Counts = GetDefault<UPRTGISettings>()->GetGridCounts(EditorWorld);
    ADirectionalLight* EditorLight = FindPrimaryDirectionalLight(EditorWorld);
    ADirectionalLight* PlayLight = FindPrimaryDirectionalLight(PlayWorld);
    if (!EditorLight || !PlayLight)
    {
        Test.AddError(TEXT("Both worlds must contain a directional light."));
        return false;
    }
    const uint64 Expected = ComputeSceneSignature(EditorWorld, EditorLight, Bounds, Counts);
    Test.TestEqual(TEXT("PIE sublevel prefixes and DefaultPawn do not invalidate the map cache"),
        ComputeSceneSignature(PlayWorld, PlayLight, Bounds, Counts), Expected);
    UPRTGISettings* MutableSettings=GetMutableDefault<UPRTGISettings>();
    const uint64 GeometrySignature=ComputeSceneSignature(EditorWorld,nullptr,Bounds,Counts,true,true);
    Test.TestEqual(TEXT("Surfel geometry identity is stable across PIE"),
        ComputeSceneSignature(PlayWorld,nullptr,Bounds,Counts,true,true),GeometrySignature);
    {
        TGuardValue<float> ClipGuard(MutableSettings->CaptureNearClipCm,MutableSettings->CaptureNearClipCm==0.5f?1.0f:0.5f);
        Test.TestNotEqual(TEXT("Near clip invalidates directional capture cache"),
            ComputeSceneSignature(EditorWorld,EditorLight,Bounds,Counts),Expected);
        Test.TestNotEqual(TEXT("Near clip invalidates Surfel geometry cache"),
            ComputeSceneSignature(EditorWorld,nullptr,Bounds,Counts,true,true),GeometrySignature);
    }
    {
        TGuardValue<int32> ResolutionGuard(MutableSettings->SurfelCaptureResolution,MutableSettings->SurfelCaptureResolution==8?16:8);
        Test.TestNotEqual(TEXT("G-buffer resolution invalidates Surfel cache"),
            ComputeSceneSignature(EditorWorld,nullptr,Bounds,Counts,true,true),GeometrySignature);
        Test.TestEqual(TEXT("G-buffer resolution leaves directional cache intact"),
            ComputeSceneSignature(EditorWorld,EditorLight,Bounds,Counts),Expected);
    }

    bool bTestedGeometry = false;
    for (TObjectIterator<UStaticMeshComponent> It; It; ++It)
    {
        if (IsCapturedStaticMeshComponent(*It, PlayWorld))
        {
            It->SetVisibility(false);
            Test.TestNotEqual(TEXT("Real scene visibility changes still invalidate cache"),
                ComputeSceneSignature(PlayWorld, PlayLight, Bounds, Counts), Expected);
            It->SetVisibility(true);
            Test.TestEqual(TEXT("Restoring geometry restores the signature"),
                ComputeSceneSignature(PlayWorld, PlayLight, Bounds, Counts), Expected);
            bTestedGeometry = true;
            break;
        }
    }
    Test.TestTrue(TEXT("Exercised actual level geometry"), bTestedGeometry);
    return !Test.HasAnyErrors();
}
#endif

class FPRTGIModule final : public IModuleInterface
{
public:
    virtual void StartupModule() override
    {
        RuntimeGI = MakeUnique<PRTGI::FRuntimeGI>();
        ActorVolumes = MakeUnique<FPRTGIVolumeManager>(
            [](UWorld* World, ADirectionalLight* Light, const FBox& Bounds, FIntVector Counts, bool Local, bool Geometry, float Near, int32 Resolution)
            { return ComputeSceneSignature(World,Light,Bounds,Counts,Local,Geometry,Near,Resolution); },
            [this](const FString& Path, FIntVector Counts) { return ReadDataFile(Path,Counts); });
        const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("PRTGI"));
        check(Plugin.IsValid());
        AddShaderSourceDirectoryMapping(TEXT("/Plugin/PRTGI"), FPaths::Combine(Plugin->GetBaseDir(), TEXT("Shaders")));

        RenderDelegateHandle = FGlobalIlluminationPluginDelegates::RenderDiffuseIndirectLight().AddRaw(
            this, &FPRTGIModule::RenderDiffuseIndirectLight_RenderThread, false);
        TickHandle = FTSTicker::GetCoreTicker().AddTicker(
            FTickerDelegate::CreateRaw(this, &FPRTGIModule::Tick));
        WorldCleanupHandle = FWorldDelegates::OnWorldCleanup.AddRaw(
            this, &FPRTGIModule::HandleWorldCleanup);

        RegisterConsoleCommands();
        GPRTGIModule = this;
        UE_LOG(LogPRTGI, Log, TEXT("PRTGI runtime started. Use PRTGI.SetMode PRT, then select a PRT Probe Volume and click Build Precomputation."));
    }

    virtual void ShutdownModule() override
    {
        RuntimeGI.Reset();
        ActorVolumes.Reset();
        ClearProbeVisualization();
        SurfelBake.Reset(); LocalSurfels.Reset();
        RestoreMapOverrides();
        GPRTGIModule = nullptr;
        GPRTGIViewExtension.Reset();
        FlushRenderingCommands();
        if (WorldCleanupHandle.IsValid())
        {
            FWorldDelegates::OnWorldCleanup.Remove(WorldCleanupHandle);
            WorldCleanupHandle.Reset();
        }
        if (GRuntimeInputProcessor.IsValid() && FSlateApplication::IsInitialized())
        {
            FSlateApplication::Get().UnregisterInputPreProcessor(GRuntimeInputProcessor);
        }
        GRuntimeInputProcessor.Reset();
        UnregisterConsoleCommands();
        if (TickHandle.IsValid())
        {
            FTSTicker::GetCoreTicker().RemoveTicker(TickHandle);
            TickHandle.Reset();
        }
        if (RenderDelegateHandle.IsValid())
        {
            // Ensure no render-thread broadcast is still executing this raw delegate
            // before removing it during Live Coding or dynamic module unload.
            FlushRenderingCommands();
            FGlobalIlluminationPluginDelegates::RenderDiffuseIndirectLight().Remove(RenderDelegateHandle);
            RenderDelegateHandle.Reset();
        }
        FinishBakeLightOwnership();
        DestroyCaptureObjects();
        FlushRenderingCommands();
        RenderData_RenderThread.Reset();
        PublishedRenderData_GameThread.Reset();
        PublishedTransport_GameThread.Reset();
    }

    void SetMode(const PRTGI::EMode Mode)
    {
        IConsoleVariable* DynamicGIMethod = IConsoleManager::Get().FindConsoleVariable(TEXT("r.DynamicGlobalIlluminationMethod"));
        if (!DynamicGIMethod)
        {
            UE_LOG(LogPRTGI, Error, TEXT("Cannot find r.DynamicGlobalIlluminationMethod."));
            return;
        }

        // An explicit mode selection always returns to the normal beauty view.
        CVarPRTGIIndirectOnly->Set(0, ECVF_SetByConsole);

        switch (Mode)
        {
        case PRTGI::EMode::Offline:
            CVarPRTGIEnable->Set(0, ECVF_SetByConsole);
            DynamicGIMethod->Set(static_cast<int32>(EDynamicGlobalIlluminationMethod::None), ECVF_SetByConsole);
            break;
        case PRTGI::EMode::PRT:
        {
            CVarPRTGIEnable->Set(1, ECVF_SetByConsole);
            DynamicGIMethod->Set(static_cast<int32>(EDynamicGlobalIlluminationMethod::Plugin), ECVF_SetByConsole);
            if (PublishedTransport_GameThread.IsValid())
            {
                UWorld* World = FindPreferredWorld();
                RelightFromTransport(World, FindPrimaryDirectionalLight(World));
            }
            else if (GetDefault<UPRTGISettings>()->bAutoBakeMissingCache)
            {
                RequestBake();
            }
            break;
        }
        case PRTGI::EMode::Lumen:
            CVarPRTGIEnable->Set(0, ECVF_SetByConsole);
            DynamicGIMethod->Set(static_cast<int32>(EDynamicGlobalIlluminationMethod::Lumen), ECVF_SetByConsole);
            break;
        }

        UE_LOG(LogPRTGI, Display, TEXT("GI mode switched to %s"),
            Mode == PRTGI::EMode::Offline ? TEXT("Offline/Lightmass") :
            Mode == PRTGI::EMode::PRT ? TEXT("PRT") : TEXT("Lumen"));
    }

    void RequestBake()
    {
        if (ActorVolumes && ActorVolumes->IsBaking())
        { UE_LOG(LogPRTGI,Warning,TEXT("Finish/cancel Actor volume builds before starting the legacy baker.")); return; }
        if (BakeState == EBakeState::Baking)
        {
            CancelBake(TEXT("New rebuild request received"));
        }
        ActiveBakeLocalIndex = INDEX_NONE;
        bBakeRequested = true;
        BakeState = EBakeState::WaitingForStableLight;
        UE_LOG(LogPRTGI, Display, TEXT("Directional PRT transport rebuild requested."));
    }

    const FPRTGILocalVolume* GetLocalVolume(UWorld* World, int32 Index) const
    {
        const FPRTGIMapProfile* Profile = GetDefault<UPRTGISettings>()->FindMapProfile(World);
        if (!Profile || !Profile->LocalVolumes.IsValidIndex(Index)) return nullptr;
        const FPRTGILocalVolume& Volume = Profile->LocalVolumes[Index];
        if (!Volume.IsValid() || Volume.DataFile.Equals(Profile->DataFile, ESearchCase::IgnoreCase)) return nullptr;
        const UPRTGISettings* Settings = GetDefault<UPRTGISettings>();
        if (FPaths::IsSamePath(Volume.DataFile, Settings->DataFile)) return nullptr;
        for (const auto& OtherProfile : Settings->MapProfiles)
        {
            if (FPaths::IsSamePath(Volume.DataFile, OtherProfile.DataFile)) return nullptr;
            for (const auto& OtherVolume : OtherProfile.LocalVolumes)
                if (&OtherVolume != &Volume && FPaths::IsSamePath(Volume.DataFile, OtherVolume.DataFile)) return nullptr;
        }
        for (int32 Other = 0; Other < Profile->LocalVolumes.Num(); ++Other)
            if (Other != Index && Volume.DataFile.Equals(Profile->LocalVolumes[Other].DataFile, ESearchCase::IgnoreCase)) return nullptr;
        return &Volume;
    }

    void RequestLocalBake(int32 Index)
    {
        if (ActorVolumes && ActorVolumes->IsBaking()) return;
        UWorld* World = FindPreferredWorld();
        if (!GetLocalVolume(World, Index))
        {
            UE_LOG(LogPRTGI, Error, TEXT("Invalid local volume %d (bounds/grid/cache paths must be valid and distinct)."), Index);
            return;
        }
        if (!PublishedTransport_GameThread.IsValid())
        {
            UE_LOG(LogPRTGI, Error, TEXT("Bake/load the global transport before baking a refinement."));
            return;
        }
        if (BakeState == EBakeState::Baking) CancelBake(TEXT("Local rebuild requested"));
        ActiveBakeLocalIndex = Index;
        bBakeRequested = true;
        BakeState = EBakeState::WaitingForStableLight;
        UE_LOG(LogPRTGI, Display, TEXT("Requested local volume %d bake; global cache is retained."), Index);
    }

    void RotatePrimaryDirectionalLight(const float YawDeltaDegrees, const float PitchDeltaDegrees)
    {
        UWorld* World = FindPreferredWorld();
        ADirectionalLight* Light = FindPrimaryDirectionalLight(World);
        if (!Light)
        {
            UE_LOG(LogPRTGI, Warning, TEXT("No DirectionalLight available in the current world."));
            return;
        }

        if (BakeState == EBakeState::Baking)
        {
            CancelBake(TEXT("Directional light adjusted by shortcut"));
        }

#if WITH_EDITOR
        Light->Modify();
        Light->GetLightComponent()->Modify();
#endif
        Light->GetLightComponent()->SetMobility(EComponentMobility::Movable);
        FRotator Rotation = Light->GetActorRotation();
        Rotation.Yaw += YawDeltaDegrees;
        Rotation.Pitch += PitchDeltaDegrees;
        Rotation.Normalize();
        Light->SetActorRotation(Rotation);
        Light->GetLightComponent()->MarkRenderStateDirty();
        LastObservedLightState = ReadLightState(*Light);
        bHasObservedLight = true;

        if (PublishedTransport_GameThread.IsValid()
            && CVarPRTGIAutoRelight.GetValueOnGameThread() != 0 && !TActorIterator<APRTProbeVolume>(World))
        {
            RelightFromTransport(World, Light);
        }
        else if (!PublishedTransport_GameThread.IsValid() && GetDefault<UPRTGISettings>()->bAutoBakeMissingCache
            && (!ActorVolumes || ActorVolumes->GetReadyCount()==0))
        {
            RequestBake();
        }

        UE_LOG(LogPRTGI, Display, TEXT("DirectionalLight rotated to Pitch=%.2f Yaw=%.2f Roll=%.2f."),
            Rotation.Pitch, Rotation.Yaw, Rotation.Roll);
    }

    FString GetStatus() const
    {
        const TCHAR* StateText = TEXT("Idle");
        switch (BakeState)
        {
        case EBakeState::WaitingForStableLight: StateText = TEXT("WaitingForStableLight"); break;
        case EBakeState::Baking: StateText = TEXT("Baking"); break;
        case EBakeState::Ready: StateText = TEXT("Ready"); break;
        case EBakeState::Error: StateText = TEXT("Error"); break;
        default: break;
        }

        const int32 ProbeCount = ActiveGridCounts.X * ActiveGridCounts.Y * ActiveGridCounts.Z;
        const int32 Total = ProbeCount > 0 ? ProbeCount * (ActiveDirectionalSampleCount + 1) : 0;
        return FString::Printf(
            TEXT("Mode=%s State=%s Transport=%s Progress=%d/%d Samples=%d Grid=%dx%dx%d Bounds=[%s]-[%s] File=%s LocalReady=%d/%d BakeLocal=%d%s SurfelReady=%d/%d SurfelBake=%d Backend=%s NearClip=%.3fcm ShowProbes=%d ProbeMarkers=%d/%d/%d/%d(global/local/invalid/unknown) ActorVolumesReady=%d ActorBuilding=%d"),
            IsPRTGIMode() ? TEXT("PRT") : TEXT("Other"),
            StateText,
            PublishedTransport_GameThread.IsValid() ? TEXT("Ready") : TEXT("Missing"),
            NextBakeJob,
            Total,
            ActiveDirectionalSampleCount,
            ActiveGridCounts.X,
            ActiveGridCounts.Y,
            ActiveGridCounts.Z,
            *ActiveBounds.Min.ToCompactString(),
            *ActiveBounds.Max.ToCompactString(),
            *GetBakeDataPath(FindPreferredWorld()),
            LocalTransports.FilterByPredicate([](const auto& Item) { return Item.IsValid(); }).Num(),
            LocalTransports.Num(), ActiveBakeLocalIndex,
            LastError.IsEmpty() ? TEXT("") : *FString::Printf(TEXT(" Error=%s"), *LastError),
            LocalSurfels.FilterByPredicate([](const auto& Item) { return Item.IsValid(); }).Num(),
            LocalSurfels.Num(), SurfelBakeIndex, CVarPRTGISurfel.GetValueOnGameThread() ? TEXT("SurfelLocal/MatrixFallback") : TEXT("Matrix"),
            GetDefault<UPRTGISettings>()->GetCaptureNearClipCm(), CVarPRTGIShowProbes.GetValueOnGameThread(),
            DebugGlobalProbes, DebugLocalProbes, DebugInvalidProbes, DebugUnknownProbes,
            ActorVolumes ? ActorVolumes->GetReadyCount() : 0, ActorVolumes && ActorVolumes->IsBaking() ? 1 : 0);
    }

private:
    enum class EBakeState : uint8
    {
        Idle,
        WaitingForStableLight,
        Baking,
        Ready,
        Error
    };

    void HandleWorldCleanup(UWorld* World, bool bSessionEnded, bool bCleanupResources)
    {
        if (ActorVolumeWorld.Get()==World)
        {
            RuntimeGI->Reset();
            bWasRuntimeGI=false;
            ActorVolumes->Reset(); ActorVolumeWorld.Reset();
            ENQUEUE_RENDER_COMMAND(PRTGIClearActorVolumes)([this](FRHICommandListImmediate&)
            { ActorRenderVolumes.Reset(); ActorRenderScene=nullptr; VisibilitySources_RenderThread.Reset(); VisibilityBuffer_RenderThread.SafeRelease(); DepthMomentsBuffer_RenderThread.SafeRelease();
              ContactGeometry_RenderThread.Reset();ContactTriangles_RenderThread.SafeRelease();ContactNodes_RenderThread.SafeRelease(); });
        }
        if (ProbeVisualization && ProbeVisualization->GetWorld() == World) ClearProbeVisualization();
        if (SurfelBake && SurfelBakeWorld.Get()==World)
        {
            SurfelBake.Reset(); SurfelBakeIndex=INDEX_NONE; SurfelBakeWorld.Reset();
        }
        if (!World)
        {
            return;
        }

        if (ActiveWorld.Get() == World
            || (CaptureComponent && CaptureComponent->GetWorld() == World))
        {
            CancelBake(TEXT("Active world is shutting down"));
            ActiveWorld.Reset();
        }

        if (ValidatedWorld.Get() == World)
        {
            ValidatedWorld.Reset();
        }

        if (PublishedRenderData_GameThread.IsValid()
            && World->Scene
            && PublishedRenderData_GameThread->Scene == World->Scene->GetRenderScene())
        {
            PublishedRenderData_GameThread.Reset();
            ENQUEUE_RENDER_COMMAND(PRTGIClearWorldRenderData)(
                [this](FRHICommandListImmediate& RHICmdList)
                {
                    RenderData_RenderThread.Reset();
                });
        }
    }

    bool Tick(float DeltaSeconds)
    {
        if (!GPRTGIViewExtension.IsValid() && GEngine)
        {
            GPRTGIViewExtension = FSceneViewExtensions::NewExtension<FPRTGIViewExtension>(
                [this](FRDGBuilder& GraphBuilder, const FSceneView& InView, const FPostProcessingInputs& Inputs)
                {
                    if (CVarPRTGIIndirectOnly.GetValueOnRenderThread() == 0
                        || InView.FinalPostProcessSettings.DynamicGlobalIlluminationMethod != EDynamicGlobalIlluminationMethod::Plugin
                        || !InView.Family || !InView.Family->Scene || !Inputs.SceneTextures)
                    {
                        return;
                    }
                    const FViewInfo& View = static_cast<const FViewInfo&>(InView);
                    const auto& Textures = *Inputs.SceneTextures;
                    FGlobalIlluminationPluginResources Resources;
                    Resources.GBufferA = Textures->GBufferATexture;
                    Resources.GBufferB = Textures->GBufferBTexture;
                    Resources.GBufferC = Textures->GBufferCTexture;
                    Resources.SceneDepthZ = Textures->SceneDepthTexture;
                    Resources.SceneColor = Textures->SceneColorTexture;
                    RenderDiffuseIndirectLight_RenderThread(
                        *InView.Family->Scene->GetRenderScene(), View, GraphBuilder, Resources, true);
                });
        }

        if (!GRuntimeInputProcessor.IsValid() && FSlateApplication::IsInitialized())
        {
            GRuntimeInputProcessor = MakeShared<FPRTGIRuntimeInputProcessor>();
            FSlateApplication::Get().RegisterInputPreProcessor(GRuntimeInputProcessor);
            UE_LOG(LogPRTGI, Log, TEXT("PIE/Game runtime shortcuts registered."));
        }

        UWorld* World = FindPreferredWorld();
        if (!World || !World->Scene || IsRunningCommandlet())
        {
            ClearProbeVisualization();
            return true;
        }
        const FString MapPackage = UWorld::RemovePIEPrefix(World->GetOutermost()->GetName());
        if (LoadedMapPackage != MapPackage)
        {
            SurfelBake.Reset(); SurfelBakeIndex = INDEX_NONE;
            if (BakeState == EBakeState::Baking) CancelBake(TEXT("Switching evaluation map"));
            ClearPublishedTransport();
            bBakeRequested = false;
            bHasObservedLight = false;
            BakeState = EBakeState::Idle;
            RestoreMapOverrides();
            LoadedMapPackage = MapPackage;
            const UPRTGISettings* Settings = GetDefault<UPRTGISettings>();
            const FPRTGIMapProfile* Profile = Settings->FindMapProfile(World);
            LoadedDataFile = Profile ? Profile->DataFile : Settings->DataFile;
            ActiveBakeLocalIndex = INDEX_NONE;
            if (Profile && Profile->bHardwareLumenComparison)
            {
                ApplyMapOverride(TEXT("r.Lumen.HardwareRayTracing"), 1);
                ApplyMapOverride(TEXT("r.Lumen.ScreenProbeGather.ScreenTraces"), 0);
                ApplyMapOverride(TEXT("r.EyeAdaptation.CachedLightingPreExposure"), 8);
            }
            LoadData(World);
            LoadLocalVolumes(World);
        }

        ADirectionalLight* Light = FindPrimaryDirectionalLight(World);
        const double Now = FPlatformTime::Seconds();
        const bool bActorMode = bool(TActorIterator<APRTProbeVolume>(World));

        const bool bRuntime=PRTGI::FRuntimeGI::IsEnabled() && IsPRTGIMode()
            && CVarPRTGIEnable.GetValueOnGameThread()!=0;
        if(bRuntime)
        {
            if(!bWasRuntimeGI)
            {
                ActorVolumes->Reset();
                ClearProbeVisualization();
                for(TActorIterator<APRTProbeVolume> It(World);It;++It)It->ClearProbeSpheres();
            }
            const bool Changed=RuntimeGI->Tick(World);
            if(Changed || !bWasRuntimeGI)
            {
                ActorVolumeWorld=World;
                auto Volumes=RuntimeGI->GetVolumes();const FScene* Scene=World->Scene->GetRenderScene();
                ENQUEUE_RENDER_COMMAND(PRTGIPublishRuntime)([this,Volumes=MoveTemp(Volumes),Scene](FRHICommandListImmediate&) mutable
                { ActorRenderVolumes=MoveTemp(Volumes);ActorRenderScene=Scene;bActorRenderMode=true; });
            }
            bWasRuntimeGI=true;
            return true;
        }
        if(bWasRuntimeGI)
        {
            bWasRuntimeGI=false;
            ActorVolumes->Reset();
            ENQUEUE_RENDER_COMMAND(PRTGIClearRuntime)([this](FRHICommandListImmediate&)
            { ActorRenderVolumes.Reset();ActorRenderScene=nullptr;bActorRenderMode=false; });
        }

        if (PublishedTransport_GameThread.IsValid()
            && BakeState != EBakeState::Baking
            && World
            && Light
            && (ValidatedWorld.Get() != World || Now >= NextSignatureCheckSeconds))
        {
            FBox CurrentBounds(EForceInit::ForceInit);
            const UPRTGISettings* Settings = GetDefault<UPRTGISettings>();
            const FIntVector ConfiguredGridCounts = Settings->GetGridCounts(World);
            const bool bHasBounds = ComputeProbeBounds(World, CurrentBounds);
            const uint64 CurrentSignature = bHasBounds
                ? ComputeSceneSignature(World, Light, CurrentBounds, ConfiguredGridCounts)
                : 0;
            if (!bHasBounds || CurrentSignature != PublishedTransport_GameThread->SceneSignature)
            {
                UE_LOG(LogPRTGI, Warning,
                    TEXT("Directional PRT cache is stale; rebuild required (cached=0x%016llx current=0x%016llx)."),
                    static_cast<unsigned long long>(PublishedTransport_GameThread->SceneSignature),
                    static_cast<unsigned long long>(CurrentSignature));
                ClearPublishedTransport();
                BakeState = EBakeState::Idle;
                if (IsPRTGIMode() && GetDefault<UPRTGISettings>()->bAutoBakeMissingCache)
                {
                    bBakeRequested = true;
                    BakeState = EBakeState::WaitingForStableLight;
                }
            }
            else
            {
                ValidateLocalVolumes(World, Light);
                ValidatedWorld = World;
                NextSignatureCheckSeconds = Now + 2.0;
            }
        }

        if (Light && BakeState != EBakeState::Baking)
        {
            const FPRTGILightState CurrentLightState = ReadLightState(*Light);
            const bool bLightChanged = !bHasObservedLight
                || !CurrentLightState.IsNearlyEqual(LastObservedLightState);
            if (bLightChanged)
            {
                LastObservedLightState = CurrentLightState;
                bHasObservedLight = true;
            }

            if (PublishedTransport_GameThread.IsValid()
                && CVarPRTGIAutoRelight.GetValueOnGameThread() != 0
                && !bActorMode
                && IsPRTGIMode()
                && (bLightChanged
                    || !PublishedLightState.IsNearlyEqual(CurrentLightState)
                    || PublishedSurfelBackend != CVarPRTGISurfel.GetValueOnGameThread()
                    || PublishedSurfelBias != GetDefault<UPRTGISettings>()->SurfelShadowBiasCm
                    || !PublishedRenderData_GameThread.IsValid()
                    || PublishedRenderData_GameThread->Scene != World->Scene->GetRenderScene()))
            {
                RelightFromTransport(World, Light);
            }
        }

        if (ActorVolumes->Tick(World, Light, CVarPRTGIShowProbes.GetValueOnGameThread(),
            BakeState==EBakeState::Baking || bBakeRequested || SurfelBake.IsValid(),
            IsPRTGIMode() && CVarPRTGIEnable.GetValueOnGameThread()!=0))
        {
            ActorVolumeWorld=World;
            auto Volumes=ActorVolumes->GetRenderVolumes();
            const FScene* Scene=World->Scene->GetRenderScene();
            ENQUEUE_RENDER_COMMAND(PRTGIPublishActorVolumes)([this, Volumes=MoveTemp(Volumes), Scene, bActorMode](FRHICommandListImmediate&) mutable
            { ActorRenderVolumes=MoveTemp(Volumes); ActorRenderScene=Scene; bActorRenderMode=bActorMode; });
        }
        TickProbeVisualization(World);
        TickSurfelBake(World, Light);

        // Expensive automatic baking is opt-in; invalid caches otherwise remain disabled.
        if (!PublishedTransport_GameThread.IsValid()
            && !bActorMode
            && GetDefault<UPRTGISettings>()->bAutoBakeMissingCache
            && IsPRTGIMode()
            && !bBakeRequested
            && BakeState == EBakeState::Idle)
        {
            bBakeRequested = true;
            BakeState = EBakeState::WaitingForStableLight;
        }

        if (bBakeRequested && BakeState != EBakeState::Baking)
        {
            StartBake(World, Light);
        }

        if (BakeState == EBakeState::Baking)
        {
            const int32 ProbeBudget = FMath::Max(1, GetDefault<UPRTGISettings>()->ProbesPerTick);
            for (int32 Probe = 0; Probe < ProbeBudget && BakeState == EBakeState::Baking; ++Probe)
            {
                BakeOneTransportCapture();
            }
        }
        return true;
    }

    void ClearProbeVisualization()
    {
        if (ProbeVisualization)
        {
            ProbeVisualization->RemoveFromRoot();
            ProbeVisualization->DestroyComponent();
            ProbeVisualization = nullptr;
        }
        NextProbeVisualizationSeconds = 0.0;
        DebugGlobalProbes = DebugLocalProbes = DebugInvalidProbes = DebugUnknownProbes = 0;
    }

    void TickProbeVisualization(UWorld* World)
    {
        if (TActorIterator<APRTProbeVolume>(World)) { ClearProbeVisualization(); return; }
        const int32 Mode = FMath::Clamp(CVarPRTGIShowProbes.GetValueOnGameThread(), 0, 3);
        if (ProbeVisualization && ProbeVisualization->GetWorld() != World) ClearProbeVisualization();
        if (!Mode)
        {
            ClearProbeVisualization();
            return;
        }
        const double Now = FPlatformTime::Seconds();
        if (Now < NextProbeVisualizationSeconds) return;
        NextProbeVisualizationSeconds = Now + 0.5;
        if (!ProbeVisualization)
        {
            // Owned separately from the world's debug lines: toggling off must not
            // clear another tool's drawings. Never serialize or duplicate into PIE.
            ProbeVisualization = NewObject<ULineBatchComponent>(World, NAME_None, RF_Transient | RF_DuplicateTransient);
            ProbeVisualization->AddToRoot();
            ProbeVisualization->SetCollisionEnabled(ECollisionEnabled::NoCollision);
            ProbeVisualization->SetCastShadow(false);
            ProbeVisualization->SetHiddenInSceneCapture(true);
            ProbeVisualization->bCalculateAccurateBounds = true;
            ProbeVisualization->RegisterComponentWithWorld(World);
        }
        ProbeVisualization->Flush();
        DebugGlobalProbes = DebugLocalProbes = DebugInvalidProbes = DebugUnknownProbes = 0;
        TArray<FBatchedLine> Lines;
        const float ConfiguredRadius = CVarPRTGIProbeRadius.GetValueOnGameThread();
        const float Radius = FMath::IsFinite(ConfiguredRadius) ? FMath::Clamp(ConfiguredRadius, 0.5f, 50.0f) : 3.0f;
        const uint8 Depth = CVarPRTGIProbeXRay.GetValueOnGameThread() ? SDPG_Foreground : SDPG_World;
        auto AppendGrid = [&](FIntVector Counts, FVector Min, FVector Max, bool bLocal,
            const FPRTGITransportData* Matrix, const PRTGI::FSurfelVolume* Surfels)
        {
            if (Counts.GetMin() < 2 || Counts.GetMax() > 32) return;
            const int32 Count = Counts.X * Counts.Y * Counts.Z;
            // Missing data remains gray. Do not perform new collision queries or
            // mistake a configured grid for a usable GI cache.
            const bool bMatrixReady = Matrix && Matrix->GridCounts == Counts
                && Matrix->GridMin.Equals(FVector3f(Min), 0.001f) && Matrix->GridMax.Equals(FVector3f(Max), 0.001f)
                && Matrix->TransportCoefficients.Num() == Count * PRTGITransportCoefficientCount;
            const bool bSurfelReady = Surfels && Surfels->GridCounts == Counts
                && Surfels->GridMin.Equals(FVector3f(Min), 0.001f) && Surfels->GridMax.Equals(FVector3f(Max), 0.001f)
                && Surfels->Validity.Num() == Count;
            for (int32 Probe = 0; Probe < Count; ++Probe)
            {
                const bool bKnown = bSurfelReady || bMatrixReady;
                const bool bInvalid = bSurfelReady ? Surfels->Validity[Probe] < 0.0f
                    : bLocal && bMatrixReady && Matrix->TransportCoefficients[Probe * PRTGITransportCoefficientCount].W < 0.0f;
                const FLinearColor Color = !bKnown ? FLinearColor(0.4f, 0.4f, 0.4f)
                    : bInvalid ? FLinearColor::Red : bLocal ? FLinearColor(1.0f, 0.35f, 0.0f) : FLinearColor(0.0f, 0.8f, 1.0f);
                if (!bKnown) ++DebugUnknownProbes;
                else if (bInvalid) ++DebugInvalidProbes;
                else if (bLocal) ++DebugLocalProbes;
                else ++DebugGlobalProbes;
                const FVector Alpha(double(Probe % Counts.X) / (Counts.X - 1),
                    double((Probe / Counts.X) % Counts.Y) / (Counts.Y - 1),
                    double(Probe / (Counts.X * Counts.Y)) / (Counts.Z - 1));
                const FVector Position = Min + Alpha * (Max - Min);
                // Three great circles form a small wire sphere; batch all markers
                // in one upload instead of issuing a render update per segment.
                for (int32 Plane = 0; Plane < 3; ++Plane)
                {
                    for (int32 Segment = 0; Segment < 12; ++Segment)
                    {
                        auto Point = [&](int32 S)
                        {
                            FVector Offset = FVector::ZeroVector;
                            Offset[Plane] = Radius * FMath::Cos(2.0 * UE_PI * S / 12.0);
                            Offset[(Plane + 1) % 3] = Radius * FMath::Sin(2.0 * UE_PI * S / 12.0);
                            return Position + Offset;
                        };
                        Lines.Emplace(Point(Segment), Point(Segment + 1), Color, -1.0f, 1.0f, Depth);
                    }
                }
            }
        };
        const UPRTGISettings* Settings = GetDefault<UPRTGISettings>();
        FBox Bounds(EForceInit::ForceInit);
        if (Mode != 2 && ComputeProbeBounds(World, Bounds))
            AppendGrid(Settings->GetGridCounts(World), Bounds.Min, Bounds.Max, false, PublishedTransport_GameThread.Get(), nullptr);
        if (Mode != 3)
        {
            if (const auto* Profile = Settings->FindMapProfile(World))
            {
                for (int32 Index = 0; Index < Profile->LocalVolumes.Num(); ++Index)
                {
                    const auto* Local = GetLocalVolume(World, Index);
                    if (!Local) continue;
                    const auto* Matrix = LocalTransports.IsValidIndex(Index) ? LocalTransports[Index].Get() : nullptr;
                    const auto* Surfels = CVarPRTGISurfel.GetValueOnGameThread() && LocalSurfels.IsValidIndex(Index)
                        ? LocalSurfels[Index].Get() : nullptr;
                    AppendGrid(Local->GridCounts, Local->BoundsMin, Local->BoundsMax, true, Matrix, Surfels);
                }
            }
        }
        ProbeVisualization->DrawLines(Lines);
    }

    void StartBake(UWorld* World, ADirectionalLight* Light)
    {
        if (!World || !Light)
        {
            FailBake(TEXT("Cannot find an Editor/Game world or DirectionalLight"));
            return;
        }

        FBox Bounds(EForceInit::ForceInit);
        const FPRTGILocalVolume* Local = GetLocalVolume(World, ActiveBakeLocalIndex);
        if (Local) Bounds = FBox(Local->BoundsMin, Local->BoundsMax);
        if ((ActiveBakeLocalIndex != INDEX_NONE && !Local) || (!Local && !ComputeProbeBounds(World, Bounds)))
        {
            FailBake(TEXT("Cannot compute valid probe bounds"));
            return;
        }

        const UPRTGISettings* Settings = GetDefault<UPRTGISettings>();
        const ULightComponent* LightComponent = Light->GetLightComponent();
        if (!LightComponent || !LightComponent->IsVisible() || !LightComponent->bAffectsWorld)
        {
            FailBake(TEXT("DirectionalLight must be visible with Affect World enabled"));
            return;
        }
        ActiveGridCounts = Local ? Local->GridCounts : Settings->GetGridCounts(World);
        ActiveDirectionalSampleCount = FMath::Clamp(Settings->DirectionalSampleCount, 9, 128);
        ActiveReferenceIntensity = FMath::Clamp(Settings->TransportReferenceIntensity, 1.0f, 1000000.0f);
        const int32 ProbeCount = ActiveGridCounts.X * ActiveGridCounts.Y * ActiveGridCounts.Z;
        if (ProbeCount <= 0 || ProbeCount > PRTGIMaxProbeCount)
        {
            FailBake(TEXT("Probe grid count exceeds the allowed range"));
            return;
        }

        ActiveBounds = Bounds;
        ActiveWorld = World;
        ActiveLight = Light;
        ActiveSceneSignature = ComputeSceneSignature(World, Light, Bounds, ActiveGridCounts, Local != nullptr);
        ActiveProbeValidity.Init(1.0f, ProbeCount);
        if (Local)
        {
            FCollisionQueryParams Query(SCENE_QUERY_STAT(PRTGILocalProbeValidity), false);
            for (TActorIterator<APawn> It(World); It; ++It) Query.AddIgnoredActor(*It);
            int32 Rejected = 0;
            for (int32 Probe = 0; Probe < ProbeCount; ++Probe)
            {
                if (World->OverlapBlockingTestByChannel(ProbePosition(Probe), FQuat::Identity,
                    ECC_Visibility, FCollisionShape::MakeSphere(1.0f), Query))
                {
                    ActiveProbeValidity[Probe] = -1.0f;
                    ++Rejected;
                }
            }
            UE_LOG(LogPRTGI, Display, TEXT("Local volume %d: rejected %d/%d solid probes."), ActiveBakeLocalIndex, Rejected, ProbeCount);
        }
        ActiveBaselineCoefficients.Init(
            FVector4f(0.0f, 0.0f, 0.0f, 0.0f),
            ProbeCount * PRTGI::Math::CoefficientCount);
        ActiveTransportCoefficients.Init(
            FVector4f(0.0f, 0.0f, 0.0f, 0.0f),
            ProbeCount * PRTGITransportCoefficientCount);
        if (!PRTGI::Math::BuildDirectionalProjection(
            ActiveDirectionalSampleCount,
            ActiveSampleDirections,
            ActiveProjectionWeights))
        {
            FailBake(TEXT("Cannot build the directional SH least-squares projection"));
            return;
        }
        NextBakeJob = 0;
        LastProgressLogJob = 0;

        if (!CreateCaptureObjects(World))
        {
            FailBake(TEXT("Failed to create the cubemap SceneCapture"));
            return;
        }

        OriginalLightMobility = Light->GetLightComponent()->Mobility;
        OriginalAttachedMobilities.Reset();
        if (OriginalLightMobility != EComponentMobility::Movable)
        {
            TArray<USceneComponent*> AttachedComponents;
            Light->GetLightComponent()->GetChildrenComponents(true, AttachedComponents);
            OriginalAttachedMobilities.Reserve(AttachedComponents.Num());
            for (USceneComponent* AttachedComponent : AttachedComponents)
            {
                if (IsValid(AttachedComponent))
                {
                    OriginalAttachedMobilities.Add(
                        { AttachedComponent, AttachedComponent->Mobility });
                }
            }
            bOwnsBakeLightMobility = true;
            Light->GetLightComponent()->SetMobility(EComponentMobility::Movable);
        }

        bBakeRequested = false;
        BakeState = EBakeState::Baking;
        LastError.Reset();
        UE_LOG(LogPRTGI, Display,
            TEXT("Building directional PRT transport: %d probes x (%d directions + baseline) = %d captures, Grid=%dx%dx%d Bounds=[%s]-[%s] Resolution=%d."),
            ProbeCount,
            ActiveDirectionalSampleCount,
            ProbeCount * (ActiveDirectionalSampleCount + 1),
            ActiveGridCounts.X, ActiveGridCounts.Y, ActiveGridCounts.Z,
            *ActiveBounds.Min.ToCompactString(), *ActiveBounds.Max.ToCompactString(),
            CaptureTarget ? CaptureTarget->SizeX : 0);
    }

    bool ComputeProbeBounds(UWorld* World, FBox& OutBounds) const
    {
        const UPRTGISettings* Settings = GetDefault<UPRTGISettings>();
        if (const FPRTGIMapProfile* Profile = Settings->FindMapProfile(World))
        {
            OutBounds = FBox(Profile->BoundsMin, Profile->BoundsMax);
            return OutBounds.IsValid && OutBounds.GetSize().GetMin() > UE_KINDA_SMALL_NUMBER;
        }
        FBox GeometryBounds(EForceInit::ForceInit);

        if (Settings->bAutoDetectBounds)
        {
            for (TObjectIterator<UStaticMeshComponent> It; It; ++It)
            {
                UStaticMeshComponent* Component = *It;
                if (!IsProbeGeometryComponent(Component, World, Settings))
                {
                    continue;
                }
                GeometryBounds += Component->Bounds.GetBox();
            }
        }

        if (!GeometryBounds.IsValid)
        {
            GeometryBounds = FBox(Settings->ManualBoundsMin, Settings->ManualBoundsMax);
        }
        if (!GeometryBounds.IsValid || GeometryBounds.GetSize().GetMin() <= UE_KINDA_SMALL_NUMBER)
        {
            return false;
        }

        const FVector Inset = GeometryBounds.GetSize() * FMath::Clamp(Settings->BoundsInsetFraction, 0.0f, 0.2f);
        OutBounds = FBox(GeometryBounds.Min + Inset, GeometryBounds.Max - Inset);
        return OutBounds.IsValid && OutBounds.GetSize().GetMin() > UE_KINDA_SMALL_NUMBER;
    }

    bool CreateCaptureObjects(UWorld* World, int32 ResolutionOverride = 0)
    {
        DestroyCaptureObjects();
        const int32 Resolution = FMath::Clamp(ResolutionOverride > 0
            ? ResolutionOverride : GetDefault<UPRTGISettings>()->CaptureResolution, 4, 128);

        CaptureTarget = NewObject<UTextureRenderTargetCube>(GetTransientPackage(), NAME_None, RF_Transient);
        if (!CaptureTarget)
        {
            return false;
        }
        CaptureTarget->AddToRoot();
        CaptureTarget->bHDR = true;
        CaptureTarget->bForceLinearGamma = true;
        CaptureTarget->ClearColor = FLinearColor::Black;
        CaptureTarget->InitAutoFormat(Resolution);
        CaptureTarget->UpdateResourceImmediate(true);

        CaptureComponent = NewObject<USceneCaptureComponentCube>(GetTransientPackage(), NAME_None, RF_Transient);
        if (!CaptureComponent)
        {
            DestroyCaptureObjects();
            return false;
        }
        CaptureComponent->AddToRoot();
        CaptureComponent->TextureTarget = CaptureTarget;
        CaptureComponent->CaptureSource = ESceneCaptureSource::SCS_SceneColorHDRNoAlpha;
        CaptureComponent->PrimitiveRenderMode = ESceneCapturePrimitiveRenderMode::PRM_RenderScenePrimitives;
        CaptureComponent->bCaptureEveryFrame = false;
        CaptureComponent->bCaptureOnMovement = false;
        CaptureComponent->bAlwaysPersistRenderingState = false;
        CaptureComponent->bCaptureRotation = false;
        CaptureComponent->PostProcessBlendWeight = 0.0f;

        CaptureComponent->ShowFlags.SetGlobalIllumination(false);
        CaptureComponent->ShowFlags.SetLumenGlobalIllumination(false);
        CaptureComponent->ShowFlags.SetReflectionEnvironment(false);
        CaptureComponent->ShowFlags.SetLumenReflections(false);
        CaptureComponent->ShowFlags.SetSpecular(false);
        CaptureComponent->ShowFlags.SetSkyLighting(false);
        CaptureComponent->ShowFlags.SetVolumetricLightmap(false);
        CaptureComponent->ShowFlags.SetIndirectLightingCache(false);
        CaptureComponent->ShowFlags.SetAtmosphere(false);
        CaptureComponent->ShowFlags.SetFog(false);
        CaptureComponent->ShowFlags.SetVolumetricFog(false);
        CaptureComponent->ShowFlags.SetPostProcessing(false);
        CaptureComponent->ShowFlags.SetBloom(false);
        CaptureComponent->ShowFlags.SetEyeAdaptation(false);
        // Transport describes the static scene; player/character geometry is excluded
        // from both the signature and the captured radiance.
        for (TActorIterator<APawn> It(World); It; ++It)
        {
            CaptureComponent->HiddenActors.Add(*It);
        }
        CaptureComponent->RegisterComponentWithWorld(World);
        return CaptureComponent->IsRegistered();
    }

    void DestroyCaptureObjects()
    {
        if (CaptureComponent)
        {
            if (CaptureComponent->IsRegistered())
            {
                CaptureComponent->UnregisterComponent();
            }
            CaptureComponent->TextureTarget = nullptr;
            CaptureComponent->RemoveFromRoot();
            CaptureComponent = nullptr;
        }
        if (CaptureTarget)
        {
            CaptureTarget->ReleaseResource();
            CaptureTarget->RemoveFromRoot();
            CaptureTarget = nullptr;
        }
    }

    FVector ProbePosition(const int32 ProbeIndex) const
    {
        const int32 X = ProbeIndex % ActiveGridCounts.X;
        const int32 Y = (ProbeIndex / ActiveGridCounts.X) % ActiveGridCounts.Y;
        const int32 Z = ProbeIndex / (ActiveGridCounts.X * ActiveGridCounts.Y);
        const FVector Alpha(
            static_cast<double>(X) / static_cast<double>(ActiveGridCounts.X - 1),
            static_cast<double>(Y) / static_cast<double>(ActiveGridCounts.Y - 1),
            static_cast<double>(Z) / static_cast<double>(ActiveGridCounts.Z - 1));
        return ActiveBounds.Min + Alpha * ActiveBounds.GetSize();
    }

    bool CaptureProbeIrradiance(
        ADirectionalLight* Light,
        const int32 ProbeIndex,
        const bool bBaseline,
        const FVector3f& Direction,
        TArray<FVector4f>& OutCoefficients,
        TArray<TArray<FLinearColor>>* OutFaces = nullptr,
        float NearClipOverride = -1.0f)
    {
        UDirectionalLightComponent* Component = Light
            ? Cast<UDirectionalLightComponent>(Light->GetLightComponent())
            : nullptr;
        if (!Light || !Component || !CaptureComponent || !CaptureTarget)
        {
            return false;
        }

        const FRotator RestoreRotation = Light->GetActorRotation();
        const float RestoreIntensity = Component->Intensity;
        const float RestoreDiffuseScale = Component->DiffuseScale;
        const FColor RestoreColor = Component->LightColor;
        const bool bRestoreUseTemperature = Component->bUseTemperature != 0;
        const auto RestoreLight = [&]()
        {
            Light->SetActorRotation(RestoreRotation);
            Component->SetIntensity(RestoreIntensity);
            Component->SetDiffuseScale(RestoreDiffuseScale);
            Component->SetLightFColor(RestoreColor);
            Component->bUseTemperature = bRestoreUseTemperature;
            Component->MarkRenderStateDirty();
        };

        Component->bUseTemperature = false;
        Component->SetLightColor(FLinearColor::White, false);
        Component->SetDiffuseScale(1.0f);
        Component->SetIntensity(bBaseline ? 0.0f : ActiveReferenceIntensity);
        if (!bBaseline)
        {
            // UE DirectionalLight forward vector is the ray travel direction;
            // PRT input directions point from the shading point toward the source.
            Light->SetActorRotation(FVector(-Direction).Rotation());
        }
        Component->MarkRenderStateDirty();

        CaptureComponent->SetWorldLocation(ProbePosition(ProbeIndex));
        {
            // Cube captures use this global while constructing their projection matrices.
            // Scope the override to this capture; never leave it changed across editor ticks.
            TGuardValue<float> NearClipGuard(GNearClippingPlane, NearClipOverride > 0.0f
                ? NearClipOverride : GetDefault<UPRTGISettings>()->GetCaptureNearClipCm());
            // UE 5.8's six-view single renderer capture loses directional shadows
            // in this scene. Separate face renderers match independent visibility rays.
            FScopedCaptureConsoleInt CubePassGuard(TEXT("r.SceneCapture.CubeSinglePass"),0);
            CaptureComponent->CaptureScene();
        }

        FTextureRenderTargetResource* Resource = CaptureTarget->GameThread_GetRenderTargetResource();
        if (!Resource)
        {
            RestoreLight();
            return false;
        }

        TArray<TArray<FLinearColor>> Faces;
        Faces.SetNum(PRTGI::Math::CubeFaceCount);
        for (int32 FaceIndex = 0; FaceIndex < PRTGI::Math::CubeFaceCount; ++FaceIndex)
        {
            FReadSurfaceDataFlags Flags(RCM_MinMax, static_cast<ECubeFace>(FaceIndex));
            Flags.SetLinearToGamma(false);
            if (!Resource->ReadLinearColorPixels(Faces[FaceIndex], Flags))
            {
                RestoreLight();
                return false;
            }
        }
        RestoreLight();

        if (OutFaces) *OutFaces = Faces;
        return PRTGI::Math::ProjectCubemapRadiance(
            Faces,
            CaptureTarget->SizeX,
            OutCoefficients);
    }

    void BakeOneTransportCapture()
    {
        UWorld* World = ActiveWorld.Get();
        ADirectionalLight* Light = ActiveLight.Get();
        if (!World || !Light || !CaptureComponent || !CaptureTarget)
        {
            FailBake(TEXT("Transport bake objects became invalid during the build"));
            return;
        }

        const int32 ProbeCount = ActiveGridCounts.X * ActiveGridCounts.Y * ActiveGridCounts.Z;
        const int32 TotalJobs = ProbeCount * (ActiveDirectionalSampleCount + 1);
        const bool bBaseline = NextBakeJob < ProbeCount;
        const int32 DirectionIndex = bBaseline ? INDEX_NONE : (NextBakeJob - ProbeCount) / ProbeCount;
        const int32 ProbeIndex = bBaseline ? NextBakeJob : (NextBakeJob - ProbeCount) % ProbeCount;
        if (ActiveBakeLocalIndex != INDEX_NONE && ActiveProbeValidity[ProbeIndex] < 0.0f)
        {
            ++NextBakeJob;
            if (NextBakeJob >= TotalJobs) CompleteBake();
            return;
        }
        const FVector3f Direction = bBaseline
            ? FVector3f::XAxisVector
            : ActiveSampleDirections[DirectionIndex];

        TArray<FVector4f> ProbeCoefficients;
        if (!CaptureProbeIrradiance(Light, ProbeIndex, bBaseline, Direction, ProbeCoefficients))
        {
            FailBake(FString::Printf(TEXT("Failed to read transport capture: job=%d"), NextBakeJob));
            return;
        }

        const int32 ProbeOutputOffset = ProbeIndex * PRTGI::Math::CoefficientCount;
        if (bBaseline)
        {
            for (int32 OutputCoefficient = 0;
                OutputCoefficient < PRTGI::Math::CoefficientCount;
                ++OutputCoefficient)
            {
                ActiveBaselineCoefficients[ProbeOutputOffset + OutputCoefficient] =
                    ProbeCoefficients[OutputCoefficient];
            }
        }
        else
        {
            const int32 ProbeTransportOffset = ProbeIndex * PRTGITransportCoefficientCount;
            for (int32 OutputCoefficient = 0;
                OutputCoefficient < PRTGI::Math::CoefficientCount;
                ++OutputCoefficient)
            {
                FVector4f DirectionalResponse =
                    ProbeCoefficients[OutputCoefficient]
                    - ActiveBaselineCoefficients[ProbeOutputOffset + OutputCoefficient];
                DirectionalResponse.W = 0.0f;
                DirectionalResponse /= ActiveReferenceIntensity;

                for (int32 InputCoefficient = 0;
                    InputCoefficient < PRTGI::Math::CoefficientCount;
                    ++InputCoefficient)
                {
                    const float ProjectionWeight = ActiveProjectionWeights[
                        DirectionIndex * PRTGI::Math::CoefficientCount + InputCoefficient];
                    ActiveTransportCoefficients[
                        ProbeTransportOffset
                        + OutputCoefficient * PRTGI::Math::CoefficientCount
                        + InputCoefficient] += DirectionalResponse * ProjectionWeight;
                }
            }
        }

        ++NextBakeJob;
        if (NextBakeJob == TotalJobs
            || NextBakeJob - LastProgressLogJob >= FMath::Max(1, TotalJobs / 20))
        {
            UE_LOG(LogPRTGI, Display,
                TEXT("Directional PRT transport bake：%d/%d captures"),
                NextBakeJob,
                TotalJobs);
            LastProgressLogJob = NextBakeJob;
        }

        if (NextBakeJob >= TotalJobs)
        {
            CompleteBake();
        }
    }

    void FinishBakeLightOwnership()
    {
        if (bOwnsBakeLightMobility)
        {
            if (ADirectionalLight* Light = ActiveLight.Get())
            {
                Light->GetLightComponent()->SetMobility(OriginalLightMobility);
            }
            for (const FPRTGIMobilityRestoreState& State : OriginalAttachedMobilities)
            {
                if (USceneComponent* Component = State.Component.Get())
                {
                    Component->SetMobility(State.Mobility);
                }
            }
            bOwnsBakeLightMobility = false;
        }
        OriginalAttachedMobilities.Reset();
        ActiveLight.Reset();
    }

    void CompleteBake()
    {
        UWorld* World = ActiveWorld.Get();
        ADirectionalLight* Light = FindPrimaryDirectionalLight(World);
        FBox CurrentBounds(EForceInit::ForceInit);
        const uint64 CurrentSignature = ComputeProbeBounds(World, CurrentBounds)
            ? ComputeSceneSignature(World, Light,
                ActiveBakeLocalIndex != INDEX_NONE ? ActiveBounds : CurrentBounds,
                ActiveGridCounts, ActiveBakeLocalIndex != INDEX_NONE)
            : 0;
        if (CurrentSignature == 0 || CurrentSignature != ActiveSceneSignature)
        {
            CancelBake(TEXT("Level, geometry, materials, or settings changed during transport bake"));
            bBakeRequested = true;
            BakeState = EBakeState::WaitingForStableLight;
            return;
        }

        TSharedPtr<FPRTGITransportData, ESPMode::ThreadSafe> Data =
            MakeShared<FPRTGITransportData, ESPMode::ThreadSafe>();
        Data->GridCounts = ActiveGridCounts;
        Data->GridMin = FVector3f(ActiveBounds.Min);
        Data->GridMax = FVector3f(ActiveBounds.Max);
        Data->SceneSignature = ActiveSceneSignature;
        Data->TransportCoefficients = MoveTemp(ActiveTransportCoefficients);

        if (ActiveBakeLocalIndex != INDEX_NONE)
        {
            const FPRTGILocalVolume* Local = GetLocalVolume(World, ActiveBakeLocalIndex);
            if (!Local || FBox(Local->BoundsMin, Local->BoundsMax) != ActiveBounds || Local->GridCounts != ActiveGridCounts)
            {
                FailBake(TEXT("Local volume configuration changed during bake"));
                return;
            }
            for (int32 Probe = 0; Probe < ActiveProbeValidity.Num(); ++Probe)
                Data->TransportCoefficients[Probe * PRTGITransportCoefficientCount].W = ActiveProbeValidity[Probe];
            LocalTransports.SetNum(FMath::Max(LocalTransports.Num(), ActiveBakeLocalIndex + 1));
            LocalTransports[ActiveBakeLocalIndex] = Data;
        }
        else PublishedTransport_GameThread = Data;
        ValidatedWorld = World;
        NextSignatureCheckSeconds = FPlatformTime::Seconds() + 2.0;
        if (!SaveData(*Data, GetBakeDataPath(World)))
        {
            UE_LOG(LogPRTGI, Warning, TEXT("Transport is ready, but saving to %s failed."), *GetBakeDataPath(World));
        }

        FinishBakeLightOwnership();
        DestroyCaptureObjects();
        ActiveBaselineCoefficients.Reset();
        ActiveSampleDirections.Reset();
        ActiveProjectionWeights.Reset();
        BakeState = EBakeState::Ready;
        LastError.Reset();
        RelightFromTransport(World, Light);
        UE_LOG(LogPRTGI, Display,
            TEXT("Directional PRT transport built and atomically published: %s"),
            *GetBakeDataPath(World));
    }

    void CancelBake(const FString& Reason)
    {
        UE_LOG(LogPRTGI, Display, TEXT("Cancelled current directional PRT transport bake: %s."), *Reason);
        FinishBakeLightOwnership();
        DestroyCaptureObjects();
        ActiveBaselineCoefficients.Reset();
        ActiveTransportCoefficients.Reset();
        ActiveSampleDirections.Reset();
        ActiveProjectionWeights.Reset();
        NextBakeJob = 0;
        BakeState = EBakeState::Idle;
    }

    void FailBake(const FString& Reason)
    {
        FinishBakeLightOwnership();
        DestroyCaptureObjects();
        ActiveBaselineCoefficients.Reset();
        ActiveTransportCoefficients.Reset();
        ActiveSampleDirections.Reset();
        ActiveProjectionWeights.Reset();
        bBakeRequested = false;
        BakeState = EBakeState::Error;
        LastError = Reason;
        UE_LOG(LogPRTGI, Error, TEXT("Directional PRT transport bake failed: %s."), *Reason);
    }

    void ApplyMapOverride(const TCHAR* Name, float Value)
    {
        if (IConsoleVariable* Variable = IConsoleManager::Get().FindConsoleVariable(Name))
        {
            MapOverridePreviousValues.Add(Name, Variable->GetFloat());
            Variable->Set(Value, ECVF_SetByConsole);
        }
    }

    void RestoreMapOverrides()
    {
        for (const auto& Pair : MapOverridePreviousValues)
        {
            if (IConsoleVariable* Variable = IConsoleManager::Get().FindConsoleVariable(*Pair.Key))
                Variable->Set(Pair.Value, ECVF_SetByConsole);
        }
        MapOverridePreviousValues.Reset();
    }

    FString GetDataPath() const
    {
        if(LoadedMapPackage.StartsWith(TEXT("/PRTGI/")))
        {
            const auto Plugin=IPluginManager::Get().FindPlugin(TEXT("PRTGI"));
            if(Plugin)return FPaths::ConvertRelativePathToFull(FPaths::Combine(Plugin->GetContentDir(),TEXT("Precomputed/Legacy"),FPaths::GetCleanFilename(LoadedDataFile)));
        }
        return FPaths::ConvertRelativePathToFull(
            FPaths::Combine(FPaths::ProjectContentDir(), LoadedDataFile));
    }

    FString GetBakeDataPath(UWorld* World) const
    {
        const FPRTGILocalVolume* Local = GetLocalVolume(World, ActiveBakeLocalIndex);
        return Local ? FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectContentDir(), Local->DataFile)) : GetDataPath();
    }

    bool SaveData(const FPRTGITransportData& Data, const FString& Path) const
    {
        TArray<uint8> Bytes;
        FMemoryWriter Writer(Bytes, true);
        uint32 Magic = PRTGIDataMagic;
        int32 Version = PRTGIDataVersion;
        Writer << Magic;
        Writer << Version;
        Writer << const_cast<FIntVector&>(Data.GridCounts);

        FVector3f GridMin = Data.GridMin;
        FVector3f GridMax = Data.GridMax;
        Writer << GridMin;
        Writer << GridMax;
        uint64 SceneSignature = Data.SceneSignature;
        Writer << SceneSignature;

        int32 CoefficientCount = Data.TransportCoefficients.Num();
        Writer << CoefficientCount;
        for (const FVector4f& Coefficient : Data.TransportCoefficients)
        {
            FVector4f MutableCoefficient = Coefficient;
            Writer << MutableCoefficient;
        }
        Writer.Close();

        const FString TemporaryPath = Path + TEXT(".tmp");
        IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path), true);
        IFileManager::Get().Delete(*TemporaryPath, false, true);
        if (!FFileHelper::SaveArrayToFile(Bytes, *TemporaryPath))
        {
            return false;
        }
        if (!IFileManager::Get().Move(*Path, *TemporaryPath, true, true))
        {
            IFileManager::Get().Delete(*TemporaryPath, false, true);
            return false;
        }
        return true;
    }

    TSharedPtr<const FPRTGITransportData, ESPMode::ThreadSafe> ReadDataFile(
        const FString& Path, const FIntVector& ConfiguredGridCounts) const
    {
        TArray<uint8> Bytes;
        if (!FFileHelper::LoadFileToArray(Bytes, *Path))
        {
            return nullptr;
        }

        if (Bytes.Num() < 56) return nullptr;
        FMemoryReader Reader(Bytes, true);
        uint32 Magic = 0;
        int32 Version = 0;
        FIntVector GridCounts = FIntVector::ZeroValue;
        FVector3f GridMin = FVector3f::ZeroVector;
        FVector3f GridMax = FVector3f::ZeroVector;
        uint64 SceneSignature = 0;
        int32 CoefficientCount = 0;
        Reader << Magic;
        Reader << Version;
        Reader << GridCounts;
        Reader << GridMin;
        Reader << GridMax;
        Reader << SceneSignature;
        Reader << CoefficientCount;

        const int64 ExpectedCoefficientCount = static_cast<int64>(GridCounts.X) * GridCounts.Y * GridCounts.Z
            * PRTGITransportCoefficientCount;
        if (Magic != PRTGIDataMagic
            || Version != PRTGIDataVersion
            || GridCounts != ConfiguredGridCounts
            || GridCounts.X < 2 || GridCounts.Y < 2 || GridCounts.Z < 2
            || SceneSignature == 0
            || ExpectedCoefficientCount <= 0
            || ExpectedCoefficientCount > static_cast<int64>(PRTGIMaxProbeCount) * PRTGITransportCoefficientCount
            || CoefficientCount != ExpectedCoefficientCount
            || Bytes.Num() != 56 + ExpectedCoefficientCount * 16
            || !FMath::IsFinite(GridMin.X) || !FMath::IsFinite(GridMin.Y) || !FMath::IsFinite(GridMin.Z)
            || !FMath::IsFinite(GridMax.X) || !FMath::IsFinite(GridMax.Y) || !FMath::IsFinite(GridMax.Z)
            || GridMax.X <= GridMin.X || GridMax.Y <= GridMin.Y || GridMax.Z <= GridMin.Z)
        {
            UE_LOG(LogPRTGI, Warning, TEXT("Ignoring incompatible or corrupt probe file: %s"), *Path);
            return nullptr;
        }

        TSharedPtr<FPRTGITransportData, ESPMode::ThreadSafe> Data =
            MakeShared<FPRTGITransportData, ESPMode::ThreadSafe>();
        Data->GridCounts = GridCounts;
        Data->GridMin = GridMin;
        Data->GridMax = GridMax;
        Data->SceneSignature = SceneSignature;
        Data->TransportCoefficients.SetNumUninitialized(CoefficientCount);
        bool bAllCoefficientsFinite = true;
        for (FVector4f& Coefficient : Data->TransportCoefficients)
        {
            Reader << Coefficient;
            bAllCoefficientsFinite &= FMath::IsFinite(Coefficient.X)
                && FMath::IsFinite(Coefficient.Y)
                && FMath::IsFinite(Coefficient.Z)
                && FMath::IsFinite(Coefficient.W);
        }

        if (Reader.IsError() || !bAllCoefficientsFinite)
        {
            UE_LOG(LogPRTGI, Warning, TEXT("Failed to read probe file: %s"), *Path);
            return nullptr;
        }

        return Data;
    }

    void LoadData(UWorld* World)
    {
        const auto Data = ReadDataFile(GetDataPath(), GetDefault<UPRTGISettings>()->GetGridCounts(World));
        if (!Data.IsValid()) return;
        PublishedTransport_GameThread = Data;
        ActiveGridCounts = Data->GridCounts;
        ActiveBounds = FBox(FVector(Data->GridMin), FVector(Data->GridMax));
        ActiveDirectionalSampleCount = FMath::Clamp(GetDefault<UPRTGISettings>()->DirectionalSampleCount, 9, 128);
        NextBakeJob = ActiveGridCounts.X * ActiveGridCounts.Y * ActiveGridCounts.Z * (ActiveDirectionalSampleCount + 1);
        BakeState = EBakeState::Ready;
        ValidatedWorld.Reset();
        NextSignatureCheckSeconds = 0.0;
        UE_LOG(LogPRTGI, Display, TEXT("Loaded global transport: %s"), *GetDataPath());
    }

    void LoadLocalVolumes(UWorld* World)
    {
        LocalTransports.Reset();
        LocalSurfels.Reset();
        const auto* Profile = GetDefault<UPRTGISettings>()->FindMapProfile(World);
        if (!Profile) return;
        LocalTransports.SetNum(Profile->LocalVolumes.Num());
        LocalSurfels.SetNum(Profile->LocalVolumes.Num());
        for (int32 Index = 0; Index < LocalTransports.Num(); ++Index)
        {
            const auto* Local = GetLocalVolume(World, Index);
            if (!Local) continue;
            const FString Path = FPaths::Combine(FPaths::ProjectContentDir(), Local->DataFile);
            if (IFileManager::Get().FileExists(*Path)) LocalTransports[Index] = ReadDataFile(Path, Local->GridCounts);
            LocalSurfels[Index] = PRTGI::FSurfelVolume::Load(Path + TEXT(".surfels"),
                ComputeSceneSignature(World, nullptr, FBox(Local->BoundsMin,Local->BoundsMax),Local->GridCounts,true,true));
        }
        ValidateLocalVolumes(World, FindPrimaryDirectionalLight(World));
    }

    void ValidateLocalVolumes(UWorld* World, ADirectionalLight* Light)
    {
        bool Changed = false;
        for (int32 Index=0; Index<LocalSurfels.Num(); ++Index)
        {
            if (!LocalSurfels[Index]) continue;
            const auto* Local=GetLocalVolume(World,Index);
            if (!Local || LocalSurfels[Index]->SceneSignature != ComputeSceneSignature(World,nullptr,
                FBox(Local->BoundsMin,Local->BoundsMax),Local->GridCounts,true,true))
            {
                LocalSurfels[Index].Reset(); Changed=true;
                UE_LOG(LogPRTGI, Warning, TEXT("Surfel cache %d invalidated; explicit PRTGI.BakeSurfels required."),Index);
            }
        }
        for (int32 Index = 0; Index < LocalTransports.Num(); ++Index)
        {
            if (!LocalTransports[Index].IsValid()) continue;
            const auto* Local = GetLocalVolume(World, Index);
            const auto& Data = LocalTransports[Index];
            if (!Local || Data->GridCounts != Local->GridCounts
                || !Data->GridMin.Equals(FVector3f(Local->BoundsMin), 0.001f)
                || !Data->GridMax.Equals(FVector3f(Local->BoundsMax), 0.001f)
                || Data->SceneSignature != ComputeSceneSignature(World, Light,
                    FBox(Local->BoundsMin, Local->BoundsMax), Local->GridCounts, true))
            {
                LocalTransports[Index].Reset();
                Changed = true;
                UE_LOG(LogPRTGI, Warning, TEXT("Local volume %d cache invalid; using global probes until PRTGI.BakeLocal %d."), Index, Index);
            }
        }
        if (Changed) PublishedRenderData_GameThread.Reset();
    }

    bool RelightFromTransport(UWorld* World, ADirectionalLight* Light)
    {
        if (!World || !World->Scene || !Light || !PublishedTransport_GameThread.IsValid())
        {
            return false;
        }

        const TSharedPtr<const FPRTGITransportData, ESPMode::ThreadSafe> Transport =
            PublishedTransport_GameThread;
        const int32 ProbeCount = Transport->GridCounts.X
            * Transport->GridCounts.Y
            * Transport->GridCounts.Z;
        if (Transport->TransportCoefficients.Num() != ProbeCount * PRTGITransportCoefficientCount)
        {
            return false;
        }

        const FPRTGILightState LightState = ReadLightState(*Light);
        const FVector3f IncidentLightDirection = -LightState.Direction;
        const float ScalarIntensity = FMath::Max(
            0.0f,
            LightState.Intensity * LightState.IndirectLightingIntensity);
        const FVector3f LightColor(
            FMath::Max(0.0f, LightState.Color.X),
            FMath::Max(0.0f, LightState.Color.Y),
            FMath::Max(0.0f, LightState.Color.Z));

        TSharedPtr<FPRTGIRenderData, ESPMode::ThreadSafe> RenderData =
            MakeShared<FPRTGIRenderData, ESPMode::ThreadSafe>();
        RenderData->GridCounts = Transport->GridCounts;
        RenderData->bTrilinear = true;
        RenderData->GridMin = Transport->GridMin;
        RenderData->GridMax = Transport->GridMax;
        RenderData->Scene = World->Scene->GetRenderScene();
        if (const FPRTGIMapProfile* Profile = GetDefault<UPRTGISettings>()->FindMapProfile(World))
            RenderData->ReceiverBoundsPadding = FMath::Max(0.0f, Profile->ReceiverBoundsPadding);
        RenderData->Coefficients.SetNumUninitialized(
            ProbeCount * PRTGI::Math::CoefficientCount);

        TArray<FVector4f> ProbeIrradiance;
        for (int32 ProbeIndex = 0; ProbeIndex < ProbeCount; ++ProbeIndex)
        {
            const int32 TransportOffset = ProbeIndex * PRTGITransportCoefficientCount;
            const TConstArrayView<FVector4f> ProbeTransport(
                Transport->TransportCoefficients.GetData() + TransportOffset,
                PRTGITransportCoefficientCount);
            if (!PRTGI::Math::EvaluateDirectionalTransport(
                ProbeTransport,
                IncidentLightDirection,
                ScalarIntensity,
                LightColor,
                ProbeIrradiance))
            {
                return false;
            }

            const int32 OutputOffset = ProbeIndex * PRTGI::Math::CoefficientCount;
            for (int32 CoefficientIndex = 0;
                CoefficientIndex < PRTGI::Math::CoefficientCount;
                ++CoefficientIndex)
            {
                RenderData->Coefficients[OutputOffset + CoefficientIndex] =
                    ProbeIrradiance[CoefficientIndex];
            }
        }

        PublishedRenderData_GameThread = RenderData;
        for (int32 Index = 0; Index < LocalTransports.Num(); ++Index)
        {
            const auto* Local = GetLocalVolume(World, Index);
            const auto& LocalTransport = LocalTransports[Index];
            const bool UseSurfel=CVarPRTGISurfel.GetValueOnGameThread()!=0 && LocalSurfels.IsValidIndex(Index) && LocalSurfels[Index].IsValid();
            if (!Local || (!LocalTransport.IsValid() && !UseSurfel)) continue;
            FPRTGIRenderVolume& Volume = RenderData->LocalVolumes.AddDefaulted_GetRef();
            Volume.GridCounts = Local->GridCounts;
            Volume.Priority = 1;
            Volume.GridMin = FVector3f(Local->BoundsMin);
            Volume.GridMax = FVector3f(Local->BoundsMax);
            Volume.ReceiverMin = FVector3f(Local->ReceiverMin);
            Volume.ReceiverMax = FVector3f(Local->ReceiverMax);
            Volume.BlendFace = FMath::Clamp(Local->BlendFace, 0, 6);
            Volume.BlendDistance = FMath::Max(0.0f, Local->BlendDistance);
            const int32 LocalProbeCount = Volume.GridCounts.X * Volume.GridCounts.Y * Volume.GridCounts.Z;
            Volume.Coefficients.SetNumUninitialized(LocalProbeCount * PRTGI::Math::CoefficientCount);
            if (UseSurfel)
            {
                const float Bias=FMath::Clamp(GetDefault<UPRTGISettings>()->SurfelShadowBiasCm,0.001f,1.0f);
                if (!LocalSurfels[Index]->Relight(World,IncidentLightDirection,LightColor,ScalarIntensity,
                    Light->GetLightComponent()->CastShadows,Bias,Volume.Coefficients)) return false;
                UE_LOG(LogPRTGI,Display,TEXT("PRT_SURFEL relight local=%d samples=%d rays=%d CPU_ms=%.3f (one bounce, no input SH)"),
                    Index,LocalSurfels[Index]->Samples.Num(),LocalSurfels[Index]->LastShadowRays,LocalSurfels[Index]->LastRelightMs);
                continue;
            }
            for (int32 Probe = 0; Probe < LocalProbeCount; ++Probe)
            {
                const TConstArrayView<FVector4f> Matrix(LocalTransport->TransportCoefficients.GetData()
                    + Probe * PRTGITransportCoefficientCount, PRTGITransportCoefficientCount);
                if (!PRTGI::Math::EvaluateDirectionalTransport(Matrix, IncidentLightDirection, ScalarIntensity, LightColor, ProbeIrradiance)) return false;
                for (int32 Coefficient = 0; Coefficient < PRTGI::Math::CoefficientCount; ++Coefficient)
                    Volume.Coefficients[Probe * PRTGI::Math::CoefficientCount + Coefficient] = ProbeIrradiance[Coefficient];
                Volume.Coefficients[Probe * PRTGI::Math::CoefficientCount].W = Matrix[0].W;
            }
        }
        PublishedLightState = LightState;
        PublishedSurfelBackend=CVarPRTGISurfel.GetValueOnGameThread();
        PublishedSurfelBias=GetDefault<UPRTGISettings>()->SurfelShadowBiasCm;
        PublishRenderData(RenderData);
        UE_LOG(LogPRTGI, Display,
            TEXT("Directional PRT runtime relight：Direction=(%.3f, %.3f, %.3f) Intensity=%.1f"),
            IncidentLightDirection.X,
            IncidentLightDirection.Y,
            IncidentLightDirection.Z,
            ScalarIntensity);
        return true;
    }

    void ClearPublishedTransport()
    {
        LocalSurfels.Reset();
        LocalTransports.Reset();
        PublishedTransport_GameThread.Reset();
        PublishedRenderData_GameThread.Reset();
        ValidatedWorld.Reset();
        ENQUEUE_RENDER_COMMAND(PRTGIClearData)(
            [this](FRHICommandListImmediate& RHICmdList)
            {
                RenderData_RenderThread.Reset();
            });
    }

    void PublishRenderData(const TSharedPtr<FPRTGIRenderData, ESPMode::ThreadSafe>& Data)
    {
        TSharedPtr<const FPRTGIRenderData, ESPMode::ThreadSafe> ImmutableData = Data;
        ENQUEUE_RENDER_COMMAND(PRTGIPublishData)(
            [this, ImmutableData](FRHICommandListImmediate& RHICmdList)
            {
                RenderData_RenderThread = ImmutableData;
            });
    }

    void RenderDiffuseIndirectLight_RenderThread(
        const FScene& Scene,
        const FViewInfo& View,
        FRDGBuilder& GraphBuilder,
        FGlobalIlluminationPluginResources& Resources,
        bool bIndirectOnly)
    {
        check(IsInRenderingThread());
        if ((!bIndirectOnly && (CVarPRTGIEnable.GetValueOnRenderThread() == 0
                || CVarPRTGIIndirectOnly.GetValueOnRenderThread() != 0))
            || View.bIsSceneCapture
            || View.bIsReflectionCapture
            || View.bIsPlanarReflection
            || !Resources.GBufferA
            || !Resources.GBufferB
            || !Resources.GBufferC
            || !Resources.SceneDepthZ
            || !Resources.SceneColor)
        {
            return;
        }

        const TSharedPtr<const FPRTGIRenderData, ESPMode::ThreadSafe> Data = RenderData_RenderThread;
        const FVector3f Extent = Data.IsValid() ? Data->GridMax - Data->GridMin : FVector3f(1.0f);
        const bool bHasTransport = !(bActorRenderMode && ActorRenderScene==&Scene) && CVarPRTGIEnable.GetValueOnRenderThread() != 0
            && Data.IsValid() && Data->Scene == &Scene && !Data->Coefficients.IsEmpty()
            && Extent.X > 0.0f && Extent.Y > 0.0f && Extent.Z > 0.0f;
        const bool bHasActors = CVarPRTGIEnable.GetValueOnRenderThread()!=0 && ActorRenderScene==&Scene && !ActorRenderVolumes.IsEmpty();
        if (!bIndirectOnly && !bHasTransport && !bHasActors)
        {
            return;
        }

        TArray<FVector4f> PackedCoefficients;
        TArray<FVector4f> PackedVolumes;
        TArray<FVector4f> VisibilityInfos;
        TArray<TSharedPtr<const FPRTGIProbeVisibility,ESPMode::ThreadSafe>> VisibilitySources;
        int32 VisibilityOffset=0;
        const FVector3f Translation(View.ViewMatrices.GetPreViewTranslation());
        auto AppendVolume = [&](const FPRTGIRenderVolume& Volume, bool bGlobal)
        {
            const FVector3f Size = Volume.GridMax - Volume.GridMin;
            const FVector3f ReceiverMin = bGlobal ? Volume.GridMin - FVector3f(Volume.ReceiverBoundsPadding) : Volume.ReceiverMin;
            const FVector3f ReceiverMax = bGlobal ? Volume.GridMax + FVector3f(Volume.ReceiverBoundsPadding) : Volume.ReceiverMax;
            PackedVolumes.Add(FVector4f(Volume.GridMin + Translation, float(PackedCoefficients.Num())));
            PackedVolumes.Add(FVector4f(FVector3f(1.0f / Size.X, 1.0f / Size.Y, 1.0f / Size.Z), Volume.bTrilinear ? 1.0f : 0.0f));
            PackedVolumes.Add(FVector4f(float(Volume.GridCounts.X), float(Volume.GridCounts.Y), float(Volume.GridCounts.Z), float(Volume.Priority)));
            PackedVolumes.Add(FVector4f(ReceiverMin + Translation, Volume.BlendDistance));
            PackedVolumes.Add(FVector4f(ReceiverMax + Translation, float(Volume.BlendFace)));
            PackedCoefficients.Append(Volume.Coefficients);
            VisibilitySources.Add(Volume.Visibility);
            VisibilityInfos.Add(FVector4f(float(VisibilityOffset),Volume.Visibility?float(Volume.Visibility->Resolution):0.f,Volume.Geometry && Volume.Geometry->CacheVersion>=6?1.f:0.f,Volume.bRuntimeGeometry?1.f:0.f));
            if(Volume.Visibility)VisibilityOffset+=Volume.Visibility->Planes.Num();
        };
        if (bHasTransport)
        {
            AppendVolume(*Data, true);
            if (CVarPRTGILocalVolumes.GetValueOnRenderThread() != 0)
                for (const auto& Volume : Data->LocalVolumes) AppendVolume(Volume, false);
        }
        if (bHasActors)
            for (const auto& Volume:ActorRenderVolumes) AppendVolume(Volume,false);
        const uint32 VolumeCount = PackedVolumes.Num() / 5;
        if (PackedVolumes.IsEmpty()) PackedVolumes.Init(FVector4f(0.0f), 5);
        if (PackedCoefficients.IsEmpty()) PackedCoefficients.Add(FVector4f(0.0f));
        FRDGBufferRef CoefficientBuffer = CreateStructuredBuffer(GraphBuilder, TEXT("PRTGI.ProbeCoefficients"), PackedCoefficients);
        if(bHasActors)
        {
            uint32 Offset=0;
            for(const auto& Volume:ActorRenderVolumes)
            {
                if(Volume.GPUCoefficients && Volume.GPUCoefficients->Buffer)
                {
                    auto* Copy=GraphBuilder.AllocParameters<FPRTGIPublishCS::FParameters>();
                    Copy->SourceSH=GraphBuilder.CreateSRV(GraphBuilder.RegisterExternalBuffer(Volume.GPUCoefficients->Buffer));
                    Copy->DestinationSH=GraphBuilder.CreateUAV(CoefficientBuffer);
                    Copy->CoefficientCount=Volume.Coefficients.Num();Copy->DestinationOffset=Offset;
                    TShaderMapRef<FPRTGIPublishCS> Shader(View.ShaderMap);
                    FComputeShaderUtils::AddPass(GraphBuilder,RDG_EVENT_NAME("PRT Publish GPU Coefficients"),Shader,Copy,
                        FIntVector(FMath::DivideAndRoundUp(Copy->CoefficientCount,64u),1,1));
                }
                Offset+=Volume.Coefficients.Num();
            }
        }
        FRDGBufferRef VolumeBuffer = CreateStructuredBuffer(GraphBuilder, TEXT("PRTGI.ProbeVolumes"), PackedVolumes);
        TSharedPtr<const PRTGI::FMultiBounceData,ESPMode::ThreadSafe> ContactGeometry;
        const bool bRuntimeGeometry=bHasActors && ActorRenderVolumes.ContainsByPredicate([](const auto& V){return V.bRuntimeGeometry;});
        if(bHasActors && (bRuntimeGeometry || CVarPRTContactSamples.GetValueOnRenderThread()>0 || CVarPRTProbeVisibility.GetValueOnRenderThread()==3))
            for(const auto& Volume:ActorRenderVolumes)if(Volume.Geometry){ContactGeometry=Volume.Geometry;break;}
        if(ContactGeometry!=ContactGeometry_RenderThread || !ContactTriangles_RenderThread)
        {
            const TArray<FVector4f> Empty{FVector4f(0)};
            ContactTriangles_RenderThread=GraphBuilder.ConvertToExternalBuffer(CreateStructuredBuffer(GraphBuilder,TEXT("PRTGI.ContactTriangles"),ContactGeometry && !ContactGeometry->Triangles.IsEmpty()?ContactGeometry->Triangles:Empty));
            ContactNodes_RenderThread=GraphBuilder.ConvertToExternalBuffer(CreateStructuredBuffer(GraphBuilder,TEXT("PRTGI.ContactNodes"),ContactGeometry && !ContactGeometry->Nodes.IsEmpty()?ContactGeometry->Nodes:Empty));
            ContactGeometry_RenderThread=ContactGeometry;
        }
        FRDGBufferRef VisibilityBuffer,DepthMomentsBuffer;
        if(VisibilitySources!=VisibilitySources_RenderThread || !VisibilityBuffer_RenderThread)
        {
            TArray<FVector4f> Planes;
            TArray<FVector2f> Moments;
            for(const auto& Source:VisibilitySources)if(Source){Planes.Append(Source->Planes);Moments.Append(Source->DepthMoments);}
            if(Planes.IsEmpty())Planes.Add(FVector4f(0));
            if(Moments.IsEmpty())Moments.Add(FVector2f(0,0));
            VisibilityBuffer=CreateStructuredBuffer(GraphBuilder,TEXT("PRTGI.ProbeVisibilityPlanes"),Planes);
            DepthMomentsBuffer=CreateStructuredBuffer(GraphBuilder,TEXT("PRTGI.ProbeDepthMoments"),Moments);
            GraphBuilder.QueueBufferExtraction(VisibilityBuffer,&VisibilityBuffer_RenderThread);
            GraphBuilder.QueueBufferExtraction(DepthMomentsBuffer,&DepthMomentsBuffer_RenderThread);
            VisibilitySources_RenderThread=VisibilitySources;
        }
        else {VisibilityBuffer=GraphBuilder.RegisterExternalBuffer(VisibilityBuffer_RenderThread);DepthMomentsBuffer=GraphBuilder.RegisterExternalBuffer(DepthMomentsBuffer_RenderThread);}
        if(VisibilityInfos.IsEmpty())VisibilityInfos.Add(FVector4f(0));
        FRDGBufferRef VisibilityInfoBuffer=CreateStructuredBuffer(GraphBuilder,TEXT("PRTGI.ProbeVisibilityVolumes"),VisibilityInfos);

        FPRTGIApplyParameters* Parameters = GraphBuilder.AllocParameters<FPRTGIApplyParameters>();
        Parameters->NormalTexture = Resources.GBufferA;
        Parameters->BaseColorTexture = Resources.GBufferC;
        Parameters->MetallicTexture = Resources.GBufferB;
        Parameters->DepthTexture = Resources.SceneDepthZ;
        Parameters->ProbeCoefficients = GraphBuilder.CreateSRV(CoefficientBuffer);
        Parameters->SceneColorOutput = GraphBuilder.CreateUAV(Resources.SceneColor);
        Parameters->PointClampSampler = TStaticSamplerState<SF_Point, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();
        Parameters->ProbeVolumes = GraphBuilder.CreateSRV(VolumeBuffer);
        Parameters->ProbeVisibilityPlanes=GraphBuilder.CreateSRV(VisibilityBuffer);
        Parameters->ProbeDepthMoments=GraphBuilder.CreateSRV(DepthMomentsBuffer);
        Parameters->ProbeVisibilityVolumes=GraphBuilder.CreateSRV(VisibilityInfoBuffer);
        Parameters->ProbeVisibilityEnabled=uint32(FMath::Clamp(CVarPRTProbeVisibility.GetValueOnRenderThread(),0,3));
        Parameters->RuntimeVisibilityAnyHit=CVarPRTRuntimeVisibilityAnyHit.GetValueOnRenderThread()!=0;
        Parameters->VisibilityBiasCm=FMath::Max(.001f,CVarPRTVisibilityBias.GetValueOnRenderThread());
        Parameters->HemisphereTransition=FMath::Clamp(CVarPRTHemisphereTransition.GetValueOnRenderThread(),0.f,.5f);
        Parameters->DebugMode=uint32(FMath::Clamp(CVarPRTDebug.GetValueOnRenderThread(),0,7));
        Parameters->ContactTriangles=GraphBuilder.CreateSRV(GraphBuilder.RegisterExternalBuffer(ContactTriangles_RenderThread));
        Parameters->ContactNodes=GraphBuilder.CreateSRV(GraphBuilder.RegisterExternalBuffer(ContactNodes_RenderThread));
        Parameters->ContactNodeCount=ContactGeometry?ContactGeometry->Nodes.Num()/2:0;
        Parameters->ContactSamples=FMath::Clamp(CVarPRTContactSamples.GetValueOnRenderThread(),0,16);
        Parameters->ContactRadius=FMath::Clamp(CVarPRTContactRadius.GetValueOnRenderThread(),0.f,200.f);
        Parameters->ContactBias=FMath::Clamp(CVarPRTContactBias.GetValueOnRenderThread(),.001f,1.f);
        Parameters->ContactWorldTranslation=Translation;
        Parameters->VolumeCount = VolumeCount;
        Parameters->ViewRectMin = View.ViewRect.Min;
        Parameters->ViewSize = View.ViewRect.Size();
        Parameters->IndirectIntensity = FMath::Max(0.0f, CVarPRTGIIntensity.GetValueOnRenderThread());
        Parameters->ShouldUsePreExposure = View.Family && View.Family->EngineShowFlags.Tonemapper ? 1u : 0u;
        Parameters->OverwriteSceneColor = bIndirectOnly ? 1u : 0u;
        Parameters->HasTransport = bHasTransport || bHasActors ? 1u : 0u;
        Parameters->View = View.ViewUniformBuffer;

        Parameters->RuntimeResolveHalfResolution=bRuntimeGeometry && Parameters->DebugMode==0
            && CVarPRTRuntimeResolveHalfResolution.GetValueOnRenderThread()!=0;
        Parameters->RuntimeIrradianceTexture=Resources.GBufferA;
        Parameters->RuntimeIrradianceOutput=nullptr;

        Parameters->ContactHalfResolution=CVarPRTContactHalfResolution.GetValueOnRenderThread()!=0
            && Parameters->ContactNodeCount>0 && Parameters->ContactSamples>0
            && Parameters->ContactRadius>0 && (Parameters->DebugMode==0 || Parameters->DebugMode==4);
        // A bound dummy is required even when the shader's runtime branch skips it.
        Parameters->ContactTexture=Resources.GBufferA;
        Parameters->ContactOutput=nullptr;
        if(Parameters->ContactHalfResolution)
        {
            const FIntPoint HalfSize=FIntPoint::DivideAndRoundUp(View.ViewRect.Size(),2);
            FRDGTextureRef Contact=GraphBuilder.CreateTexture(FRDGTextureDesc::Create2D(HalfSize,
                PF_FloatRGBA,FClearValueBinding::None,TexCreate_ShaderResource|TexCreate_UAV),TEXT("PRTGI.ContactHalf"));
            auto* ContactParameters=GraphBuilder.AllocParameters<FPRTGIApplyParameters>();
            *ContactParameters=*Parameters;
            ContactParameters->ContactOutput=GraphBuilder.CreateUAV(Contact);
            TShaderMapRef<FPRTGIContactCS> ContactShader(GetGlobalShaderMap(View.GetFeatureLevel()));
            FComputeShaderUtils::AddPass(GraphBuilder,RDG_EVENT_NAME("PRTGI Contact Half"),ContactShader,
                ContactParameters,FComputeShaderUtils::GetGroupCount(HalfSize,FIntPoint(8,8)));
            Parameters->ContactTexture=Contact;
        }

        if(Parameters->RuntimeResolveHalfResolution)
        {
            const FIntPoint HalfSize=FIntPoint::DivideAndRoundUp(View.ViewRect.Size(),2);
            FRDGTextureRef Irradiance=GraphBuilder.CreateTexture(FRDGTextureDesc::Create2D(HalfSize,
                PF_A32B32G32R32F,FClearValueBinding::None,TexCreate_ShaderResource|TexCreate_UAV),TEXT("PRTGI.RuntimeIrradianceHalf"));
            auto* GatherParameters=GraphBuilder.AllocParameters<FPRTGIApplyParameters>();
            *GatherParameters=*Parameters;
            GatherParameters->RuntimeIrradianceOutput=GraphBuilder.CreateUAV(Irradiance);
            TShaderMapRef<FPRTGIRuntimeResolveCS> GatherShader(GetGlobalShaderMap(GMaxRHIShaderPlatform));
            FComputeShaderUtils::AddPass(GraphBuilder,RDG_EVENT_NAME("PRTGI Runtime Irradiance Half"),GatherShader,
                GatherParameters,FComputeShaderUtils::GetGroupCount(HalfSize,FIntPoint(8,8)));
            Parameters->RuntimeIrradianceTexture=Irradiance;
        }

        TShaderMapRef<FPRTGIApplyCS> ComputeShader(GetGlobalShaderMap(View.GetFeatureLevel()));
        FComputeShaderUtils::AddPass(
            GraphBuilder,
            RDG_EVENT_NAME("PRTGI %s", bIndirectOnly ? TEXT("Indirect Only") : TEXT("Apply Diffuse Indirect")),
            ComputeShader,
            Parameters,
            FComputeShaderUtils::GetGroupCount(View.ViewRect.Size(), FIntPoint(8, 8)));
    }

    // Explicit diagnostic only: leaves bake settings, caches and the runtime relight path unchanged.
    void ValidateLocalDirection(const TArray<FString>& Args)
    {
        UWorld* World = FindPreferredWorld();
        ADirectionalLight* Light = FindPrimaryDirectionalLight(World);
        const int32 Index = Args.Num() ? FCString::Atoi(*Args[0]) : 0;
        const int32 Resolution = Args.Num() > 1 ? FMath::Clamp(FCString::Atoi(*Args[1]), 4, 128) : 16;
        const float NearClip = Args.Num() > 2 ? FCString::Atof(*Args[2]) : GetDefault<UPRTGISettings>()->GetCaptureNearClipCm();
        if (!World || !Light || BakeState == EBakeState::Baking || bBakeRequested
            || !FMath::IsFinite(NearClip) || NearClip <= 0.0f)
        {
            UE_LOG(LogPRTGI, Error, TEXT("Direction validation requires an idle world/light and a positive near clip."));
            return;
        }
        ValidateLocalVolumes(World, Light);
        const FPRTGILocalVolume* Local = GetLocalVolume(World, Index);
        if (!Local || !LocalTransports.IsValidIndex(Index) || !LocalTransports[Index].IsValid()
            || Light->GetLightComponent()->Mobility != EComponentMobility::Movable)
        {
            UE_LOG(LogPRTGI, Error, TEXT("Direction validation requires a valid local cache and movable light."));
            return;
        }
        const auto Transport = LocalTransports[Index];
        TGuardValue<FBox> BoundsGuard(ActiveBounds, FBox(Local->BoundsMin, Local->BoundsMax));
        TGuardValue<FIntVector> GridGuard(ActiveGridCounts, Local->GridCounts);
        TGuardValue<float> IntensityGuard(ActiveReferenceIntensity, GetDefault<UPRTGISettings>()->TransportReferenceIntensity);
        TGuardValue<float> ClipGuard(GNearClippingPlane, NearClip);
        if (!CreateCaptureObjects(World, Resolution))
        {
            DestroyCaptureObjects();
            UE_LOG(LogPRTGI, Error, TEXT("Direction validation could not create capture objects."));
            return;
        }
        const FVector3f Direction = -FVector3f(Light->GetActorForwardVector());
        const FVector3f Normal(-1.0f, 0.0f, 0.0f);
        UE_LOG(LogPRTGI, Display, TEXT("PRT_DIRECTION_CHECK begin: local=%d res=%d near=%.3f direction=%s reference_lux=%.1f"),
            Index, Resolution, NearClip, *Direction.ToString(), ActiveReferenceIntensity);
        auto Integrate = [&](const TArray<TArray<FLinearColor>>& Faces)
        {
            FVector3d Sum(0.0); double Total = 0.0;
            for (int32 Face = 0; Face < 6; ++Face)
                for (int32 Y = 0; Y < Resolution; ++Y)
                    for (int32 X = 0; X < Resolution; ++X)
                    {
                        const double U = 2.0 * (X + 0.5) / Resolution - 1.0;
                        const double V = 1.0 - 2.0 * (Y + 0.5) / Resolution;
                        const double W = 1.0 / FMath::Pow(1.0 + U * U + V * V, 1.5);
                        const FLinearColor Pixel = Faces[Face][Y * Resolution + X];
                        const double Cosine = FMath::Max(0.0f, FVector3f::DotProduct(Normal,
                            PRTGI::Math::CubemapTexelDirection(Face, X, Y, Resolution)));
                        Sum += FVector3d(FMath::Max(0.0f, Pixel.R), FMath::Max(0.0f, Pixel.G), FMath::Max(0.0f, Pixel.B)) * (W * Cosine);
                        Total += W;
                    }
            return Sum * (4.0 * UE_DOUBLE_PI / Total);
        };
        FString CSV = TEXT("probe,x,y,z,resolution,near_clip,cached_r,cached_g,cached_b,direct_sh_r,direct_sh_g,direct_sh_b,direct_cos_r,direct_cos_g,direct_cos_b,baseline_cos_r,baseline_cos_g,baseline_cos_b\n");
        bool Success = true;
        int32 Captured = 0;
        // Back-wall-adjacent column; upper half includes the reported dark band.
        for (int32 Z = ActiveGridCounts.Z / 2; Z < ActiveGridCounts.Z && Success; ++Z)
            for (int32 Y = 0; Y < ActiveGridCounts.Y; ++Y)
            {
                const int32 Probe = ActiveGridCounts.X - 1 + ActiveGridCounts.X * (Y + ActiveGridCounts.Y * Z);
                if (Transport->TransportCoefficients[Probe * PRTGITransportCoefficientCount].W < 0.0f) continue;
                TArray<FVector4f> Baseline, Direct, Predicted;
                TArray<TArray<FLinearColor>> BaselineFaces, DirectFaces;
                if (!CaptureProbeIrradiance(Light, Probe, true, Direction, Baseline, &BaselineFaces, NearClip)
                    || !CaptureProbeIrradiance(Light, Probe, false, Direction, Direct, &DirectFaces, NearClip))
                { Success = false; break; }
                for (int32 C = 0; C < 9; ++C) Direct[C] -= Baseline[C];
                const TConstArrayView<FVector4f> Matrix(Transport->TransportCoefficients.GetData() + Probe * PRTGITransportCoefficientCount, PRTGITransportCoefficientCount);
                if (!PRTGI::Math::EvaluateDirectionalTransport(Matrix, Direction, ActiveReferenceIntensity, FVector3f(1.0f), Predicted))
                { Success = false; break; }
                const FVector3f CachedE = PRTGI::Math::ReconstructIrradiance(Predicted, Normal);
                const FVector3f DirectE = PRTGI::Math::ReconstructIrradiance(Direct, Normal);
                const FVector3d BaseE = Integrate(BaselineFaces);
                const FVector3d CosE = Integrate(DirectFaces) - BaseE;
                const FVector P = ProbePosition(Probe);
                if (Args.Num()>3 && (Probe==335 || Probe==383))
                {
                    const FString DebugDirectory=FPaths::Combine(FPaths::ProjectSavedDir(),TEXT("PRTSurfelValidation"));
                    IFileManager::Get().MakeDirectory(*DebugDirectory,true);
                    TArray<uint8> Pixels;
                    for (int32 F=0;F<6;++F)
                        Pixels.Append(reinterpret_cast<const uint8*>(DirectFaces[F].GetData()),DirectFaces[F].Num()*sizeof(FLinearColor));
                    FFileHelper::SaveArrayToFile(Pixels,*FPaths::Combine(DebugDirectory,FString::Printf(TEXT("probe%d_direct_rgba32.bin"),Probe)));
                }
                CSV += FString::Printf(TEXT("%d,%.6f,%.6f,%.6f,%d,%.6f,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g\n"),
                    Probe, P.X, P.Y, P.Z, Resolution, NearClip,
                    CachedE.X, CachedE.Y, CachedE.Z, DirectE.X, DirectE.Y, DirectE.Z,
                    CosE.X, CosE.Y, CosE.Z, BaseE.X, BaseE.Y, BaseE.Z);
                ++Captured;
            }
        DestroyCaptureObjects();
        const FString Directory = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("PRTDirectionValidation"));
        IFileManager::Get().MakeDirectory(*Directory, true);
        const FString Path = FPaths::Combine(Directory, FString::Printf(TEXT("local%d_res%d_near%.3f.csv"), Index, Resolution, NearClip));
        if (!Success || !FFileHelper::SaveStringToFile(CSV, *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
        {
            UE_LOG(LogPRTGI, Error, TEXT("PRT_DIRECTION_CHECK failed; caches unchanged."));
        }
        else
        {
            UE_LOG(LogPRTGI, Display, TEXT("PRT_DIRECTION_CHECK complete: %d probes, %s; caches unchanged."), Captured, *Path);
        }
    }

    void RequestSurfelBake(const TArray<FString>& Args)
    {
        if (ActorVolumes && ActorVolumes->IsBaking()) return;
        UWorld* World=FindPreferredWorld();
        const int32 Index=Args.Num()?FCString::Atoi(*Args[0]):0;
        const auto* Local=GetLocalVolume(World,Index);
        if (!Local || BakeState==EBakeState::Baking || bBakeRequested || SurfelBake)
        {
            UE_LOG(LogPRTGI,Error,TEXT("PRT_SURFEL requires a valid local profile and idle baker."));
            return;
        }
        const auto* Settings=GetDefault<UPRTGISettings>();
        auto Data=MakeShared<PRTGI::FSurfelVolume>();
        const FBox Bounds(Local->BoundsMin,Local->BoundsMax);
        if (!Data->Begin(World,Bounds,Local->GridCounts,FMath::Clamp(Settings->SurfelCaptureResolution,4,32),
            Settings->GetCaptureNearClipCm(),ComputeSceneSignature(World,nullptr,Bounds,Local->GridCounts,true,true)))
        {
            UE_LOG(LogPRTGI,Error,TEXT("PRT_SURFEL cannot initialize geometry capture (sample budget 4M).")); return;
        }
        SurfelBake=Data; SurfelBakeIndex=Index; SurfelBakeWorld=World;
        UE_LOG(LogPRTGI,Display,TEXT("PRT_SURFEL begin local=%d probes=%d res=%d near=%.3f; no light-direction sampling"),
            Index,Data->Validity.Num(),Data->CaptureResolution,Data->CaptureNearClipCm);
    }

    void TickSurfelBake(UWorld* World, ADirectionalLight* Light)
    {
        if (!SurfelBake) return;
        const auto* Local=GetLocalVolume(World,SurfelBakeIndex);
        if (!Local || BakeState==EBakeState::Baking || bBakeRequested || !SurfelBake->Step(World))
        {
            UE_LOG(LogPRTGI,Error,TEXT("PRT_SURFEL aborted: %s"),*SurfelBake->Error);
            SurfelBake.Reset(); SurfelBakeIndex=INDEX_NONE; return;
        }
        if (SurfelBake->NextProbe%64==0)
        {
            UE_LOG(LogPRTGI,Display,TEXT("PRT_SURFEL progress %d/%d samples=%d"),
                SurfelBake->NextProbe,SurfelBake->Validity.Num(),SurfelBake->Samples.Num());
        }
        if (!SurfelBake->IsComplete()) return;
        SurfelBake->ReleaseCapture();
        const uint64 Signature=ComputeSceneSignature(World,nullptr,FBox(Local->BoundsMin,Local->BoundsMax),Local->GridCounts,true,true);
        const FString Path=FPaths::Combine(FPaths::ProjectContentDir(),Local->DataFile)+TEXT(".surfels");
        if (!SurfelBake->AuditWarning().IsEmpty())
            UE_LOG(LogPRTGI,Warning,TEXT("PRT_SURFEL capture exclusion: %s"),*SurfelBake->AuditWarning());
        if (Signature!=SurfelBake->SceneSignature || !SurfelBake->PassesGeometryAudit() || !SurfelBake->Save(Path))
        {
            UE_LOG(LogPRTGI,Error,TEXT("PRT_SURFEL validation/save failed: rejected probes=%d/%d (allowed 5%%); old file retained"),
                SurfelBake->RejectedCaptureProbes,SurfelBake->AuditedProbes);
        }
        else
        {
            LocalSurfels.SetNum(FMath::Max(LocalSurfels.Num(),SurfelBakeIndex+1));
            LocalSurfels[SurfelBakeIndex]=SurfelBake;
            UE_LOG(LogPRTGI,Display,TEXT("PRT_SURFEL complete local=%d samples=%d geometry_mismatches=%d/%d max_error_cm=%.4f file=%s"),
                SurfelBakeIndex,SurfelBake->Samples.Num(),SurfelBake->GeometryMismatches,SurfelBake->GeometryChecks,
                SurfelBake->MaxGeometryErrorCm,*Path);
            PublishedRenderData_GameThread.Reset();
        }
        SurfelBake.Reset(); SurfelBakeIndex=INDEX_NONE;
    }

    void DumpSurfelProbes(const TArray<FString>& Args)
    {
        UWorld* World=FindPreferredWorld();
        ADirectionalLight* Light=FindPrimaryDirectionalLight(World);
        const int32 Index=Args.Num()?FCString::Atoi(*Args[0]):0;
        ValidateLocalVolumes(World,Light);
        if (!Light || !LocalSurfels.IsValidIndex(Index) || !LocalSurfels[Index] || SurfelBake) return;
        const auto State=ReadLightState(*Light);
        TArray<FVector4f> Coefficients;
        auto& Data=*LocalSurfels[Index];
        const FString Directory=FPaths::Combine(FPaths::ProjectSavedDir(),TEXT("PRTSurfelValidation"));
        IFileManager::Get().MakeDirectory(*Directory,true);
        if (!Data.Relight(World,-State.Direction,State.Color,State.Intensity*State.IndirectLightingIntensity,
            Light->GetLightComponent()->CastShadows,FMath::Clamp(GetDefault<UPRTGISettings>()->SurfelShadowBiasCm,0.001f,1.0f),Coefficients,
            Args.Num()>1?FPaths::Combine(Directory,TEXT("sample_radiance.csv")):FString())) return;
        FString CSV=TEXT("probe,x,y,z,valid,surfel_r,surfel_g,surfel_b\n");
        for (int32 P=0;P<Data.Validity.Num();++P)
        {
            const FVector Pos=Data.ProbePosition(P);
            const FVector3f E=PRTGI::Math::ReconstructIrradiance(MakeArrayView(Coefficients.GetData()+P*9,9),FVector3f(-1,0,0));
            CSV+=FString::Printf(TEXT("%d,%.6f,%.6f,%.6f,%.0f,%.9g,%.9g,%.9g\n"),P,Pos.X,Pos.Y,Pos.Z,Data.Validity[P],E.X,E.Y,E.Z);
        }
        const bool Saved=FFileHelper::SaveStringToFile(CSV,*FPaths::Combine(Directory,TEXT("surfel_probes.csv")));
        UE_LOG(LogPRTGI,Display,TEXT("PRT_SURFEL dump %s"),Saved?TEXT("complete"):TEXT("failed"));
    }

    void ImportVolumeActors()
    {
#if WITH_EDITOR
        UWorld* World=FindPreferredWorld();
        if (!World || World->WorldType!=EWorldType::Editor || TActorIterator<APRTProbeVolume>(World))
        { UE_LOG(LogPRTGI,Warning,TEXT("Import requires an editor world without existing PRT Probe Volume actors.")); return; }
        const auto* Settings=GetDefault<UPRTGISettings>();
        const auto* Profile=Settings->FindMapProfile(World);
        FBox Global;
        if(!ComputeProbeBounds(World,Global)) return;
        World->Modify();
        auto Spawn=[&](FString Name, FBox Grid, FBox Receiver, FIntVector Counts, FString Cache, bool GlobalVolume, int32 Priority, int32 Face, float Blend)
        {
            FActorSpawnParameters Params;
            Params.ObjectFlags|=RF_Transactional;
            auto* A=World->SpawnActor<APRTProbeVolume>(Receiver.GetCenter(), FRotator::ZeroRotator, Params);
            if(!A) return;
            A->Modify(); A->SetActorLabel(Name); A->Extent=Receiver.GetExtent();
            A->ProbeInset=Grid.Min-Receiver.Min;
            A->ProbeSpacing=Grid.GetSize()/FVector(Counts-FIntVector(1));
            A->Priority=Priority; A->NearClipCm=Settings->GetCaptureNearClipCm();
            A->CaptureResolution=Settings->SurfelCaptureResolution;
            A->LegacyCacheBase=Cache; A->bLegacyGlobal=GlobalVolume;
            A->LegacyGridMin=Grid.Min; A->LegacyGridMax=Grid.Max; A->LegacyCounts=Counts;
            A->BlendFace=Face; A->BlendDistance=Blend;
            A->OnConstruction(A->GetActorTransform()); A->MarkPackageDirty();
        };
        const float Padding=Profile?Profile->ReceiverBoundsPadding:0;
        Spawn(TEXT("PRT_Global"),Global,FBox(Global.Min-FVector(Padding),Global.Max+FVector(Padding)),
            Settings->GetGridCounts(World),Profile?Profile->DataFile:Settings->DataFile,true,0,0,0);
        if(Profile)
            for(const auto& V:Profile->LocalVolumes)
                Spawn(TEXT("PRT_")+V.Name,FBox(V.BoundsMin,V.BoundsMax),FBox(V.ReceiverMin,V.ReceiverMax),
                    V.GridCounts,V.DataFile,false,10,V.BlendFace,V.BlendDistance);
        UE_LOG(LogPRTGI,Display,TEXT("Imported editable Probe Volume actors. Legacy caches retained; save the level when ready."));
#endif
    }

    void RegisterConsoleCommands()
    {
        IConsoleManager& Console = IConsoleManager::Get();
        ConsoleObjects.Add(Console.RegisterConsoleCommand(TEXT("PRTGI.Runtime.Prepare"),
            TEXT("Prepare reusable asset-local material charts for the PC runtime reference; does not bake scene transport or save assets."),
            FConsoleCommandDelegate::CreateLambda([this]{UE_LOG(LogPRTGI,Display,TEXT("%s"),*RuntimeGI->Prepare(FindPreferredWorld()));}),ECVF_Default));
        ConsoleObjects.Add(Console.RegisterConsoleCommand(TEXT("PRTGI.Runtime.Status"),TEXT("Report PC runtime reference generation and timing."),
            FConsoleCommandDelegate::CreateLambda([this]{UE_LOG(LogPRTGI,Display,TEXT("%s"),*RuntimeGI->Status());}),ECVF_Default));
        ConsoleObjects.Add(Console.RegisterConsoleCommand(TEXT("PRTGI.Runtime.Dump"),TEXT("Write published runtime probe SH to Saved/PRTGIRuntime/probes.csv."),
            FConsoleCommandDelegate::CreateLambda([this]{UE_LOG(LogPRTGI,Display,TEXT("%s"),*RuntimeGI->Dump());}),ECVF_Default));
        ConsoleObjects.Add(Console.RegisterConsoleCommand(TEXT("PRTGI.Runtime.Surfels.Validate"),TEXT("Audit GPU Surfel ownership, transforms, grid membership and lookup against a CPU oracle; diagnostic readback only."),
            FConsoleCommandDelegate::CreateLambda([this]{UE_LOG(LogPRTGI,Display,TEXT("%s"),*RuntimeGI->AuditSurfels());}),ECVF_Default));
        ConsoleObjects.Add(Console.RegisterConsoleCommand(TEXT("PRTGI.ImportVolumes"),
            TEXT("Create editable Volume Actors from the current legacy grid/profile, reusing caches read-only. Does not save the map or bake."),
            FConsoleCommandDelegate::CreateRaw(this, &FPRTGIModule::ImportVolumeActors), ECVF_Default));
        ConsoleObjects.Add(Console.RegisterConsoleCommand(TEXT("PRTGI.ToggleProbes"),
            TEXT("Toggle probe position markers (Ctrl+Alt+P). Does not change GI mode or bake data."),
            FConsoleCommandDelegate::CreateStatic(&PRTGI::ToggleProbes), ECVF_Default));
        ConsoleObjects.Add(Console.RegisterConsoleCommand(TEXT("PRTGI.ValidateMultiBounce"),
            TEXT("Compare published GPU transport against CPU and one-bounce reference; writes Saved/PRTMultiBounceValidation.json."),
            FConsoleCommandDelegate::CreateLambda([this]{UE_LOG(LogPRTGI,Display,TEXT("%s"),*ActorVolumes->ValidateMultiBounce());}),ECVF_Default));
        ConsoleObjects.Add(Console.RegisterConsoleCommand(TEXT("PRTGI.DumpQuality"),
            TEXT("Write cache version, surface binding, missing-hit and bounce statistics to Saved/PRTGIQuality.json without relighting."),
            FConsoleCommandDelegate::CreateLambda([this]{UE_LOG(LogPRTGI,Display,TEXT("%s"),*ActorVolumes->DumpQualityReport());}),ECVF_Default));
        ConsoleObjects.Add(Console.RegisterConsoleCommand(TEXT("PRTGI.BakeSurfels"),
            TEXT("Bake light-independent local G-buffer geometry: <localIndex>. Separate .surfels file; no directional matrix overwrite."),
            FConsoleCommandWithArgsDelegate::CreateRaw(this,&FPRTGIModule::RequestSurfelBake),ECVF_Default));
        ConsoleObjects.Add(Console.RegisterConsoleCommand(TEXT("PRTGI.DumpSurfels"),
            TEXT("Dump local Surfel normal -X irradiance to Saved/PRTSurfelValidation/surfel_probes.csv."),
            FConsoleCommandWithArgsDelegate::CreateRaw(this,&FPRTGIModule::DumpSurfelProbes),ECVF_Default));
        ConsoleObjects.Add(Console.RegisterConsoleCommand(TEXT("PRTGI.ValidateLocalDirection"),
            TEXT("Diagnostic fixed-sun captures on a local back-wall column: <index> <resolution> [nearClip]. Writes CSV only to Saved."),
            FConsoleCommandWithArgsDelegate::CreateRaw(this, &FPRTGIModule::ValidateLocalDirection), ECVF_Default));
        ConsoleObjects.Add(Console.RegisterConsoleCommand(TEXT("PRTGI.BakeLocal"),
            TEXT("Bake one local volume by zero-based index, retaining the global cache."),
            FConsoleCommandWithArgsDelegate::CreateLambda([this](const TArray<FString>& Args)
            {
                RequestLocalBake(Args.Num() ? FCString::Atoi(*Args[0]) : 0);
            }), ECVF_Default));
        ConsoleObjects.Add(Console.RegisterConsoleCommand(
            TEXT("PRTGI.ToggleIndirectOnly"),
            TEXT("Toggle PRT indirect diffuse visualization (Ctrl+Alt+4)."),
            FConsoleCommandDelegate::CreateStatic(&PRTGI::ToggleIndirectOnly), ECVF_Default));
        ConsoleObjects.Add(Console.RegisterConsoleCommand(
            TEXT("PRTGI.SetMode"),
            TEXT("PRTGI.SetMode Offline|PRT|Lumen"),
            FConsoleCommandWithArgsDelegate::CreateRaw(this, &FPRTGIModule::ConsoleSetMode),
            ECVF_Default));
        ConsoleObjects.Add(Console.RegisterConsoleCommand(
            TEXT("PRTGI.Bake"),
            TEXT("Build legacy global light-independent directional PRT transport from the current scene. Does not build Actor Volumes."),
            FConsoleCommandDelegate::CreateRaw(this, &FPRTGIModule::RequestBake),
            ECVF_Default));
        ConsoleObjects.Add(Console.RegisterConsoleCommand(
            TEXT("PRTGI.RotateLight"),
            TEXT("PRTGI.RotateLight <YawDelta> [PitchDelta]"),
            FConsoleCommandWithArgsDelegate::CreateRaw(this, &FPRTGIModule::ConsoleRotateLight),
            ECVF_Default));
        ConsoleObjects.Add(Console.RegisterConsoleCommand(
            TEXT("PRTGI.Status"),
            TEXT("Print the current PRTGI status."),
            FConsoleCommandDelegate::CreateLambda([this]()
            {
                UE_LOG(LogPRTGI, Display, TEXT("%s"), *GetStatus());
            }),
            ECVF_Default));
    }

    void UnregisterConsoleCommands()
    {
        for (IConsoleObject* ConsoleObject : ConsoleObjects)
        {
            if (ConsoleObject)
            {
                IConsoleManager::Get().UnregisterConsoleObject(ConsoleObject, false);
            }
        }
        ConsoleObjects.Reset();
    }

    void ConsoleSetMode(const TArray<FString>& Args)
    {
        if (Args.IsEmpty())
        {
            UE_LOG(LogPRTGI, Display, TEXT("Usage: PRTGI.SetMode Offline|PRT|Lumen"));
            return;
        }
        if (Args[0].Equals(TEXT("Offline"), ESearchCase::IgnoreCase))
        {
            SetMode(PRTGI::EMode::Offline);
        }
        else if (Args[0].Equals(TEXT("PRT"), ESearchCase::IgnoreCase))
        {
            SetMode(PRTGI::EMode::PRT);
        }
        else if (Args[0].Equals(TEXT("Lumen"), ESearchCase::IgnoreCase))
        {
            SetMode(PRTGI::EMode::Lumen);
        }
        else
        {
            UE_LOG(LogPRTGI, Warning, TEXT("Unknown mode: %s"), *Args[0]);
        }
    }

    void ConsoleRotateLight(const TArray<FString>& Args)
    {
        const float Yaw = Args.Num() > 0 ? FCString::Atof(*Args[0]) : 0.0f;
        const float Pitch = Args.Num() > 1 ? FCString::Atof(*Args[1]) : 0.0f;
        RotatePrimaryDirectionalLight(Yaw, Pitch);
    }

private:
    static FPRTGIModule* GPRTGIModule;

    FDelegateHandle RenderDelegateHandle;
    FDelegateHandle WorldCleanupHandle;
    FTSTicker::FDelegateHandle TickHandle;
    TArray<IConsoleObject*> ConsoleObjects;
    TUniquePtr<FPRTGIVolumeManager> ActorVolumes;
    TUniquePtr<PRTGI::FRuntimeGI> RuntimeGI;
    bool bWasRuntimeGI=false;
    TWeakObjectPtr<UWorld> ActorVolumeWorld;
    TArray<FPRTGIRenderVolume> ActorRenderVolumes;
    TArray<TSharedPtr<const FPRTGIProbeVisibility,ESPMode::ThreadSafe>> VisibilitySources_RenderThread;
    TRefCountPtr<FRDGPooledBuffer> VisibilityBuffer_RenderThread;
    TRefCountPtr<FRDGPooledBuffer> DepthMomentsBuffer_RenderThread;
    TSharedPtr<const PRTGI::FMultiBounceData,ESPMode::ThreadSafe> ContactGeometry_RenderThread;
    TRefCountPtr<FRDGPooledBuffer> ContactTriangles_RenderThread,ContactNodes_RenderThread;
    const FScene* ActorRenderScene = nullptr;
    bool bActorRenderMode = false;
    ULineBatchComponent* ProbeVisualization = nullptr; // Explicit root, released on world change/shutdown.
    double NextProbeVisualizationSeconds = 0.0;
    int32 DebugGlobalProbes = 0;
    int32 DebugLocalProbes = 0;
    int32 DebugInvalidProbes = 0;
    int32 DebugUnknownProbes = 0;

    TSharedPtr<const FPRTGIRenderData, ESPMode::ThreadSafe> RenderData_RenderThread;
    TSharedPtr<const FPRTGIRenderData, ESPMode::ThreadSafe> PublishedRenderData_GameThread;
    TSharedPtr<const FPRTGITransportData, ESPMode::ThreadSafe> PublishedTransport_GameThread;
    TArray<TSharedPtr<const FPRTGITransportData, ESPMode::ThreadSafe>> LocalTransports;
    TArray<TSharedPtr<PRTGI::FSurfelVolume>> LocalSurfels;
    TSharedPtr<PRTGI::FSurfelVolume> SurfelBake;
    TWeakObjectPtr<UWorld> SurfelBakeWorld;
    int32 SurfelBakeIndex = INDEX_NONE;
    int32 PublishedSurfelBackend = -1;
    float PublishedSurfelBias = -1.0f;
    TArray<float> ActiveProbeValidity;
    int32 ActiveBakeLocalIndex = INDEX_NONE;

    TObjectPtr<UTextureRenderTargetCube> CaptureTarget = nullptr;
    TObjectPtr<USceneCaptureComponentCube> CaptureComponent = nullptr;
    TWeakObjectPtr<UWorld> ActiveWorld;
    TWeakObjectPtr<ADirectionalLight> ActiveLight;
    TWeakObjectPtr<UWorld> ValidatedWorld;
    TArray<FVector4f> ActiveBaselineCoefficients;
    TArray<FVector4f> ActiveTransportCoefficients;
    TArray<FVector3f> ActiveSampleDirections;
    TArray<float> ActiveProjectionWeights;
    FBox ActiveBounds = FBox(EForceInit::ForceInit);
    FIntVector ActiveGridCounts = FIntVector::ZeroValue;
    FPRTGILightState PublishedLightState;
    FPRTGILightState LastObservedLightState;
    uint64 ActiveSceneSignature = 0;
    int32 ActiveDirectionalSampleCount = 0;
    float ActiveReferenceIntensity = 100000.0f;
    EComponentMobility::Type OriginalLightMobility = EComponentMobility::Movable;
    TArray<FPRTGIMobilityRestoreState> OriginalAttachedMobilities;
    bool bOwnsBakeLightMobility = false;
    bool bHasObservedLight = false;
    FString LoadedMapPackage;
    FString LoadedDataFile;
    TMap<FString, float> MapOverridePreviousValues;
    bool bBakeRequested = false;
    int32 NextBakeJob = 0;
    int32 LastProgressLogJob = 0;
    double NextSignatureCheckSeconds = 0.0;
    EBakeState BakeState = EBakeState::Idle;
    FString LastError;

    friend void PRTGI::SetMode(PRTGI::EMode Mode);
    friend void PRTGI::RequestBake();
    friend void PRTGI::BuildVolume(APRTProbeVolume*);
    friend void PRTGI::CancelVolumeBuild(APRTProbeVolume*);
    friend void PRTGI::RotatePrimaryDirectionalLight(float YawDeltaDegrees, float PitchDeltaDegrees);
    friend FString PRTGI::GetStatus();
};

FPRTGIModule* FPRTGIModule::GPRTGIModule = nullptr;

namespace PRTGI
{
#if WITH_EDITOR
    FString GetVolumeGeometrySignature(APRTProbeVolume* V)
    {
        FBox Grid,Receiver;FIntVector Counts;FString Error;
        if(!V || !V->GetLayout(Grid,Receiver,Counts,Error))return FString();
        if(Counts==V->LegacyCounts && Grid.Min.Equals(V->LegacyGridMin,.001) && Grid.Max.Equals(V->LegacyGridMax,.001))
            Grid=FBox(V->LegacyGridMin,V->LegacyGridMax);
        return FString::Printf(TEXT("%llu"),ComputeSceneSignature(V->GetWorld(),nullptr,Grid,Counts,true,true,V->NearClipCm,V->CaptureResolution));
    }
    bool ImportRelocatedVolumeCache(APRTProbeVolume* V,const FString& SourcePath,const FString& SourceSignature)
    {
        if(!V || !V->GetWorld()->GetOutermost()->GetName().StartsWith(TEXT("/PRTGI/")))return false;
        uint64 Old=0;if(!LexTryParseString(Old,*SourceSignature))return false;
        auto Base=FSurfelVolume::Load(SourcePath,Old);if(!Base)return false;
        auto Multi=FMultiBounceData::Load(SourcePath+TEXT(".multi"),Old,V->TransportCellSize,MultiBounceBaseCRC(*Base));
        if(!Multi)return false;
        FBox Grid,Receiver;FIntVector Counts;FString Error;
        if(!V->GetLayout(Grid,Receiver,Counts,Error) || Counts!=Base->GridCounts
            || !FVector(Base->GridMin).Equals(Grid.Min,.001) || !FVector(Base->GridMax).Equals(Grid.Max,.001)
            || V->NearClipCm!=Base->CaptureNearClipCm || V->CaptureResolution!=Base->CaptureResolution)return false;
        uint64 New=0;if(!LexTryParseString(New,*GetVolumeGeometrySignature(V)))return false;
        const FString Destination=V->GetCachePath();
        if(IFileManager::Get().FileExists(*Destination) || IFileManager::Get().FileExists(*(Destination+TEXT(".multi"))))return false;
        Base->SceneSignature=New;Multi->Signature=New;
        // Samples, transport rows, geometry, validity and BaseCRC are unchanged.
        if(!Base->Save(Destination) || !Multi->Save(Destination+TEXT(".multi")))return false;
        V->Modify();V->LegacyCacheBase.Reset();return true;
    }
#endif
    void BuildVolume(APRTProbeVolume* Volume)
    {
        if (auto* Module=FPRTGIModule::GPRTGIModule)
            if (Module->ActorVolumes) Module->ActorVolumes->RequestBuild(Volume);
    }
    void CancelVolumeBuild(APRTProbeVolume* Volume)
    {
        if (auto* Module=FPRTGIModule::GPRTGIModule)
            if (Module->ActorVolumes) Module->ActorVolumes->CancelBuild(Volume);
    }
    void CycleBounces()
    {
        if(auto* Variable=IConsoleManager::Get().FindConsoleVariable(TEXT("r.PRTGI.Bounces")))
        {
            const int32 Current=Variable->GetInt();
            const int32 Count=Current<4?4:Current<8?8:Current<12?12:4;
            Variable->Set(Count,ECVF_SetByConsole);
            if(GEngine)GEngine->AddOnScreenDebugMessage(-1,3,FColor::Cyan,FString::Printf(TEXT("PRT: %d indirect bounces (requires built multi-bounce cache)"),Count));
        }
    }

    void ToggleProbes()
    {
        const bool bEnable = CVarPRTGIShowProbes.GetValueOnGameThread() == 0;
        CVarPRTGIShowProbes->Set(bEnable ? 1 : 0, ECVF_SetByConsole);
        UE_LOG(LogPRTGI, Display, TEXT("PRT probe markers: %s (cyan=global, orange=local, red=invalid, gray=no cache)"),
            bEnable ? TEXT("ON") : TEXT("OFF"));
        if (GEngine)
            GEngine->AddOnScreenDebugMessage(0x505250, 5.0f, FColor::Cyan,
                bEnable ? TEXT("Probes ON [Ctrl+Alt+P]: Volume Display Mode controls positions / SH lighting")
                    : TEXT("Probes OFF [Ctrl+Alt+P]"));
    }

    void ToggleIndirectOnly()
    {
        const bool bEnable = CVarPRTGIIndirectOnly.GetValueOnGameThread() == 0;
        if (bEnable && !IsPRTGIMode())
        {
            SetMode(EMode::PRT);
        }
        CVarPRTGIIndirectOnly->Set(bEnable ? 1 : 0, ECVF_SetByConsole);
        UE_LOG(LogPRTGI, Display, TEXT("PRT indirect-only view: %s"), bEnable ? TEXT("ON") : TEXT("OFF"));
        if (GEngine)
        {
            GEngine->AddOnScreenDebugMessage(0x505254, 3.0f, FColor::Cyan,
                bEnable ? TEXT("PRT indirect only [Ctrl+Alt+4]") : TEXT("PRT normal view"));
        }
    }

    void SetMode(const EMode Mode)
    {
        if (FPRTGIModule::GPRTGIModule)
        {
            FPRTGIModule::GPRTGIModule->SetMode(Mode);
        }
    }

    void RequestBake()
    {
        if (FPRTGIModule::GPRTGIModule)
        {
            FPRTGIModule::GPRTGIModule->RequestBake();
        }
    }

    void RotatePrimaryDirectionalLight(const float YawDeltaDegrees, const float PitchDeltaDegrees)
    {
        if (FPRTGIModule::GPRTGIModule)
        {
            FPRTGIModule::GPRTGIModule->RotatePrimaryDirectionalLight(YawDeltaDegrees, PitchDeltaDegrees);
        }
    }

    FString GetStatus()
    {
        return FPRTGIModule::GPRTGIModule
            ? FPRTGIModule::GPRTGIModule->GetStatus()
            : TEXT("PRTGI module is not loaded");
    }
}

IMPLEMENT_MODULE(FPRTGIModule, PRTGI)
