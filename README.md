# PRTGI — Unreal Engine 6

**한국어** | [English](README.en.md) | [简体中文](README.zh-CN.md)

[사용자 가이드 · GitHub Wiki](https://github.com/mazelines/UE5-PRT-GI/wiki)

`feat/runtime-surfel-gi` 브랜치는 **Unreal Engine 6.0**용 PRTGI를 가구 배치 변경에 대응하는 구조로 확장하는 실험 브랜치입니다. **PC 우선 검증 후 모바일 확장**을 목표로 하며, 기존 정적 PRT와 선택 가능한 [런타임 광전달 PC 기준 구현](docs/runtime-gi-pc.md)을 함께 제공합니다. 기존 경로의 Cornell Box 예제, 방향광 재조명, 1~12회 간접 반사, 독립 프로브 볼륨은 유지합니다.

![PRT GI Cornell Box 예제](Plugins/PRTGI/docs/images/prt-gi-showcase-ue6-cfcd80c5.png)

## 포크 원본 대비 실험적 개선

이 브랜치는 [wuzhy66/UE5-PRT-GI](https://github.com/wuzhy66/UE5-PRT-GI)를 포크해 UE6 환경에서 품질과 실행 경로를 실험한 버전입니다. 아래 비교 기준은 포크 당시의 [원본 커밋 `69e08cf`](https://github.com/wuzhy66/UE5-PRT-GI/tree/69e08cfcdc3bb5c5ca1e83169c81ec95f47a67a9)이며, 이후 원본 저장소의 변경 사항을 포함하지 않습니다. **프로브·Surfel 기반 사전 계산, GPU 방향광 재조명, 1~4회 간접 반사, 독립 볼륨, 캡처 감사와 근접 클리핑 보호는 원본에서 제공한 기능입니다.**

| 항목 | 포크 당시 원본 | 이 UE6 브랜치의 변경 |
|---|---|---|
| 런타임 Surfel 표면 풀 | 런타임 인스턴스 소유권·GPU 공간 인덱스 없음 | 공유 에셋 표본과 인스턴스별 GPU 슬롯, 이동·복제·삭제 갱신, 다단계 공간 해시 추가. Showcase 18,312개 표본의 GPU 대조 통과. 표면 풀 단독 옵션은 조명 출력을 바꾸지 않음. [구조·검증·제한](docs/runtime-gi-pc.md#gpu-surfel-풀과-공간-조회--2026-11-03) |
| GPU 조명 캐시 실험 | 런타임 Surfel 조명 반복 갱신 없음 | 삼각형에 연결된 irradiance 캐시, 이중 버퍼 Jacobi 반복, 세대별 초기화·폐기와 프로브 SH 출력을 추가. 자동화 19개 통과. 기본 설정의 기준 SH 차이 약 10.5%, 게시 지연 약 6.4초로 아직 기본 비활성 유지. [계산식·측정 결과](docs/runtime-gi-pc.md#gpu-조명-캐시-반복-갱신--2026-11-03) |
| 배치 변경 대응 실험 | 레벨의 정적 광전달 사전 계산 | 에셋 로컬 재질 차트를 재사용하고 가구 이동·복제·삭제 및 광원 변경 시 GPU에서 광전달을 갱신하는 선택 경로 추가. 기본 비활성. 현재는 삼각형 BVH 기준 구현이며 Surfel Radiance Cascade·모바일 구현은 후속 단계. [실행·제한 사항](docs/runtime-gi-pc.md) |
| 런타임 화면 합성 | 런타임 배치 변경 경로 없음 | BVH에서 첫 차폐물을 찾으면 종료하고, 깊이·법선·볼륨 검사로 원래 해상도 계산에 복귀하는 반해상도 간접광 옵션 추가. PC 근접 화면에서는 비용이 줄었지만 원경에서는 늘어 반해상도 기본값은 0 유지. [측정 조건·결과](docs/runtime-gi-pc.md#화면-합성-최적화-실험--2026-11-03) |
| UE6 실행 지원 | UE 5.8 대상 플러그인, 숫자로 지정한 GI 모드 | UE6 실행용 프로젝트·빌드 타깃을 구성하고 엔진의 GI 열거형을 사용하도록 수정. UE6에서 사전 계산 후에도 PRT 패스가 활성화되지 않던 문제를 해결 |
| 2차 표면 조회 | 광선 교차점 주변의 Surfel 클러스터를 공간 검색 | 캡처 표본과 전송 소스를 삼각형에 연결. 기하 법선·셰이딩 법선을 분리하고 수치 오차에 따른 광선 원점 보정을 추가해 잘못된 표면 연결과 자기 교차를 줄이는 실험 |
| 재질 조회 | 캡처된 Surfel의 재질값을 주변 소스 검색으로 재사용 | 실제로 맞힌 삼각형의 원래 UV·정점 색상을 유지한 16×16 재질 차트에서 기본색·금속성·접선 공간 법선을 조회. 차트 내부 캡처 검사와 제한적인 재시도를 추가 |
| 수신 표면의 확산 에너지 | 최종 PRT 수신 표면에 해당 GGX 에너지 보정 없음 | GBuffer의 반사율·거칠기와 해석적 GGX 근사를 이용해 정반사에 배분되는 에너지를 확산광에서 제외. 단일 GGX 수신 표면의 근사이며 광택 반사 전송을 추가한 것은 아님 |
| GPU 계수 유지 | 재조명 결과를 CPU로 읽은 뒤 렌더링에 전달 | SH 계수를 GPU 버퍼에 유지하는 경로와 계수 게시 패스를 추가. CPU 읽기는 진단·프로브 표시가 필요할 때 사용. 이 구조 변경 자체가 전체 프레임 성능 향상을 보장하지는 않음 |
| 가시성·접촉부 | 최초 교차 평면 기반 프로브 가시성 | 깊이 모멘트·정적 BVH 선분 검사 비교와 짧은 거리의 접촉 차폐를 추가. 기본 가시성은 평면 방식 유지. 깊이 모멘트는 벽 얼룩이 늘어 기본값으로 채택하지 않음 |
| 곡면의 프로브 보간 | 수신 표면 뒤쪽 프로브를 즉시 제외해 경계에서 기여도가 불연속적으로 변함 | 이 브랜치에 구현해 둔 `smoothstep` 전환을 기본 활성화하도록 `r.PRTGI.HemisphereTransition`을 0 → 0.2로 변경. 최소 프로브 간격의 20% 구간에서 기여도를 줄여 구체의 원형 경계를 완화. 뒤쪽 프로브 제외·벽 차폐 검사는 유지하며, 이 설정 변경에는 재사전 계산이 필요 없음 |
| 다중 바운스 | 최대 4회, 기본 4회 | 최대 12회 사전 계산, 런타임 기본 6회, 4·8·12회 비교. 처음 4회 이후 누적 RGB 반사율 최댓값이 0.01 미만이면 경로를 조기에 종료. 임계값 변경 시 재계산 필요 |
| 표본 배분·접촉 차폐 비용 | 캡처 표본당 단일 전송 경로 | SurfelPlus 자료를 참고해 다중 경로·예비 표본 기반 적응형 배분과 반해상도 접촉 차폐를 선택 옵션으로 추가. 기본값 유지. 작은 검증 장면에서는 균등 4개가 적응형 2개＋예비 2개보다 오차가 작았으며, 반해상도 성능 이득도 시점에 따라 달랐음. [실험 결과](Plugins/PRTGI/Tools/validation_surfelplus.json) |
| 진단·재현 | 기존 캐시·전송 검증 | v8 캐시와 구버전 읽기, 프로브 기여 수·유효 수·최대 정규화 가중치 진단, 단계별 경로·조기 종료·재질 조회 통계, 선형 HDR 비교 도구와 자동화 테스트를 확장. [곡면 검증](Plugins/PRTGI/Tools/validation_curved_surface.json)에 전후 이미지와 국소 불연속 지표를 추가. 해당 지표는 Path Tracing 기준 오차가 아님 |

로컬 Showcase 검증에서는 여섯 v7 캐시의 2차 표면 조회 누락이 모두 0이었고, GPU·CPU 계수 상대 오차는 `3.75e-5` 미만이며 관련 테스트 5개가 통과했습니다. ClassicBox의 조기 종료 대조 실험에서는 전송 항목이 5.76%, 캐시 용량이 6.71% 줄고 12회 프로브 L0 에너지 합이 0.040% 낮아졌습니다. **이는 이 브랜치 안에서 수행한 설정·캐시 단계별 비교이며, 원본 저장소와의 동일 조건 전체 성능·화질 비교는 아닙니다.** 조건과 원시 결과는 [표면·접촉 조명 검증](Plugins/PRTGI/Tools/validation_showcase.json)과 [바운스·조기 종료 검증](Plugins/PRTGI/Tools/validation_bounces.json)에 기록했습니다.

추가 기능은 실험 단계입니다. 전송은 정적 장면과 하나의 방향광에 대한 Lambert 확산 모델이며, 동적 차폐물·하늘광·발광 전송·전체 Substrate 레이어·정반사 GI를 지원하지 않습니다. 조기 종료는 편향된 근사로 화면 오차의 상한을 보장하지 않습니다. 접촉 차폐는 매 프레임 GPU 비용을 추가하고, 12회 캐시는 원본의 4회 캐시보다 커질 수 있습니다. MetalBox·ShinyBox의 기존 캡처 경고도 남아 있습니다.

배포 방식도 변경했습니다. 한국어를 기본 문서로 제공하고, 사전 계산 캐시를 Git에서 제외합니다. 프로젝트를 처음 열면 아래 절차에 따라 **Build Precomputation**을 직접 실행해야 합니다.

위의 캐시·지원 범위 설명과 아래의 기본 사용법은 **기존 정적 PRT 경로**를 기준으로 합니다. 런타임 실험 경로는 레벨 사전 계산 대신 에셋 준비 명령을 사용합니다. [런타임 실행 안내](docs/runtime-gi-pc.md)를 참고하세요.

## 설치

이 저장소는 실행용 프로젝트 `UE_PRT_GI.uproject`와 `Plugins/PRTGI`에 있는 플러그인으로 구성됩니다. Game·Editor 타깃 모두 `BuildSettingsVersion.V9`와 `EngineIncludeOrderVersion.Unreal6_0`을 사용합니다. 이 브랜치는 **UE 6.0, Windows / Win64, Development Editor, DX12 / SM6, 디퍼드 렌더링** 환경에서 빌드와 에디터 검증을 마쳤습니다. 측정 조건과 결과는 [바운스 검증 보고서](Plugins/PRTGI/Tools/validation_bounces.json)에 있습니다.

> **첫 실행 시 직접 프리빌드해야 합니다.** 사전 계산 캐시(`.prtgi`, `.surfels`, `.surfels.multi`)는 Git 저장소에 포함하지 않습니다. 프로젝트를 UE6 에디터로 열고 `PRTGI_Showcase` 레벨에서 `PRTProbeVolume` 여섯 개를 선택한 뒤, **Details → PRT → Build → Build Precomputation**을 실행하세요. 모든 볼륨의 계산이 완료되어야 PRT 조명을 비교할 수 있습니다. **C++ 프로젝트를 빌드하거나 프로젝트를 여는 것만으로는 캐시가 생성되지 않습니다.** 다른 레벨에서도 참여하는 볼륨을 직접 사전 계산해야 합니다.

1. 포함된 프로젝트를 사용하려면 `UE_PRT_GI.uproject`를 로컬에 설치한 **UE 6.0**에 연결한 뒤 엽니다. 저장소의 엔진 연결 ID는 개발 PC의 설치 경로에 대응하므로 다른 PC에서는 엔진 연결을 다시 지정하세요. 다른 프로젝트에 설치하려면 이 저장소의 `Plugins/PRTGI` 폴더를 `YourProject/Plugins/PRTGI`로 복사합니다. 해당 폴더 바로 아래에 `PRTGI.uplugin`이 있어야 합니다.
2. **UE 6.0**에서 **UE_PRT_GIEditor** 타깃을 **Win64 / Development Editor**로 빌드합니다. 다른 프로젝트에 설치했다면 해당 프로젝트의 Editor 타깃을 UE6로 빌드합니다. 블루프린트 프로젝트도 이 C++ 플러그인을 컴파일할 환경이 필요합니다. 필요한 경우 C++ 클래스를 추가해 프로젝트 타깃을 생성합니다.
3. **Edit → Plugins**에서 **PRT**를 검색해 활성화하고(현재 표시 이름은 **UE5-PRT-GI**), 안내에 따라 에디터를 다시 시작합니다.

GI 패스는 GBuffer를 사용하므로 현재 방식은 Forward Shading을 지원하지 않습니다. 플러그인이 Renderer 내부 인터페이스를 참조하므로 다른 엔진 버전이나 플랫폼에서는 다시 컴파일하고 검증해야 합니다. 사전 컴파일된 DLL은 엔진 빌드 간 호환을 보장하지 않습니다. 기존 구현은 설치를 위한 별도의 엔진 소스 수정을 요구하지 않습니다.

## 빠른 시작

1. 콘텐츠 브라우저에서 **Show Plugin Content**를 켭니다.
2. **PRTGI Content → Maps → PRTGI_Showcase**를 엽니다. 에셋 경로는 `/PRTGI/Maps/PRTGI_Showcase`입니다.
3. `PRTProbeVolume` 여섯 개를 선택하고 **Build Precomputation**을 실행한 뒤 완료될 때까지 기다립니다.
4. 에디터 뷰포트나 PIE 게임 화면을 클릭한 뒤 **Ctrl+Alt+2**로 PRT를 켜고, **Ctrl+Alt+3**으로 Lumen과 비교합니다.
5. **Ctrl+Alt+4**로 PRT 간접광만 확인하고, **Ctrl+Alt+P**로 프로브를 표시합니다.
6. **Ctrl+Alt+[ / ]**로 태양을 회전해 조명 변화를 확인합니다.

이 브랜치는 UE6 Renderer 인터페이스를 사용합니다. 하드웨어 레이 트레이싱 Lumen과 비교하려면 프로젝트에 DX12 / SM6, Ray Tracing 및 관련 지원 설정이 필요합니다. 플러그인은 프로젝트 렌더링 설정을 강제로 덮어쓰지 않습니다.

지원 GPU에서 **Support Hardware Ray Tracing**, **Support Compute Skin Cache**, **Use Hardware Ray Tracing when available**을 활성화하세요. 재시작이 필요한 설정을 변경했다면 에디터를 다시 시작합니다. 관련 설명은 Epic의 [Lumen Technical Details](https://dev.epicgames.com/documentation/en-us/unreal-engine/lumen-technical-details-in-unreal-engine)를 참고하세요.

포함된 예제 맵의 기본 프로필은 일시적으로 `r.Lumen.HardwareRayTracing=1`, `r.Lumen.ScreenProbeGather.ScreenTraces=0`, `r.EyeAdaptation.CachedLightingPreExposure=8`을 적용하며, 맵을 벗어나면 이전 값으로 복원합니다. 따라서 예제의 Lumen 비교는 확산광 화면 추적을 끈 상태로 이루어집니다. 이 설정만으로 프로젝트에 없는 하드웨어 레이 트레이싱 지원이 활성화되지는 않습니다.

UE6 실행용 프로젝트 설정은 [Config/DefaultEngine.ini](Config/DefaultEngine.ini)에 있습니다. Plugin GI는 `r.DynamicGlobalIlluminationMethod=2`, 반사는 `r.ReflectionMethod=0`이며, Substrate를 켜고 `r.Substrate.ProjectGBufferFormat=0`을 사용합니다. 런타임에서는 엔진의 GI 열거형을 확인하는 `PRTGI.SetMode PRT` 명령으로 PRT를 선택합니다. 기본 바운스는 **6회**이고, 사전 계산 캐시는 로컬에서 생성합니다. 다른 UE6 프로젝트에 설치할 때는 필요한 설정을 해당 프로젝트에 반영하세요.

예제는 UE DirectionalLight 하나를 공유합니다. 박스의 기존 면광원 영역은 개구부로 교체했으며, 관찰을 위한 전면 개구부는 유지했습니다.

| 볼륨 | 로컬 검증 설정·결과 |
|---|---|
| PRT_Global, PRT_ClassicBox, PRT_ColorBox, PRT_InvertedBox | 간접광 반사 1~12회용 데이터. 런타임 기본값은 6회 |
| PRT_ShinyBox | 면당 16×16 해상도. 검사한 프로브 878개 중 21개를 캡처 경고와 함께 제외 |
| PRT_MetallicBox | 면당 16×16 해상도, 프로브 간격 40 cm, 전송 셀 크기 2 cm. 검사한 프로브 2,212개 중 77개를 캡처 경고와 함께 제외 |

캐시는 저장소에 포함하지 않습니다. 예제의 `PRTProbeVolume` 여섯 개를 선택해 **Build Precomputation**을 실행하고 완료된 뒤 조명을 비교하세요. 실행용 빌드를 패키징할 때는 로컬에서 생성한 캐시를 포함해야 합니다.

## 장면에 프로브 볼륨 추가

1. 레벨을 저장합니다. 사전 계산에 사용하는 지오메트리와 머티리얼은 정적 상태를 유지해야 합니다.
2. UE **DirectionalLight**를 추가하고, 실시간으로 회전할 수 있도록 Mobility를 **Movable**로 설정합니다. 방향광이 여러 개라면 사용할 광원의 Actor Tag에 `PRTGI.PrimaryLight`를 추가합니다.
3. Place Actors에서 **PRT Probe Volume**을 찾아 장면에 배치합니다.
4. 볼륨 범위, 프로브 간격, Near Clip, Priority를 설정합니다.
5. PRT GI를 켠 뒤 액터의 Details → **PRT → Build**에서 **Build Precomputation**을 누릅니다.
6. 완료되면 **PRT → Status**의 **Cache Status**, **GI Active**, **Effective Bounces**, **Build Warnings**를 확인하고 레벨을 저장합니다.

볼륨은 **PRT GI 모드**에서만 간접광에 기여하고 프로브를 표시합니다. Lumen으로 전환하면 PRT GI의 기여는 꺼집니다.

### 설정 항목

| 항목 | 설명 |
|---|---|
| Enabled | 볼륨 활성화 여부 |
| Extent | 각 축 방향 반쪽 크기(cm). 액터 스케일도 범위에 반영됨 |
| Probe Spacing | 각 축의 목표 최대 간격(cm). 실제 균등 배치 간격은 Actual Spacing에서 확인 |
| Probe Inset | 볼륨 경계에서 프로브 격자를 안쪽으로 들이는 거리(cm) |
| Priority | 국소적으로 촘촘히 배치한 영역에는 더 높은 우선순위 사용 |
| Near Clip Cm | 프로브 캡처의 근거리 클리핑 거리. 기본 1 cm, 범위 0.01~10 cm. 주 카메라의 Near Clip과 별개 |
| Capture Resolution | 캡처 면당 해상도. 기본 16, 범위 4~32. 높일수록 캡처 비용 증가 |
| Transport Cell Size | 정적 표면을 묶는 단위 크기. 기본 2 cm, 범위 1~100 cm. 작을수록 대체로 데이터와 계산량 증가 |
| Blend Face / Distance | 전환에 사용할 면 하나와 거리(cm). 0=없음, 1/2=−/+X, 3/4=−/+Y, 5/6=−/+Z. 전환이 필요한 개구부에만 설정 |
| Display Mode | Hidden, Positions, Lighting. Lighting은 각 프로브의 조명 색상 표시 |
| Probe Radius | 시각화 구체의 반지름(cm). 실제 샘플링 위치에는 영향 없음 |

볼륨은 현재 **축에 정렬된 형태**이며 회전은 지원하지 않습니다. 각 축에는 최대 32개의 프로브를 배치할 수 있습니다. 한도를 넘으면 간격을 넓히거나 볼륨을 나누세요.

별도로 볼륨당 **캡처 샘플 4,000,000개** 제한이 있습니다. `ProbeCounts.X × ProbeCounts.Y × ProbeCounts.Z × 6 × CaptureResolution²`가 이 값을 넘으면 안 됩니다. 기본 해상도 16에서는 축별 제한을 지키면서 총 2,604개까지 배치할 수 있습니다. 한도를 넘으면 캡처 해상도를 낮추거나, 프로브 간격을 넓히거나, 볼륨을 나누세요.

처음에는 전역 볼륨을 Priority=0, 간격 약 200 cm로, 지역 볼륨을 Priority=10, 간격 약 30~40 cm로 설정한 뒤 장면 크기에 맞춰 조정하세요. 프로브를 벽 안에 배치하거나 막힌 벽을 가로질러 블렌딩 전환을 설정하지 마세요. Near Clip이 너무 크면 프로브 가까이의 지오메트리가 잘려 가시성 오류나 빛샘이 생길 수 있습니다.

### 사전 계산과 진행 상태

- 볼륨을 하나 이상 선택하고 **Build Precomputation**을 누르면 빌드 대기열에 추가됩니다. **Cancel Build**는 선택한 볼륨의 빌드를 취소합니다.
- **PRT → Build**의 **Build Progress**는 지오메트리 캡처, 전송 계산, 저장을 포함한 진행률입니다. **Build Stage**에는 현재 작업이 표시됩니다. 유효한 지오메트리 캐시가 있으면 재사용하므로 전송 단계만 실행될 수도 있습니다. 진행률은 단계별 가중치이며 남은 시간의 추정값이 아닙니다.
- 빌드는 UE 장면 캡처와 CPU 전송 사전 계산을 함께 사용합니다. 런타임 조명 갱신은 기본적으로 GPU에서 처리하지만 빌드 전체가 GPU 베이크인 것은 아닙니다.
- 빌드 시 간접광 반사 1~12회 전환에 필요한 데이터를 저장합니다. 캐시가 있는 반사 횟수로 전환할 때는 장면을 다시 캡처하지 않습니다. 기본값은 6회입니다.
- 빌드 시간은 프로브 수, 캡처 해상도, 지오메트리 복잡도, 전송 셀 크기와 하드웨어에 따라 달라집니다. 작은 볼륨에서 먼저 검증한 뒤 범위를 넓히세요.
- 정적 지오메트리, 머티리얼, 프로브 격자 배치, 캡처 매개변수를 바꾸면 다시 빌드해야 합니다. 볼륨을 복제하면 새 GUID가 생성되므로 복제본에도 별도의 캐시가 필요합니다.

지오메트리 검사 결과가 맞지 않거나 캡처가 비어 있는 프로브는 전송과 보간에서 제외됩니다. 제외된 수가 검사한 프로브의 5% 이하이고 나머지 데이터가 검증을 통과하면 경고와 함께 빌드가 완료될 수 있습니다. 이를 넘는 오류는 빌드를 중단시킵니다. 결과를 사용하기 전에 **Build Warnings**, **Invalid Probes**와 해당 영역을 확인하세요. 캡처 검사는 **Visibility** 채널의 충돌 쿼리를 사용하므로, 참여 지오메트리에는 렌더링 표면과 일치하며 이 채널을 차단하는 충돌 설정이 필요합니다.

볼륨당 전송 항목은 **2,400만 개**, 항목당 64바이트로 제한되며 전송 데이터는 최대 약 **1464.8 MiB**입니다. 파일 로드 한도에는 지오메트리 인덱스 등을 위한 384 MiB가 추가로 허용됩니다. 빌드 임시 복사본, CPU 캐시, GPU 버퍼는 별도 메모리를 사용하므로 이 수치는 총 메모리 한도가 아닙니다. 한도에 가까워져도 캡처 해상도가 자동으로 낮아지지는 않습니다.

## 콘텐츠 구조

아래 경로는 플러그인 폴더 `Plugins/PRTGI`를 기준으로 합니다.

- `Content/Maps`: 예제 레벨.
- `Content/Showcase/Materials`, `Meshes` 등: 예제에 필요한 프로젝트 에셋의 복사본. 참조는 플러그인 내부 에셋으로 연결됩니다.
- `Content/Materials`: 프로브 시각화와 지오메트리 캡처용 머티리얼.
- `Content/Precomputed/Volumes`: 레벨 패키지 이름과 볼륨 GUID별로 구분한 `.surfels` / `.multi` 원시 캐시. 광원 매개변수는 포함하지 않습니다.

`.surfels`는 캡처한 지오메트리를, 짝이 되는 `.multi`는 다중 반사 전송 데이터를 저장합니다. 두 파일을 함께 보관하세요. 원시 바이너리 파일이므로 콘텐츠 브라우저에 `.uasset`으로 표시되지 않습니다. 캐시는 고정된 태양 각도의 최종 이미지가 아닙니다. 런타임에도 현재 광원 입력과 가림을 계산한 뒤 사전 계산된 전송 데이터를 적용합니다.

`/PRTGI/` 아래의 플러그인 레벨은 플러그인 내부의 `Content/Precomputed/Volumes/<레벨-패키지>/<GUID>.prtgi.surfels`와 `<GUID>.prtgi.surfels.multi`에 저장합니다. `/Game/`을 포함한 다른 레벨은 `YourProject/Content/PRTGI/Volumes/<레벨-패키지>/`에 저장합니다. 레벨 패키지 이름의 슬래시는 밑줄로 바뀝니다. 레벨을 이동하거나 이름을 바꾸면 캐시 경로도 바뀌므로 **Cache File**을 확인하고 필요하면 다시 빌드하세요.

플러그인의 빌드 규칙은 자체 `Content/Precomputed` 폴더를 별도의 **NonUFS** 파일로 패키징합니다. `/Game/` 레벨을 사용하는 프로젝트에서는 **Additional Non-Asset Directories To Copy**에 `PRTGI/Volumes`를 추가해 프로젝트의 `Content/PRTGI/Volumes`도 포함해야 합니다. 입력 경로는 프로젝트 Content 기준입니다. 기존 검증 기록은 에디터 사용을 대상으로 하므로 패키징 결과에서도 캐시 로드를 확인하세요.

## 현재 제한 사항

- PRT는 선택한 DirectionalLight 하나에 의한 간접 확산광을 계산합니다. Point, Spot, Rect Light, SkyLight, 발광 머티리얼은 이 전송 계산의 광원으로 사용되지 않습니다. 플러그인은 스페큘러 GI나 반사를 제공하지 않습니다.
- 전송에 사용하는 지오메트리와 머티리얼은 정적 상태여야 합니다. 런타임 광원이 바뀌면 GPU 가림 계산에 캐시된 지오메트리를 재사용합니다. 움직이는 차폐물은 캐시된 전송에 자동으로 반영되지 않습니다.
- 곡면에 프로브로 인한 고리 무늬가 나타나거나 Lumen과 밝기가 다를 수 있습니다. 조명을 비교할 때 실제 장면의 결과를 확인하세요.

## 단축키

에디터 또는 PIE 뷰포트를 클릭해 포커스를 준 뒤 사용하세요.

| 단축키 | 동작 |
|---|---|
| Ctrl+Alt+1 | 동적 GI 끄기. 이 예제에는 오프라인 베이크 조명이 없음 |
| Ctrl+Alt+2 | PRT GI |
| Ctrl+Alt+3 | Lumen GI |
| Ctrl+Alt+4 | PRT 간접 확산광만 보기 / 전체 화면 전환 |
| Ctrl+Alt+P | 프로브 표시 / 숨기기. 위치 마커 또는 조명 구체 표시는 볼륨의 Display Mode에 따름 |
| Ctrl+Alt+G | 간접광 반사 4 / 8 / 12회 순환. 해당 캐시 필요 |
| Ctrl+Alt+[ / ] | DirectionalLight Yaw를 기본 간격인 5°만큼 감소 / 증가 |
| Ctrl+Alt+Shift+[ / ] | DirectionalLight Pitch를 기본 간격인 5°만큼 감소 / 증가 |
| Ctrl+Alt+B | 이전 방식의 전역 베이크 명령. 액터 볼륨은 빌드하지 않음 |

지역 사전 계산은 해당 `PRTProbeVolume`을 선택하고 **Build Precomputation**을 눌러 실행합니다. 간접광 반사 기본값은 6회입니다.

포커스나 키보드 배열 때문에 단축키가 작동하지 않으면 UE 콘솔에서 다음 명령을 하나씩 실행하세요.

```text
PRTGI.SetMode PRT
PRTGI.SetMode Lumen
PRTGI.ToggleIndirectOnly
r.PRTGI.ShowProbes 1
r.PRTGI.Bounces 4
PRTGI.RotateLight 5 0
PRTGI.Status
```

`PRTGI.SetMode Offline`은 동적 GI를 종료하는 이전 명령 이름입니다. 오프라인 조명이나 Path Tracing 기준 이미지를 자동으로 생성하지는 않습니다.

## Surfel 품질 진단

v6부터 전송 캐시는 캡처 표면을 삼각형에 연결하고 기하 법선과 셰이딩 법선을 분리합니다. 2차 광선이 실제로 맞힌 삼각형의 재질을 원래 UV와 정점 색상을 유지한 차트에서 조회합니다. 베이크에는 기본색, 금속성, 접선 공간 법선을 사용하며 전송 계산은 Lambert 확산 모델을 유지합니다. 최종 수신 표면은 GBuffer의 반사율·거칠기와 UE의 해석적 GGX 에너지 근사·다중 산란 보정으로 반사 로브의 에너지를 제외합니다. 이 수신 표면 보정은 단일 GGX 재질의 근사이며, Substrate의 모든 레이어·광택 반사 전송·동적 차폐물·하늘광·발광 전송을 구현한 것은 아닙니다. v3~v6 캐시는 계속 읽고 같은 버전으로 저장할 수 있습니다. 새 v8 형식을 일관되게 적용하려면 참여하는 볼륨을 모두 다시 계산하세요.

재질 베이커는 차트 내부의 모든 텍셀이 렌더링됐는지 검사합니다. 프리미티브 변환 때문에 차트가 잘리면 정점·월드 데이터에 의존하지 않는다고 확인된 재질만 프리미티브 데이터 없이 재시도합니다. 그 외 실패는 베이크를 중단하고 이전 캐시를 보존합니다. 삼각형마다 16×16 차트를 사용하므로 매우 세밀한 텍스처까지 그대로 보존하지는 않습니다.

`PRTGI.DumpQuality`는 캐시 버전, 실제 재질 조회 수, 반사 단계별 연결 실패 수, GPU 상주 상태를 `Saved/PRTGIQuality.json`에 기록합니다. 연결 실패율은 손실된 에너지의 비율이 아닙니다. 수치 검증은 `r.PRTGI.GPUReadback 1`을 설정하고 재조명이 끝난 뒤 `PRTGI.ValidateMultiBounce`를 실행하세요. 결과는 `Saved/PRTMultiBounceValidation.json`에 저장되며 에디터가 잠시 멈출 수 있습니다. 검증 후 `GPUReadback 0`으로 복원하세요. 일반 렌더링은 계수를 GPU에 보관하며, CPU 읽기는 진단과 프로브 시각화에 사용합니다.

```text
python Plugins/PRTGI/Tools/quality_report.py --output Saved/PRTGIQuality/caches.json Plugins/PRTGI/Content/Precomputed/Volumes
python -m pip install numpy OpenEXR
python Plugins/PRTGI/Tools/compare_hdr.py reference.exr candidate.exr --roi 230 150 708 570
```

HDR 비교 도구는 노출을 임의로 맞추지 않고 정렬된 선형 RGB 영상을 비교합니다. `PreTonemapHDRColor`와 동일한 카메라·해상도·광원·반사 횟수를 사용하세요. 확산 간접광만 비교할 때는 DirectLighting 표시 플래그를 끄고, Specular 표시 플래그를 켠 상태에서 임시 포스트 프로세스 볼륨으로 간접 정반사 기여를 제외합니다. 그래도 기준 영상에는 확산·정반사가 섞인 경로가 남을 수 있으므로 Lambert PRT와 완전히 같은 모델의 정답 영상은 아닙니다. 이 테스트에서는 발광과 디노이저를 끄고, 경로 밝기 제한이 기준 영상을 잘라내지 않는지도 확인해야 합니다. Showcase 기준 영상은 512 샘플, 4회 반사, 사전 노출 1, `r.PathTracing.MaxPathIntensity 65504`로 촬영했습니다.

| 콘솔 변수 | 기본값 | 의미 |
|---|---:|---|
| `r.PRTGI.GPUResident` | 1 | 재조명 계수를 GPU에 보관. 0은 읽기·재업로드 경로 |
| `r.PRTGI.GPUReadback` | 0 | 수치 진단을 위해 CPU에도 계수 보관 |
| `r.PRTGI.ProbeVisibility` | 1 | 0 끔, 1 최초 교차 평면, 2 실험적 깊이 모멘트, 3 정적 BVH 선분 검사 |
| `r.PRTGI.VisibilityBiasCm` | 1 | 수신 표면 가시성 바이어스(cm) |
| `r.PRTGI.HemisphereTransition` | 0.2 | 최소 프로브 간격 대비 양의 반구 전환 폭. 0은 기존의 불연속 제외 방식 |
| `r.PRTGI.ContactSamples` | 0 | 선택적 정적 접촉 차폐: 픽셀당 0~16개 광선 |
| `r.PRTGI.ContactRadiusCm` | 20 | 접촉 광선 최대 거리 |
| `r.PRTGI.ContactBiasCm` | 0.1 | 접촉 광선 시작점·교차 바이어스 |
| `r.PRTGI.Debug` | 0 | 1 가시성, 2 보간 지지율, 3 반구 수용률, 4 접촉 가시성 |

## SurfelPlus 자료를 참고한 선택적 실험

[SurfelPlus](https://wangruipeng.com/SurfelPlus/)의 표본 배분과 저해상도 계산 방식을 현재의 정적 PRT 전송 구조에 맞춰 실험합니다. SurfelPlus의 동적 Surfel 시스템이나 반사 렌더러 전체를 이식한 것은 아닙니다. 기존 기본값은 유지하며 다음 옵션을 명시적으로 켜서 비교할 수 있습니다.

| 설정 | 기본값 | 사용 방법 |
|---|---:|---|
| `r.PRTGI.Debug 5` | 꺼짐 | 실제 기여하는 프로브 수. 16개 이상이면 녹색 |
| `r.PRTGI.Debug 6` | 꺼짐 | 유효 프로브 수 `(Σw)²/Σ(w²)`. 가중치가 소수 프로브에 집중되면 낮아짐. 16 이상이면 녹색 |
| `r.PRTGI.Debug 7` | 꺼짐 | 최대 정규화 가중치에 대한 의존도. 한 프로브에 집중되면 빨강, 분산되면 녹색 |
| `r.PRTGI.ContactHalfResolution` | 0 | 1이면 접촉 차폐를 가로·세로 절반 해상도로 계산하고 깊이·법선으로 복원. 경계와 지지 표본 부족 영역은 전체 해상도로 다시 계산 |
| 볼륨의 `Transport Paths Per Sample` | 1 | 캡처 표본당 실제 전송 경로 수. 1~16, 적응 모드에서는 볼륨 내 각 프로브의 평균 예산 |
| 볼륨의 `Adaptive Transport Sampling` | false | 위 경로 수가 2 이상일 때 사용. 별도의 예비 경로 2개로 광원과 무관한 전송 변동을 추정하고 실제 경로 예산을 재배분 |

진단 5~7은 볼륨 경계 혼합 전 선택된 볼륨을 표시하며, 기존 전역 trilinear 경로는 자홍색으로 구분합니다. 진단 시 `r.PRTGI.IndirectOnly 1`과 기본 노출 설정을 사용하고, 확인 후 `r.PRTGI.Debug 0`, `r.PRTGI.IndirectOnly 0`으로 복원하세요. 접촉 차폐 옵션은 시간 이력을 추가하지 않으며, 캐시를 다시 만들 필요가 없습니다. 화면 밖 동적 차폐물을 새로 지원하는 기능은 아닙니다.

표본 배분은 현재 태양의 밝기를 사용하지 않습니다. 예비 경로에서 삼각형·바운스별 확산 응답의 변동을 추정하고, 각 캡처 표본에 최소 1개에서 평균 예산의 4배까지 실제 경로를 배분합니다. 각 표본의 전송 가중치는 실제 경로 수로 나눕니다. 예비 경로는 최종 조명에 더하지 않으며, 별도 빌드 비용입니다. 예를 들어 **균등 4개**와 **적응 2개 + 예비 2개**는 시작 경로 수가 같지만, 조기 종료와 교차 비용에 따라 실행 시간은 달라집니다. 품질 향상을 보장하는 오차 추정기는 아닙니다.

표본 설정 변경 시 **Build Precomputation**으로 전송을 다시 계산해야 합니다. v8 캐시는 표본 설정과 예비·실제 경로 수를 저장합니다. 기존 v3~v7 캐시는 균등 1개 설정에서 계속 읽으며, 새 표본 설정과 일치하지 않는 캐시는 사용하지 않습니다. 생성된 캐시는 계속 Git에서 제외됩니다.

Showcase에서 반해상도 접촉 차폐의 전체 해상도 대비 HDR 상대 RMSE는 넓은 시점 **3.37%**, 구체 근접 시점 **6.04%**였습니다. RTX 4090에서 각 모드를 번갈아 5회 측정한 PRT 패스 합계 중앙값은 **3.479 → 3.454 ms**, **3.800 → 2.609 ms**였습니다(TSR 입력 1671×577, 출력 1853×639). 넓은 시점에서는 유의미한 성능 이득을 확인하지 못했고, 이 HDR 수치는 Path Tracing 기준 정확도가 아닙니다.

별도 8프로브·741표본 검증에서 균등 16개를 유한 기준으로 삼은 SH 계수 차이는 균등 1개 **8.89%**, 균등 4개 **2.87%**, 적응형 2개＋예비 2개 **4.69%**였습니다. 예비 경로를 포함한 시작 경로 수가 같은 두 옵션 중 이 장면에서는 균등 4개가 더 정확했습니다. 광원 가림을 끈 4개 광원 방향·고정 난수열 비교이고 기준과 경로 일부를 공유하므로 일반적인 분산 감소를 입증하지는 않습니다. [측정 조건·원시 수치·한계](Plugins/PRTGI/Tools/validation_surfelplus.json)를 함께 확인하세요.

## 기존 표면·접촉 조명 검증

접촉 차폐는 짧은 거리의 확산광 근사입니다. GPU 비용과 표본 노이즈가 추가되며 전송된 복사휘도를 대신하지 않습니다. 깊이 모멘트는 캡처의 네 이웃 표본을 보간하는 실험으로, Showcase에서 벽의 얼룩이 증가했습니다. 정확한 BVH 가시성도 선택 기능으로 유지합니다. 진단 후 `IndirectOnly 0`, `Debug 0`, `ProbeVisibility 1`, `HemisphereTransition 0.2`로 복원하세요.

곡면의 원형 경계는 프로브가 수신 표면의 접평면을 넘을 때 기여도가 갑자기 사라지는 현상으로 재현했습니다. 기존 연속 전환 경로를 기본값 0.2로 활성화해 경계 근처의 기여도를 부드럽게 줄입니다. 가시성 검사는 유지하며 재사전 계산은 필요 없습니다. DDGI의 연속 가중치 원칙을 참고한 보수적인 설정으로, 뒤쪽 프로브에도 가중치를 주는 RTXGI의 wrap 방식 전체를 이식한 것은 아닙니다. 확인한 최신 자료와 비교 조건은 [곡면 검증 기록](Plugins/PRTGI/Tools/validation_curved_surface.json)에 있습니다. 기존 v6·바운스 검증 수치는 전환 폭 0에서 얻은 과거 기록입니다.

기하·셰이딩 법선 분리와 재구성은 [EA GIBS SIGGRAPH 2024 발표](https://www.advances.realtimerendering.com/s2024/content/EA-GIBS2/Apers_Advances-s2024_Shipping-Dynamic-GI.pdf)를 참고했습니다. [SurfelPlus SIGGRAPH 2025 포스터](https://github.com/WANG-Ruipeng/SurfelPlus/blob/master/docs/files/Poster.pdf)와 [DDGI](https://arxiv.org/abs/2009.10796)도 비교 자료로 사용하며, 이 플러그인이 해당 시스템 전체를 구현한 것은 아닙니다.

이전 v6의 UE6 Showcase 검증에서 v6 캐시 여섯 개의 2차 표면 조회 누락은 모두 0이었습니다. 기존 누락률은 0.68~9.26%였습니다. 고정 카메라의 확산 성분 HDR 기준 영상과 비교한 RGB 상대 RMSE는 ClassicBox / ColorBox / 접촉부에서 **23.24% / 32.61% / 24.84% → 18.02% / 16.70% / 11.24%**로 줄었습니다(접촉 표본 8개). 특정 영역의 비교 결과이며 장면 전체가 기준 렌더러와 같다는 뜻은 아닙니다. GPU·CPU 계수 오차는 최대 `3.75e-5`이고, 관련 자동화 테스트 5개가 통과했습니다. MetalBox와 ShinyBox의 기존 캡처 감사 제외 항목은 유지됩니다.

RTX 4090의 정적 프레임 3회 측정에서 PRT 적용 패스 중앙값은 접촉 보정 없이 **0.499 ms**, 표본 8개 사용 시 **2.725 ms**였습니다(입력 1740×590, TSR 출력 2164×733). 여섯 볼륨의 전송 파일 합계는 **1651 MiB → 2157 MiB**로 증가했습니다. 호스트 프로젝트의 품질 설정은 접촉 표본 8개를 사용하고 플러그인 기본값은 0을 유지합니다. GPU 비용을 줄이려면 `r.PRTGI.ContactSamples 0`을 설정하세요. 측정 조건, ROI 좌표, 캐시 통계, 남은 한계는 [validation_showcase.json](Plugins/PRTGI/Tools/validation_showcase.json)에 기록했습니다.

## 4·8·12회 바운스 비교

v7 이상 캐시는 최대 12회 경로를 한 번 사전 계산합니다. `r.PRTGI.Bounces 4`, `8`, `12` 또는 **Ctrl+Alt+G**(4 → 8 → 12 → 4)로 비교합니다. 런타임 기본값은 6회입니다. 기존 v3~v6 캐시는 실제 적용 횟수를 4회로 제한하고 재계산 안내를 표시합니다. 8·12회를 비교하려면 참여하는 볼륨을 모두 다시 계산하세요.

각 `PRTProbeVolume`의 **Transport Min Throughput** 기본값은 **0.01**입니다. 처음 4회는 유지하고, 이후 누적 확산 반사율의 RGB 최댓값이 임계값 미만이면 다음 광선을 추적하기 전에 경로를 끝냅니다. 0으로 설정하면 이 조기 종료를 끕니다. 기존의 극소값 수치 보호 조건은 유지됩니다. 값을 바꾸면 전송 캐시가 무효화되므로 **Build Precomputation**이 필요합니다. 이 방식은 작은 기여를 생략하는 편향된 근사이며, 특히 밝기 대비가 큰 조명에서 화면 오차 1%를 보장하지 않습니다. 현재 광원 방향이나 세기에 의존하지 않는 기준입니다.

매 프레임 8·12회 경로를 다시 추적하지 않습니다. 광원이나 바운스 수가 바뀔 때 광원 입력과 저장된 전송 데이터를 계산해 SH 계수를 갱신하고, 정적인 프레임에서는 그 계수를 보간합니다. 별도의 접촉 차폐 패스는 매 프레임 실행됩니다. 모든 횟수가 하나의 캐시를 공유하므로 4회를 선택해도 할당된 캐시 메모리는 줄지 않지만, GPU 전송 계산은 이후 단계의 정렬된 항목을 건너뜁니다.

`PRTGI.DumpQuality`와 `quality_report.py`는 단계별 경로 수·조기 종료 수·최대 횟수·임계값을 기록합니다. `PRTGI.ValidateMultiBounce`의 에너지 배열은 이제 `bounce_counts: [1,4,8,12]` 순서입니다. 구버전 캐시는 뒤의 두 항목도 4회로 제한합니다.

```text
python Plugins/PRTGI/Tools/compare_bounces.py reference12.exr b4.exr b8.exr b12.exr --roi classic 930 260 1190 510 --roi color 1490 330 1810 580 --output comparison.json
```

선형 `*_PreTonemapHDRColor.exr` 파일을 입력하세요. 이번 비교에서는 12회 기준 영상 하나를 사용하고, 카메라·광원·노출과 후보 영상의 접촉 차폐 설정을 동일하게 맞춥니다.

UE6 Showcase의 12회 확산 성분 기준 영상 대비 결과입니다(RGB 상대 RMSE, 접촉 표본 8개).

| 비교 영역 | 4회 | 8회 | 12회 |
|---|---:|---:|---:|
| InvertedBox | 32.86% | 20.38% | 17.97% |
| ClassicBox | 21.06% | 19.23% | 19.12% |
| ColorBox | 17.47% | 16.04% | 16.33% |

단일 프레임 캡처에는 렌더링 노이즈가 남아 있으므로 ColorBox의 작은 차이로 12회가 더 나쁘다고 단정할 수는 없습니다. ClassicBox·ColorBox는 8회에서 대부분 개선되고, InvertedBox는 12회의 추가 효과가 더 큽니다. RTX 4090에서 여섯 볼륨의 저장된 전송 데이터 계산은 4 / 8 / 12회에서 약 **2.16 / 2.91 / 3.04 ms**였습니다(완전한 조명 갱신 표본 2 / 3 / 3회의 중앙값). 여기에 광원 입력 계산 약 8.7~8.9 ms가 더해집니다. 정적 프레임 프로파일에서는 전송 계산이 실행되지 않았습니다.

ClassicBox를 조기 종료 없이 계산한 대조군과 비교하면 임계값 0.01에서 전송 항목 **5.76%**, 캐시 용량 **6.71%**가 줄고, 12회 프로브 L0 에너지 합은 **0.040%** 낮아졌습니다. 화면 오차를 보장하는 수치는 아닙니다. 여섯 v7 캐시는 총 **3016.94 MiB**이며 2차 표면 조회 누락이 모두 0이고 CRC 검사를 통과했습니다. GPU·CPU 계수 상대 오차는 `3.75e-5` 미만이고 관련 테스트 5개가 통과했습니다. MetalBox·ShinyBox의 기존 캡처 경고는 남아 있습니다. 좌표·측정 조건·원시 시간·한계는 [validation_bounces.json](Plugins/PRTGI/Tools/validation_bounces.json)에 기록했습니다.
