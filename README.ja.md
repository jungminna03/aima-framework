# aima-framework

🌏 [한국어](README.md) · [日本語](README.ja.md)

**均一なクオリティの HD2D ゲームを量産するためのフレームワーク。** カスタムライブラリ
エンジン + 内蔵 **HD2D レンダラーモジュール** + `aimaちゃん` チャットボットで、新しい HD2D
ゲームを「同じ土台・同じルック」で繰り返し量産する。
[arimu](https://github.com/jungminna03/arimu-framework)（EnTT ベースの ECS）をコアとして包み、
クロスプラットフォームビルド · ホストループ · ホットリロード · SDL3 プラットフォーム層を
ひとつのエンジンにまとめ、その上に **バッテリー同梱（batteries-included）の HD2D レンダラー**
（DX12/SDL_GPU · bloom/AgX ポスト · LiveScene · スプライト/ビルボード · ペーパーアニメ ·
標準プラグインセット）と Telegram チャットボット **aimaちゃん** を乗せている。レンダラーは
オプション `AIMA_HD2D_RENDERER=ON` + vcpkg フィーチャ `hd2d-renderer` で有効化する。抽象
**`aima::Renderer` シームはそのまま** — 望めばゲームが自分のレンダラーを持ち込むこともできるが、
基礎が全部入った HD2D パスがこのフレームワークの目玉だ。

## なぜ作ったか — ワークフローをひっくり返すため

従来のゲーム開発はたいてい **ドキュメント作業 → 実装** の順だ。企画書を書き、仕様をまとめ、
それからようやくコードを書く。けれどゲームは *実際にプレイしてみるまで* 何が面白いか分からない。
どれだけ丁寧にドキュメントを書いても、手に取って触ってみれば半分は外れている。

だから順番をひっくり返したかった — **実装 → フィードバック → 実装** のループに。まずプロトタイプを
動かしてみて、プレイした感想を投げると、その感想がそのまま次の実装につながる流れ。**ドキュメント作業は
人が前もってやるのではなく、このループの中で自動的についてくる** ようにする。

そのループを人が手で回すと結局遅くなるので、その役割をチャットボット **aimaちゃん** が埋める。

## aimaちゃん — 感想を実装に変えるチャットボット

```
プロトタイプをプレイ  →  Telegram に感想を投げる  →  aimaちゃん
                                                       │
                          感想を整理（＝ドキュメント自動生成）│
                                                       ▼
                                              実際のコード実装 · ビルド · プッシュ
                                                       │
                                                       ▼
                                              新しいプロトタイプを再びプレイ  ⟲
```

1. **プロトタイプをプレイ**してみる。
2. 感じたことを **Telegram グループに自然言語で** 投げる。（「ジャンプがもたつく」「ボスのパターンを一拍速く」…）
3. **aimaちゃん** がその感想を **整理（ドキュメント化）** し、**実際の実装まで** 行う — コード修正 → ビルド → プッシュ。
4. 新しくビルドされたプロトタイプを **再びプレイ** → 感想 → 実装。ループが回り続ける。

ドキュメントは「書く作業」ではなく、**ループが残していく副産物** になる。

> **HD2D レンダラーが内蔵されている。** `AIMA_HD2D_RENDERER=ON` ならエンジンがそのまま
> HD-2D 画面を描く — DX12(Windows)/SDL_GPU(macOS Metal) バックエンド、bloom·AgX トーンマップ
> ポスト、LiveScene シーン提出、スプライト/ビルボード、ペーパーアニメ数学、
> render/sprite/physics/sound/ui/input 標準プラグインセットまで。だから新しい HD2D ゲームは
> **レンダラーを書き直さず** コンテンツから始められる。抽象 `aima::Renderer` シームは残って
> いるので、望めばゲームが自分のグラフィック（2D、単純な SDL clear、別の 3D バックエンド）で
> 実装して差し込むこともできる — だが均一クオリティへの近道は内蔵 HD2D パスだ。

## 1フォルダのコピペで新しいゲームを始める

```
MyGame/                    ← IDE で開く「自分のゲーム」フォルダ（名前は自由）
├─ aima_framework/         ← このフォルダを丸ごとコピペ（純粋エンジン + tools、絶対に触らない）
├─ game/                   ← setup が生成する。ゲームロジック = ここ（aima_framework の外）
├─ CMakeLists.txt          add_subdirectory(aima_framework) + game ビルド
├─ CMakePresets.json · vcpkg.json · .vscode/ · .run/ · third_party/ · aima.project.json
└─ build/                  （ビルド成果物）
```

**流れ:**
1. `MyGame` フォルダを作る。
2. **`aima_framework` フォルダをその中にコピペ。**
3. **`aima_framework/tools/setup`** をクリック → 親 `MyGame` がビルド可能なゲームプロジェクトに変身
   （ゲームスケルトンが `MyGame/game/` に生成され、ツールチェーン·vcpkg インストール、IDE 起動、**黒い窓** ビルド）。
   - macOS: `tools/setup-mac.command` · Windows: `tools/setup.bat`
4. **`aima_framework/tools/issue-token`** をクリック → トークン発行。
5. Telegram グループで **`/bind <トークン>`** → この部屋 ↔ このゲームを接続。
6. あとは **しゃべれば aimaちゃんが `MyGame/game/` を実装し始める** — 実装 → フィードバック → 実装ループ開始。

> ゲームロジックは **常に `MyGame/game/`（＝ aima_framework の外）** にある。`aima_framework/` は
> 純粋な依存なので絶対に変更しない — 新バージョンが出たらフォルダごと差し替えるだけ。

## フォルダの中身 (aima_framework/)

```
aima_framework/
├─ CMakeLists.txt           # エンジンライブラリ（add_subdirectory で取り込む）+ AIMA_HD2D_RENDERER オプション
├─ CMakePresets.json        # Windows / macOS / Linux プリセット
├─ vcpkg.json               # 汎用 deps + フィーチャ hd2d-renderer(imgui/meshopt/directxtk12…)
├─ include/aima/            # aima.h · renderer.h（インターフェース）· host.h（ループ + モジュール ABI）
├─ src/                     # core(log/math/hot_reload/host) · platform(window/input/audio) · assets(res_path)
├─ hd2d/                    # 🎨 内蔵 HD2D レンダラーモジュール（オプション AIMA_HD2D_RENDERER=ON）
│  ├─ renderer/             #   DX12(dx12/) · SDL_GPU(sdlgpu/, live_scene.h 共有レイアウト) · camera · post
│  ├─ ui/ · assets/ · shaders/  #   imgui_layer · gltf/sprite ローダー · MSL/HLSL シェーダー
│  └─ game_core/            #   paper_anim/compass/dof/clip_rules/components_core + 標準プラグイン6種
├─ arimu-framework/         # 内蔵 Arimu ECS (+ EnTT) — github.com/jungminna03/arimu-framework
├─ USAGE_FOR_AI.md          # AI/開発者向け詳細マニュアル（Renderer インターフェース·ABI·ホットリロード·hd2d モジュール契約）
└─ tools/
   ├─ setup-mac.command/.sh · setup-windows.ps1 · setup.bat   ← 親をプロジェクトへスキャフォールド
   ├─ issue-token.command/.sh/.bat · issue_token.py           ← トークン発行（親を登録）
   └─ template/             ← setup が親(MyGame)へ生成するスケルトン
```

## アーキテクチャ一望

```
   aimaちゃん (telegram)  ── 感想 → 整理(自動ドキュメント) → 実装 → ビルド → プッシュ ⟲
        │
        ▼
   your game/ (game logic)         ← ホットリロードされるゲームモジュール（aima_framework の外）
        │  implements aima::Renderer, App::Tick, GameServiceFrame
        ▼
   aima エンジン  ── host loop · hot-reload · SDL3 platform(window/input/gamepad/audio) · assets
        │
        ▼
   arimu (ECS)  ── World · Schedule · System · Query · Resource · Event · Commands  （EnTT の上）
```

ゲームは毎フレーム、ホストが呼ぶ `App::Tick(dt)` と（任意の）`GameServiceFrame` フックで ECS を
回し、`aima::Renderer` 実装で描画する。入力はキーボード·マウス·ゲームパッドがひとつの
`InputState` にまとめられて入ってくる。

> **私の役割 — 設計者。** このシステムを構築するにあたり、エンジン内部のコード実装の大部分は AI に
> 任せ、私は **Telegram API ↔ LLM をつなぐパイプライン設計とプロンプト最適化（Prompt
> Engineering）に集中** した。「どうしゃべればどんな実装が出てくるか」を決める境界面 — 感想を
> 構造化されたタスク指示に変えるプロンプト、ビルド・プッシュまでつながる自動化パイプライン — が
> 私自身が設計した核心だ。

## IN vs 除外

**常に IN:** クロスプラットフォームビルド + 汎用ライブラリ（SDL3, spdlog, efsw, nlohmann_json,
tomlplusplus, DirectXMath, EnTT via Arimu; Jolt オプション）+ ホストループ + コード·アセットの
ホットリロード + プラットフォーム層（ウィンドウ·入力·ゲームパッド·オーディオ）+ ECS +
**Renderer インターフェース** + **aimaちゃん チャットボットワークフロー**。

**オプション IN — HD2D レンダラーモジュール（`AIMA_HD2D_RENDERER=ON` + vcpkg フィーチャ
`hd2d-renderer`）:** 具体的なレンダラー（DX12 / SDL_GPU バックエンド）· GPU デバイス ·
フォワード/シャドウ/ポストパス · bloom/AgX シェーダー · gltf/sprite GPU アセットローダー ·
imgui レイヤー · LiveScene シーン提出 · ペーパーアニメ数学 ·
render/sprite/physics/sound/ui/input 標準プラグインセット。OFF（デフォルト）にすれば
フレームワークはレンダラーレスのまま、ゲームが自分の `aima::Renderer` を供給する。

## setup オプション

- mac: `--skip-build` · `--skip-install` · `--ide vscode|clion|xcode|none` · `--no-open`
- win: `-Config release` · `-Ide vscode|clion|vs|none` · `-SkipBuild`

詳細な契約（Renderer インターフェース·ゲームモジュール ABI·ホットリロード·新規プロジェクト）は [`USAGE_FOR_AI.md`](USAGE_FOR_AI.md)、
ECS は [`arimu-framework/USAGE_FOR_AI.md`](arimu-framework/USAGE_FOR_AI.md) を参照。

---

<sub>📝 この README は AI が制作・修正しています。</sub>
