# aima-framework

**비프로그래머도 게임을 만들 수 있게** 하는, 복붙용 크로스플랫폼 게임 프레임워크.
[arimu](https://github.com/jungminna03/arimu-framework)(EnTT 기반 ECS)를 코어로 감싸,
호스트 루프 · 핫리로드 · SDL3 플랫폼 계층 · 추상 **Renderer 인터페이스**를 한 폴더로 묶었다.

목적은 셋:

1. **arimu(ECS) + 커스텀 라이브러리를 공유·재활용** — 크로스플랫폼 빌드, 범용 라이브러리,
   호스트 루프, 코드·에셋 핫리로드, SDL3 플랫폼 계층(윈도우·입력·게임패드·오디오),
   추상 **Renderer 인터페이스**.
2. **클릭 한 번 셋업** — 툴체인 / vcpkg / IDE까지 알아서.
3. **텔레그램 연동 개발 플로우** — 떠들면 봇이 코드 고치고 빌드·푸시.

> **렌더러는 안 들어있다.** 프레임워크는 아무것도 안 그린다 — 각 게임이 `aima::Renderer` 를
> 자기 그래픽으로 구현한다(3D, 2D, 단순 SDL clear, 무엇이든). 그래서 어떤 장르든 같은 토대를 쓴다.

## 한 폴더 복붙으로 새 게임 시작

```
MyGame/                    ← IDE에서 여는 "내 게임" 폴더 (이름은 자유)
├─ aima_framework/         ← 이 폴더를 통째로 복붙 (순수 프레임워크 + tools, 절대 안 건드림)
├─ game/                   ← setup이 찍어줌. 게임 로직 = 여기 (aima_framework 밖)
├─ CMakeLists.txt          add_subdirectory(aima_framework) + game 빌드
├─ CMakePresets.json · vcpkg.json · .vscode/ · .run/ · third_party/ · aima.project.json
└─ build/                  (빌드 산출물)
```

**흐름:**
1. `MyGame` 폴더 만들기.
2. **`aima_framework` 폴더를 그 안에 복붙.**
3. **`aima_framework/tools/setup`** 클릭 → 부모 `MyGame` 이 빌드되는 게임 프로젝트로 변신
   (게임 스켈레톤이 `MyGame/game/` 에 찍히고, 툴체인·vcpkg 설치, IDE 열림, **검은 창** 빌드).
   - macOS: `tools/setup-mac.command` · Windows: `tools/setup.bat`
4. **`aima_framework/tools/issue-token`** 클릭 → 토큰 발급.
5. 텔레그램 그룹에서 **`/bind <토큰>`** → 이 방 ↔ 이 게임 연결.
6. **봇에게 "○○ 게임 만들어" → `MyGame/game/` 이 채워지기 시작.**

> 게임 로직은 **항상 `MyGame/game/` (= aima_framework 밖)** 에 있다. `aima_framework/` 는
> 순수 의존성이라 절대 수정하지 않는다 — 새 버전이 나오면 폴더째 교체만 하면 된다.

## 폴더 안 (aima_framework/)

```
aima_framework/
├─ CMakeLists.txt           # 라이브러리 (add_subdirectory 로 끌어씀, NO renderer)
├─ CMakePresets.json        # Windows / macOS / Linux 프리셋
├─ vcpkg.json               # 범용 deps (그래픽 라이브러리 없음; Jolt 물리 옵션)
├─ include/aima/            # aima.h · renderer.h(인터페이스) · host.h(루프+모듈 ABI)
├─ src/                     # core(log/math/hot_reload/host) · platform(window/input/audio) · assets
├─ arimu-framework/         # 내장 Arimu ECS (+ EnTT) — github.com/jungminna03/arimu-framework
├─ USAGE_FOR_AI.md          # AI/개발자용 상세 매뉴얼 (Renderer 인터페이스·ABI·핫리로드)
└─ tools/
   ├─ setup-mac.command/.sh · setup-windows.ps1 · setup.bat   ← 부모를 프로젝트로 스캐폴딩
   ├─ issue-token.command/.sh/.bat · issue_token.py           ← 토큰 발급(부모 등록)
   └─ template/             ← setup이 부모(MyGame)로 찍어내는 스켈레톤
```

## 아키텍처 한눈에

```
   your game/ (game logic)         ← 핫리로드되는 게임 모듈 (aima_framework 밖)
        │  implements aima::Renderer, App::Tick, GameServiceFrame
        ▼
   aima_framework  ── host loop · hot-reload · SDL3 platform(window/input/gamepad/audio) · assets
        │
        ▼
   arimu (ECS)  ── World · Schedule · System · Query · Resource · Event · Commands  (EnTT 위)
```

게임은 매 프레임 호스트가 부르는 `App::Tick(dt)` 과(선택) `GameServiceFrame` 훅에서 ECS를
돌리고, `aima::Renderer` 구현으로 그린다. 입력은 키보드·마우스·게임패드가 하나의
`InputState` 로 합쳐져 들어온다.

## IN vs 제외

**IN:** 크로스플랫폼 빌드 + 범용 라이브러리(SDL3, spdlog, efsw, nlohmann_json, tomlplusplus,
DirectXMath, EnTT via Arimu; Jolt 옵션) + 호스트 루프 + 코드·에셋 핫리로드 + 플랫폼 계층
(윈도우·입력·게임패드·오디오) + ECS + **Renderer 인터페이스**.

**제외(게임이 공급):** 구체 렌더러 / GPU 디바이스 / 패스 / 셰이더, GPU 에셋 로더, imgui,
DX12/SDL_GPU — 즉 모든 그래픽.

## setup 옵션

- mac: `--skip-build` · `--skip-install` · `--ide vscode|clion|xcode|none` · `--no-open`
- win: `-Config release` · `-Ide vscode|clion|vs|none` · `-SkipBuild`

상세 계약(Renderer 인터페이스·게임 모듈 ABI·핫리로드·새 프로젝트)은 [`USAGE_FOR_AI.md`](USAGE_FOR_AI.md),
ECS는 [`arimu-framework/USAGE_FOR_AI.md`](arimu-framework/USAGE_FOR_AI.md) 참고.
