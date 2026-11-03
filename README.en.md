# PRTGI for Unreal Engine 6

[한국어](README.md) | **English** | [简体中文](README.zh-CN.md)

[User guide · GitHub Wiki](https://github.com/mazelines/UE5-PRT-GI/wiki)

This `feat/runtime-surfel-gi` branch targets **Unreal Engine 6.0**, with **PC validation first and mobile expansion later**. It retains the static PRT plugin and adds an opt-in GPU runtime transport reference for changing furniture layouts. The new path currently uses triangle BVH traversal, not Surfel Radiance Cascades. See the [runtime guide and limitations (Korean)](docs/runtime-gi-pc.md). The remaining setup and cache instructions below describe the existing static PRT path.

![PRT GI Cornell Box showcase](Plugins/PRTGI/docs/images/prt-gi-showcase-ue6-cfcd80c5.png)

## Experimental changes from the fork baseline

This `ue6` branch forks [wuzhy66/UE5-PRT-GI](https://github.com/wuzhy66/UE5-PRT-GI) to investigate quality and runtime changes in UE6. The comparison below uses the imported [upstream commit `69e08cf`](https://github.com/wuzhy66/UE5-PRT-GI/tree/69e08cfcdc3bb5c5ca1e83169c81ec95f47a67a9), not subsequent upstream development. **Probe/Surfel precomputation, GPU directional-light relighting, 1–4 indirect bounces, independent volumes, capture audits and near-clip safety already existed upstream.**

| Area | Fork baseline | Changes in this UE6 branch |
|---|---|---|
| Runtime Surfel surface pool | No runtime instance-owned GPU pool or spatial index | Added shared local seeds, per-instance GPU slots, move/duplicate/delete updates and a multilevel spatial hash. Audited 18,312 Showcase samples. The pool-only option does not change lighting. [Details and limits](docs/runtime-gi-pc.md) |
| GPU lighting cache experiment | No iterative runtime Surfel lighting | Added triangle-bound irradiance, double-buffered Jacobi sweeps, generation reset/rejection and probe SH projection. All 19 PRTGI tests pass. The roughly 10.5% difference from reference SH remains; disabled by default. [Method and measurements](docs/runtime-gi-pc.md) |
| GPU submission and pending lighting | No runtime cache scheduler | Group 8 cache dispatches with GPU fence completion. Median publication latency fell from 6.491 to 1.299 seconds versus one dispatch, with identical SH. Keep old SH only while geometry/probe layout is unchanged. GPU cost in update frames increases. [Measurements](docs/runtime-gi-pc.md) |
| Runtime resolve | No runtime layout-change path | Added any-hit BVH visibility and optional half-resolution irradiance with depth/normal/volume checks and full-resolution fallback. Close views improved but the wide view regressed, so half resolution remains disabled by default. [Measurements](docs/runtime-gi-pc.md) |
| UE6 integration | UE 5.8 plugin with numeric GI mode selection | Added UE6 host project/targets and engine-enum mode selection; fixed PRT failing to activate after precomputation in UE6 |
| Secondary surface lookup | Spatial search for nearby captured Surfel clusters at ray hits | Bound captured samples and transport sources to triangles; separated geometric/shading normals and added numerical ray-origin offsets to reduce incorrect associations and self-intersections |
| Material lookup | Reused captured Surfel material values via nearby source lookup | Added per-triangle 16×16 charts preserving original UVs and vertex colors, with base color, metallic and tangent-space normals; added interior coverage auditing and restricted retries |
| Receiver diffuse energy | No corresponding GGX energy correction at the final PRT receiver | Used GBuffer specular/roughness with an analytic GGX approximation to reserve energy for the specular lobe; a single-GGX receiver approximation, not glossy transport |
| GPU coefficients | Read relit coefficients back to the CPU before rendering | Added GPU-resident SH buffers and a publication pass; CPU readback is used for diagnostics/probe display. This structural change alone does not establish an overall frame-time improvement |
| Visibility/contact | First-hit-plane probe visibility | Added comparison modes for depth moments and static BVH segments, plus short-range contact occlusion. Plane visibility remains the default; depth moments increased wall artifacts and were not adopted as the default |
| Curved-receiver interpolation | Immediately rejected probes behind the receiving surface, creating discontinuous contributions at the boundary | Enabled the branch's existing `smoothstep` transition by changing `r.PRTGI.HemisphereTransition` from 0 to 0.2. Fades contributions over 20% of the smallest probe spacing to reduce circular boundaries on spheres. Behind-surface rejection and wall visibility remain; this setting change requires no rebake |
| Bounce count | Maximum 4, default 4 | Bake up to 12, runtime default 6, and 4/8/12 comparison. After four orders, terminate paths whose maximum RGB cumulative throughput falls below 0.01; changing the threshold requires rebuilding |
| Sampling and contact cost | One transport path per captured sample | Optional multi-path transport, independent-pilot adaptive allocation and half-resolution contact occlusion informed by SurfelPlus. Defaults unchanged; local validation did not establish an adaptive advantage. [Results](Plugins/PRTGI/Tools/validation_surfelplus.json) |
| Diagnostics | Existing cache/transport checks | Added v8 caches with legacy readers, accepted/effective probe count and dominant-weight diagnostics, per-order path/termination/material-hit counters, linear HDR comparison tools, and expanded GPU/CPU validation and automation tests. [Curved-surface validation](Plugins/PRTGI/Tools/validation_curved_surface.json) adds same-view before/after captures, visibility/contact ablations and a local discontinuity metric; this metric is not error against a path-traced reference |

In local showcase validation, all six v7 caches had zero unmapped secondary hits, GPU/CPU relative coefficient error stayed below `3.75e-5`, and five focused tests passed. A ClassicBox cutoff control reduced transport entries by 5.76% and cache size by 6.71%, with a 0.040% decrease in summed 12-order probe L0 energy. **These are comparisons between settings and cache stages within this branch, not a matched end-to-end quality/performance benchmark against upstream.** Conditions and raw results are in [surface/contact validation](Plugins/PRTGI/Tools/validation_showcase.json) and [bounce/cutoff validation](Plugins/PRTGI/Tools/validation_bounces.json).

These additions are experimental. Transport remains Lambertian for static scenes driven by one directional light; dynamic occluders, sky/emissive transport, full Substrate layering and specular GI are unsupported. The biased cutoff does not bound image error. Contact occlusion adds per-frame GPU cost, and 12-order caches can be larger than upstream four-order caches. Existing MetalBox/ShinyBox capture warnings remain.

Distribution also changed: Korean is the default documentation language, and generated caches are excluded from Git. On first opening the project, run **Build Precomputation** yourself as described below.

## Installation

> **First run: manually build the precomputation cache.** Precomputed `.prtgi`, `.surfels` and `.surfels.multi` caches are **not included in Git**. Open the project in the UE6 Editor, load `PRTGI_Showcase`, select all six `PRTProbeVolume` actors, then run **Details → PRT → Build → Build Precomputation**. Wait for every volume to finish before comparing PRT lighting. **Compiling the C++ project or simply opening it does not generate these caches.** Other levels also require local precomputation for each participating volume.

The repository now contains the `UE_PRT_GI.uproject` host project, with the plugin under `Plugins/PRTGI`. Both Game and Editor targets use `BuildSettingsVersion.V9` and `EngineIncludeOrderVersion.Unreal6_0`. This branch has been built and tested in the UE6 Editor; the [bounce validation report](Plugins/PRTGI/Tools/validation_bounces.json) records the measured conditions and results.

1. To use the bundled host project, associate `UE_PRT_GI.uproject` with your local **UE 6.0** installation, then open it. The checked-in engine association identifies the development machine’s engine installation. To install the plugin in another project, copy this repository's `Plugins/PRTGI` directory to `YourProject/Plugins/PRTGI`. This directory must directly contain `PRTGI.uplugin`, without an extra nested repository folder.
2. Build the **UE_PRT_GIEditor** target with **UE 6.0**, **Win64**, and **Development Editor**. When installing the plugin in another project, build that project’s Editor target with UE6. Blueprint projects also need an environment capable of compiling this C++ plugin; add a C++ class first if needed to generate a project target.
3. In **Edit → Plugins**, search for **PRT** (the current display name is **UE5-PRT-GI**), enable it, and restart the Editor when prompted.

This branch is validated in **Unreal Engine 6.0, Windows / Win64, Development Editor, DX12 / SM6**, using deferred rendering. The GI pass requires the GBuffer; Forward Shading is not supported by this path. The plugin references internal Renderer interfaces, so other UE versions and platforms require recompilation and validation. Precompiled DLLs are not guaranteed to be compatible across Engine builds. The current implementation does not require additional Engine source modifications to install the plugin.

## Quick Start

1. Enable **Show Plugin Content** in the Content Browser.
2. Open **PRTGI Content → Maps → PRTGI_Showcase**, asset path `/PRTGI/Maps/PRTGI_Showcase`.
3. Select the six `PRTProbeVolume` actors and run **Build Precomputation**; wait for completion.
4. Click the Editor viewport or PIE game view, then press **Ctrl+Alt+2** to enable PRT and **Ctrl+Alt+3** to compare it with Lumen.
5. Press **Ctrl+Alt+4** to inspect PRT indirect lighting only, and **Ctrl+Alt+P** to view probes.
6. Press **Ctrl+Alt+[ / ]** to rotate the sun and observe the relighting results.

This branch uses UE6 Renderer interfaces. Comparing against hardware ray tracing Lumen requires DX12 / SM6, Ray Tracing, and the relevant support settings in the project. These are host project rendering settings; the plugin does not forcibly override the project's rendering configuration.

When comparing against hardware ray tracing Lumen, enable **Support Hardware Ray Tracing**, **Support Compute Skin Cache**, and **Use Hardware Ray Tracing when available**, and use a supported GPU. Restart the Editor after changing settings that require it. See Epic's [Lumen Technical Details](https://dev.epicgames.com/documentation/en-us/unreal-engine/lumen-technical-details-in-unreal-engine).

The bundled showcase's default map profile temporarily sets `r.Lumen.HardwareRayTracing=1`, `r.Lumen.ScreenProbeGather.ScreenTraces=0`, and `r.EyeAdaptation.CachedLightingPreExposure=8`. Previous values are restored when leaving the map. Its Lumen comparison therefore has diffuse screen traces disabled; it is not the default Lumen configuration. These overrides do not enable missing project-level hardware ray tracing support.

The UE6 host settings are in [Config/DefaultEngine.ini](Config/DefaultEngine.ini): Plugin GI uses `r.DynamicGlobalIlluminationMethod=2`, reflections use `r.ReflectionMethod=0`, and Substrate is enabled with `r.Substrate.ProjectGBufferFormat=0`. Use `PRTGI.SetMode PRT` to select Plugin GI at runtime; the command resolves the engine enum. The runtime PRT default is **6 bounces**, and generated caches remain local. When installing elsewhere, merge the relevant settings into that UE6 project.

The showcase uses a single shared UE DirectionalLight. The original area-light regions of the boxes have been replaced with openings, and the front openings are retained for viewing.

| Volume | Local validation settings/results |
|---|---|
| PRT_Global, PRT_ClassicBox, PRT_ColorBox, PRT_InvertedBox | Built for 1–12 indirect bounces; defaults to 6 at runtime |
| PRT_ShinyBox | Built at 16×16 per face; 21 of 878 audited probes excluded with a capture warning |
| PRT_MetallicBox | Built at 16×16 per face, with 40 cm probe spacing and a 2 cm transport cell size; 77 of 2,212 audited probes excluded with a capture warning |

Caches are not bundled. Select all six showcase `PRTProbeVolume` actors and run **Build Precomputation**, then wait for completion before comparing lighting. Locally generated caches must still be included when packaging a playable build.

## Adding a Probe Volume to Your Scene

1. Save the level and ensure that the geometry and materials participating in precomputation remain static.
2. Add a UE **DirectionalLight** and set its Mobility to **Movable** for real-time rotation. If there are multiple directional lights, add the Actor Tag `PRTGI.PrimaryLight` to the intended light.
3. Search for **PRT Probe Volume** in Place Actors and drag it into the scene.
4. Configure the volume bounds, probe spacing, Near Clip, and Priority.
5. Enable PRT GI, then click **Build Precomputation** under the Actor's Details → **PRT → Build**.
6. Wait for completion, check **Cache Status**, **GI Active**, **Effective Bounces**, and **Build Warnings** under **PRT → Status**, then save the level.

Volumes contribute indirect lighting and display probes only in **PRT GI mode**. Switching to Lumen disables the PRT GI contribution.

### Parameters

| Parameter | Description |
|---|---|
| Enabled | Whether this volume is enabled |
| Extent | Half-size along each axis, in cm; Actor Scale also affects the bounds |
| Probe Spacing | Target maximum spacing along each axis, in cm; see Actual Spacing for the resulting uniform spacing |
| Probe Inset | Distance by which the probe grid is inset from the volume boundaries, in cm |
| Priority | Use a higher priority for locally dense regions |
| Near Clip Cm | Near clipping distance for probe captures, default 1 cm, range 0.01–10 cm; separate from the main camera's Near Clip |
| Capture Resolution | Resolution of each capture face, default 16, range 4–32; increasing it raises capture cost |
| Transport Cell Size | Static surface aggregation size, default 2 cm, range 1–100 cm; smaller values generally increase data size and computation |
| Blend Face / Distance | One transition face and its distance in cm: 0=none, 1/2=−/+X, 3/4=−/+Y, 5/6=−/+Z; configure only at an opening where a transition is needed |
| Display Mode | Hidden, Positions, or Lighting; Lighting shows each probe's lighting color |
| Probe Radius | Radius of the visualization spheres, in cm; does not change the actual sampling positions |

Volumes are currently **axis-aligned** and do not support rotation. Each axis supports up to 32 probes. Increase the spacing or split the volume if this limit is exceeded.

There is also a separate **4,000,000 capture sample budget** per volume: `ProbeCounts.X × ProbeCounts.Y × ProbeCounts.Z × 6 × CaptureResolution²` must not exceed this limit. At the default resolution of 16, this allows at most 2,604 probes in total, subject to the per-axis limit. Reduce capture resolution, increase probe spacing, or split the volume if the budget is exceeded.

A starting point is Priority=0 with roughly 200 cm spacing for a global volume, and Priority=10 with roughly 30–40 cm spacing for local volumes. Adjust these values to your scene's dimensions. Avoid placing probes inside walls or configuring blend transitions across solid walls. An excessively large Near Clip can clip geometry close to a probe, causing incorrect visibility or light leaks.

### Precomputation and Progress

- Select one or more volumes and click **Build Precomputation** to queue their builds. Use **Cancel Build** to cancel builds for the selected volumes.
- Under **PRT → Build**, **Build Progress** covers geometry capture, transport computation, and saving. **Build Stage** describes the current operation; both appear alongside the build buttons. An existing valid geometry cache is reused, so a build may run only the transport stage. Progress is weighted by stage and is not an estimate of remaining time.
- The current build combines UE scene capture with CPU transport precomputation. Runtime relighting uses the GPU by default; the entire build is not a GPU-only bake.
- A build saves the data needed to switch between 1–12 indirect bounces. Switching between cached bounce counts at runtime does not require recapturing the scene. The default is 6 bounces.
- Build time depends on probe count, capture resolution, geometry complexity, transport cell size, and hardware. Validate a small volume first, then expand the coverage.
- Rebuild after changing static geometry, materials, the probe grid layout, or capture parameters. Duplicating a volume generates a new GUID, so the duplicate needs its own cache.

Probes with geometry audit mismatches or empty captures are excluded from transport and interpolation. A build can finish with a warning if the excluded count is at most 5% of audited probes and the remaining data passes validation; larger failures stop the build. Inspect **Build Warnings**, **Invalid Probes**, and the affected area before using the result. The capture audit uses collision queries on the **Visibility** channel, so participating geometry needs collision that matches its rendered surfaces and blocks this channel.

Each volume is limited to **24 million transport entries**, at 64 bytes per entry, for up to approximately **1464.8 MiB** of transport data. The file loading limit allows an additional 384 MiB for geometry indices and other data. This is not a total memory budget: temporary build copies, CPU caches, and GPU buffers require additional memory. Capture resolution is not automatically reduced when approaching the limit.

## Content Layout

The paths below are relative to the plugin directory, `Plugins/PRTGI`.

- `Content/Maps`: Showcase level.
- `Content/Showcase/Materials`, `Meshes`, etc.: Copies of project assets required by the showcase, with references redirected to assets inside the plugin.
- `Content/Materials`: Probe visualization and geometry capture materials.
- `Content/Precomputed/Volumes`: Raw `.surfels` / `.multi` caches, separated by level package name and volume GUID; they do not contain light parameters.

The `.surfels` file stores captured geometry data, and its paired `.multi` file stores multi-bounce transport data. Keep both files together. These are raw binary files and do not appear as `.uasset` files in the Content Browser. The caches are not final images for a fixed sun angle: at runtime, the plugin still computes the current light input and occlusion before applying the precomputed transport.

For plugin levels under `/PRTGI/`, caches are saved inside the plugin at `Content/Precomputed/Volumes/<level-package>/<GUID>.prtgi.surfels`, with a companion `<GUID>.prtgi.surfels.multi`. Other levels, including your own `/Game/` levels, save to `YourProject/Content/PRTGI/Volumes/<level-package>/` instead. Slashes in the level package name are replaced with underscores. Moving or renaming a level changes its cache path, so check **Cache File** and rebuild if needed.

The plugin's build rules stage its own `Content/Precomputed` directory as loose **NonUFS** files. For packaged projects using `/Game/` levels, also stage `Content/PRTGI/Volumes` via **Additional Non-Asset Directories To Copy** (path relative to project Content: `PRTGI/Volumes`). Verify cache loading in the packaged build; the included validation report covers Editor usage.

## Current Limitations

- PRT computes indirect diffuse lighting driven by one selected DirectionalLight. Point, spot, and rect lights, SkyLight, and emissive materials do not drive this PRT transport; specular GI and reflections are not supplied by the plugin.
- Geometry and materials used by the transport must remain static. Runtime light changes reuse cached geometry for GPU occlusion; moving occluders are not incorporated into that cached transport automatically.
- Curved surfaces can show probe-related rings, and brightness can differ from Lumen. Check the results in your scene before relying on the lighting comparison.

## Keyboard Shortcuts

Click the Editor / PIE viewport first to give it focus.

| Shortcut | Action |
|---|---|
| Ctrl+Alt+1 | Disable dynamic GI; this showcase has no offline baked lighting |
| Ctrl+Alt+2 | PRT GI |
| Ctrl+Alt+3 | Lumen GI |
| Ctrl+Alt+4 | Toggle PRT indirect diffuse only / full view |
| Ctrl+Alt+P | Show / hide probes; the volume's Display Mode determines whether they appear as position markers or lighting spheres |
| Ctrl+Alt+G | Cycle through 4 / 8 / 12 indirect bounces; requires the corresponding cache |
| Ctrl+Alt+[ / ] | Decrease / increase DirectionalLight Yaw by 5° (default step) |
| Ctrl+Alt+Shift+[ / ] | Decrease / increase DirectionalLight Pitch by 5° (default step) |
| Ctrl+Alt+B | Legacy global bake command; does not build Actor Volumes |

For local precomputation, select the corresponding `PRTProbeVolume` and click **Build Precomputation**. The default is 6 indirect bounces.

If focus or keyboard layout interferes with the shortcuts, run these commands individually in the UE Console:

```text
PRTGI.SetMode PRT
PRTGI.SetMode Lumen
PRTGI.ToggleIndirectOnly
r.PRTGI.ShowProbes 1
r.PRTGI.Bounces 4
PRTGI.RotateLight 5 0
PRTGI.Status
```

`PRTGI.SetMode Offline` is a legacy command name for exiting dynamic GI. It does not automatically generate offline lighting or a Path Tracing reference image.

## Surfel Quality Diagnostics

Since v6, transport caches bind captured samples to triangles, separate geometric and shading normals, and evaluate secondary hits using per-triangle material charts with original UVs and vertex colors. The bake includes base color, metallic and tangent-space normals. Transport remains Lambertian. Receiver shading uses GBuffer specular/roughness and UE's analytic GGX energy approximation with multiple-scattering compensation to reserve energy for the specular lobe. This receiver correction approximates a single GGX material; it does not implement all Substrate layers, glossy transport, dynamic occluders, sky lighting or emissive transport. Existing v3–v6 caches remain readable and retain their version on save. Rebuild every participating volume to adopt v8 consistently.

The material baker checks every interior chart texel. If primitive transforms cause raster clipping, only materials proven independent of vertex/world data may retry without primitive data. Other failures stop the bake and retain the previous cache. Charts use 16×16 tiles per triangle and therefore do not preserve arbitrarily fine texture detail.

`PRTGI.DumpQuality` writes cache versions, material-hit coverage, per-bounce missing-hit statistics and GPU residency to `Saved/PRTGIQuality.json`. Missing-hit fractions are not energy-loss fractions. For GPU/CPU validation, set `r.PRTGI.GPUReadback 1`, allow relighting to finish, then run `PRTGI.ValidateMultiBounce`. It writes `Saved/PRTMultiBounceValidation.json` and may briefly block the editor. Restore `GPUReadback 0` afterward. Normal rendering keeps coefficients on the GPU; readback is needed only for diagnostics and probe visualization.

```text
python Plugins/PRTGI/Tools/quality_report.py --output Saved/PRTGIQuality/caches.json Plugins/PRTGI/Content/Precomputed/Volumes
python -m pip install numpy OpenEXR
python Plugins/PRTGI/Tools/compare_hdr.py reference.exr candidate.exr --roi 230 150 708 570
```

The HDR tool compares aligned linear RGB without exposure fitting. Use `PreTonemapHDRColor`, identical camera/resolution/light inputs and bounce limits. For diffuse indirect-only comparisons, disable the DirectLighting show flag and exclude indirect specular in a temporary post-process volume while keeping the Specular show flag enabled. The remaining reference can still include mixed diffuse/specular paths, so it is not an exact oracle for Lambertian PRT. Disable emissive sources and the denoiser for this test; ensure the path-intensity clamp does not clip the reference. The showcase reference uses 512 samples, four bounces, unit pre-exposure and `r.PathTracing.MaxPathIntensity 65504`.

| Console variable | Default | Meaning |
|---|---:|---|
| `r.PRTGI.GPUResident` | 1 | Keep relit coefficients on the GPU; 0 uses readback/upload |
| `r.PRTGI.GPUReadback` | 0 | Retain CPU coefficients for numeric diagnostics |
| `r.PRTGI.ProbeVisibility` | 1 | 0 disabled, 1 first-hit planes, 2 experimental depth moments, 3 static BVH segments |
| `r.PRTGI.VisibilityBiasCm` | 1 | Receiver visibility bias in cm |
| `r.PRTGI.HemisphereTransition` | 0.2 | Positive-hemisphere transition as a fraction of the smallest probe spacing; 0 restores the legacy hard gate |
| `r.PRTGI.ContactSamples` | 0 | Optional static contact occlusion: 0–16 rays per pixel |
| `r.PRTGI.ContactRadiusCm` | 20 | Maximum contact ray distance |
| `r.PRTGI.ContactBiasCm` | 0.1 | Contact ray origin/intersection bias |
| `r.PRTGI.Debug` | 0 | 1 visibility, 2 interpolation support, 3 hemisphere acceptance, 4 contact visibility |

Optional experiments informed by [SurfelPlus](https://wangruipeng.com/SurfelPlus/), adapted to static precomputed transport:

- `r.PRTGI.Debug 5/6/7`: accepted probe count, effective count `(sum(w))²/sum(w²)`, and weight concentration. Counts reach green at 16; concentration is red when one probe dominates. These show the selected volume before boundary blending; legacy trilinear is magenta. Restore `Debug 0` afterwards.
- `r.PRTGI.ContactHalfResolution 1`: half-width/half-height contact traces, depth/normal-aware reconstruction and full-resolution fallback at edges or insufficient support. Default 0; no new history or rebake.
- Volume **Transport Paths Per Sample**: 1–16 production paths per capture sample, default 1. **Adaptive Transport Sampling**, default false, requires at least 2. Two separate pilot paths estimate a light-independent triangle/bounce diffuse-response variation proxy. Allocate at least one and at most four times the average production count per sample, preserving each probe's total production budget. Normalize each sample's contribution by its production count; discard pilot contributions.
- Compare uniform 4 against adaptive 2 plus 2 pilots for equal starting-path counts; actual ray work and time may differ. This heuristic does not guarantee lower error. Sampling changes require **Build Precomputation**. v8 stores settings and pilot/production counters; v3–v7 remain readable with uniform 1. Generated caches remain excluded from Git. These are not a port of SurfelPlus's full dynamic GI or reflection system.

Local results: full-resolution-relative HDR RMSE was 3.37% (wide) / 6.04% (sphere close-up). Five alternating RTX 4090 measurements gave combined PRT pass medians of 3.479 → 3.454 ms / 3.800 → 2.609 ms (TSR 1671×577 → 1853×639); no meaningful wide-view gain was established. An eight-probe fixture gave coefficient discrepancies of 8.89% (uniform 1), 2.87% (uniform 4), and 4.69% (adaptive 2 plus 2 pilots) against finite uniform 16. This shadow-disabled, four-light, fixed-seed comparison shares production paths with the reference and does not establish variance reduction. See [conditions, raw data and limitations](Plugins/PRTGI/Tools/validation_surfelplus.json). For diagnostic colors, use `r.PRTGI.IndirectOnly 1` and default exposure; restore debug/indirect-only to 0 afterwards.

Contact occlusion is a short-range diffuse approximation, with extra GPU cost and possible sampling noise. It does not replace transported radiance. Depth moments blend four neighboring capture samples and are experimental; they produced wall artifacts in the showcase. Exact BVH visibility also remains optional. Restore `IndirectOnly 0`, `Debug 0`, `ProbeVisibility 1`, `HemisphereTransition 0.2` after diagnostics.

Curved-receiver rings were reproduced when probes abruptly lost their contribution at the receiver tangent plane. The existing continuous transition now defaults to 0.2, fading support before rejection while retaining visibility tests; no rebake is needed. This conservative setting follows the continuous-weighting principle used by DDGI, not the complete RTXGI wrap weighting that also admits back-facing probes. Recent sources and controlled comparisons are recorded in [curved-surface validation](Plugins/PRTGI/Tools/validation_curved_surface.json). Earlier v6 and bounce metrics remain historical measurements with transition width 0.

The implementation follows the geometric/shading-normal and reconstruction guidance in [EA GIBS, SIGGRAPH 2024](https://www.advances.realtimerendering.com/s2024/content/EA-GIBS2/Apers_Advances-s2024_Shipping-Dynamic-GI.pdf). [SurfelPlus, SIGGRAPH 2025 poster](https://github.com/WANG-Ruipeng/SurfelPlus/blob/master/docs/files/Poster.pdf) and [DDGI](https://arxiv.org/abs/2009.10796) inform further comparisons; this plugin is not an implementation of their complete systems.

Earlier v6 showcase validation on UE6: all six v6 caches have zero unmapped secondary hits (previously 0.68–9.26%). At the fixed camera, RGB relative RMSE against the diffuse-component HDR reference changed from **23.24% / 32.61% / 24.84%** to **18.02% / 16.70% / 11.24%** for ClassicBox / ColorBox / contact-region ROIs with 8 contact samples. These are limited-region comparisons, not a claim of complete scene equivalence. GPU/CPU coefficient error is at most `3.75e-5`; five focused automation tests pass. MetalBox and ShinyBox retain their existing capture-audit exclusions.

On an RTX 4090, three static-frame measurements gave a median PRT apply cost of **0.499 ms** without contact correction and **2.725 ms** with 8 samples (1740×590 input, TSR to 2164×733). Total six-volume transport files grew from **1651 MiB to 2157 MiB**. The host project's quality preset enables 8 contact samples; the plugin default remains 0. Set `r.PRTGI.ContactSamples 0` for the lower-cost option. Detailed conditions, ROI coordinates, cache counters and remaining limitations are in [validation_showcase.json](Plugins/PRTGI/Tools/validation_showcase.json).

## Comparing 4, 8 and 12 bounces

Caches from v7 onward bake up to 12 orders once. Use `r.PRTGI.Bounces 4`, `8` or `12`, or **Ctrl+Alt+G** (4 → 8 → 12 → 4). The runtime default is 6. Old v3–v6 caches cap the effective count at 4 and report that they need rebuilding. Rebuild all participating volumes to compare higher orders.

**Transport Min Throughput** on each `PRTProbeVolume` defaults to **0.01**. After preserving the first four orders, the bake stops a path before its next trace when the largest RGB component of cumulative diffuse throughput falls below this threshold. Set 0 to disable this cutoff; the existing near-zero numerical guard remains. Changing the value invalidates the transport cache and requires **Build Precomputation**. This deterministic truncation is biased and is not a 1% radiance-error guarantee, especially with high-contrast lighting. It is independent of the current light direction or intensity.

Paths are not traced for 8 or 12 bounces every frame. Light or bounce-count changes re-evaluate source lighting and cached transport into SH coefficients. Static frames interpolate those coefficients; the separate contact-occlusion pass still runs per frame. All orders share one cache, so selecting 4 does not shrink the allocated cache, although GPU transport evaluation skips later sorted entries.

`PRTGI.DumpQuality` and `quality_report.py` include per-order paths, early termination counts, the maximum order and cutoff. `PRTGI.ValidateMultiBounce` now reports `bounce_counts: [1,4,8,12]` alongside its energy array (legacy caches cap the last two entries).

```text
python Plugins/PRTGI/Tools/compare_bounces.py reference12.exr b4.exr b8.exr b12.exr --roi classic 930 260 1190 510 --roi color 1490 330 1810 580 --output comparison.json
```

Use the linear `*_PreTonemapHDRColor.exr` files. For this comparison, use one 12-bounce reference, identical camera/light/exposure settings, and the same contact-occlusion setting for all candidates.

UE6 showcase results against the 12-bounce diffuse-component reference (RGB relative RMSE, contact samples 8):

| ROI | 4 bounces | 8 bounces | 12 bounces |
|---|---:|---:|---:|
| InvertedBox | 32.86% | 20.38% | 17.97% |
| ClassicBox | 21.06% | 19.23% | 19.12% |
| ColorBox | 17.47% | 16.04% | 16.33% |

These single-frame captures contain rendering noise; the small ColorBox difference does not establish that 12 is worse. Eight orders provide most of the improvement in ClassicBox and ColorBox, while InvertedBox benefits more from 12. On an RTX 4090, the six-volume cached transport evaluation took approximately **2.16 / 2.91 / 3.04 ms** at 4 / 8 / 12 (median of 2 / 3 / 3 complete relight samples). Source-light evaluation adds roughly 8.7–8.9 ms to these relight events. Static-frame profiles contained no transport dispatches; changing the maximum order does not add a per-frame path-tracing loop.

In a ClassicBox control bake, cutoff 0.01 reduced transport entries by **5.76%** and cache size by **6.71%** versus cutoff 0, with **0.040%** lower summed 12-order probe L0 energy. This is not an image-error guarantee. All six v7 caches total **3016.94 MiB**, have zero unmapped secondary hits, and passed CRC checks. GPU/CPU relative coefficient error remains below `3.75e-5`; all five focused tests passed. Existing MetalBox and ShinyBox capture warnings remain. See [validation_bounces.json](Plugins/PRTGI/Tools/validation_bounces.json) for ROI coordinates, conditions, raw timings and limitations.
