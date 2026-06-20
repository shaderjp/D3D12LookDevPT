# D3D12LookDevPT

ドキュメント: [English](README.md) / 日本語

D3D12LookDevPT は、Direct3D 12 / DXR ベースの LookDev 用パストレーシング環境です。ラスタライズは swapchain への表示コピーと ImGui UI に限定し、シーン本体は DXR の progressive path tracer で描画します。レンダリングモードは `Baseline PT`、`ReSTIR GI`、`ReSTIR DI`、`ReSTIR GI + DI` を切り替えられます。

## プレビュー

![Bistro Exterior を ReSTIR GI + DI で表示し、MCP Server panel を開いた D3D12LookDevPT](images/screenshot001.png)

上の screenshot は local test asset として配置した Bistro Exterior scene を使っています。大きな scene asset は repository に含めない方針です。配置方法は [アセットの配置](docs/assets.ja.md) を参照してください。

## 主な機能

- Direct3D 12 Agility SDK と DXC を NuGet から利用。
- Assimp による static mesh の glTF / GLB / FBX / OBJ 読み込み。
- DirectXTex による PNG / JPEG / TGA / DDS / HDR テクスチャ読み込み。
- base color、normal、roughness、metallic、occlusion、emissive、alpha mask を扱う PBR material。
- DXR BLAS / TLAS、shader table、progressive accumulation、debug view、ReSTIR reservoir、軽量 AOV denoise。
- `.lookdevpt.json` による project 保存。
- UI と同じ action layer を使う localhost MCP server。

## セットアップ

この repository では `ThirdParty` を submodule として管理しています。clone 後に submodule を初期化してください。

```powershell
git clone https://github.com/shaderjp/D3D12LookDevPT.git
cd D3D12LookDevPT
git submodule update --init --recursive --depth 1
```

local setup の不足確認には setup checker を使えます。

```powershell
.\Scripts\CheckSetup.ps1
```

`ThirdParty` の実体は repository 本体には含めません。必要な依存は以下です。

- `ThirdParty/imgui`: ImGui docking branch
- `ThirdParty/assimp`: Assimp
- `ThirdParty/DirectXTex`: DirectXTex

## 大きなアセット

Bistro などの大きなテストアセットは repository で管理しません。容量が大きく、通常の GitHub repository には向かないため、利用者側で別途ダウンロードして配置してください。詳しくは [アセットの配置](docs/assets.ja.md) を参照してください。

例:

```text
D3D12LookDevPT/
  Bistro_v5_2/
    BistroExterior.fbx
    Textures/
```

`Bistro_v5_2/` は `.gitignore` 済みです。

## ビルド

Visual Studio 2022:

```powershell
& "C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\amd64\MSBuild.exe" .\D3D12LookDevPT.sln /m /p:Configuration=Debug /p:Platform=x64
```

Visual Studio 2026 Insiders:

```powershell
& "C:\Program Files\Microsoft Visual Studio\18\Insiders\MSBuild\Current\Bin\amd64\MSBuild.exe" .\D3D12LookDevPT.sln /m /p:Configuration=Debug /p:Platform=x64
```

project は VS 2026 では `v145`、それ以外では `v143` を選ぶ設定です。Assimp と DirectXTex の static library は `BuildThirdParty.ps1` からビルドされます。

この checkout で確認済み:

- VS 2022 `v143`: Debug x64 / Release x64
- MCP HTTP smoke test: initialize、tools/list、get_stats、resources/read、set_view、capture_viewport

この環境には VS 2026 が入っていないため、`v145` は未確認です。

## 起動

何も読み込まない場合は preview cube で起動します。

```powershell
.\Bin\x64\Debug\D3D12LookDevPT.exe
```

起動時に scene や environment を指定できます。

```powershell
.\Bin\x64\Debug\D3D12LookDevPT.exe --scene .\Bistro_v5_2\BistroExterior.fbx
.\Bin\x64\Debug\D3D12LookDevPT.exe --environment .\Bistro_v5_2\san_giuseppe_bridge_4k.hdr
```

load diagnostics は `%TEMP%\D3D12LookDevPT.log` に追記されます。

## UI

初期解像度は 1920 x 1080 です。ImGui docking を有効にしており、初回フレームで以下の panel を配置します。

- `Viewport`
- `Scene`
- `Material`
- `Lighting`
- `Path Tracing`
- `ReSTIR`
- `Denoise`
- `Diagnostics / Stats`
- `MCP Server`

UI は 18 px の default font と拡大済み spacing を使います。表示解像度を変更すると、DXGI swapchain、RTV、DXR output、accumulation、reservoir、denoise resource をまとめて resize します。

WASD / QE / Shift による camera 移動は、D3D12LookDevPT の window が foreground のときだけ有効です。

## MCP Server

`MCP Server` panel から localhost MCP endpoint を起動できます。server は default disabled です。

- Endpoint: `http://127.0.0.1:<port>/mcp`
- Default port: `8777`
- Token: `%APPDATA%\D3D12LookDevPT\settings.json` に保存
- Access mode: `Read Only`、`Confirm Mutations`、`Allow Mutations`

MCP request は server thread から D3D12 / ImGui state に直接触れません。mutation は main thread queue に積まれ、`OnUpdate()` の安全なタイミングで実行されます。

VS Code 設定、JSON-RPC 例、tools/resources、troubleshooting は [MCP サーバー詳細](docs/mcp.ja.md) を参照してください。

起動時に MCP server を明示的に start することもできます。

```powershell
.\Bin\x64\Debug\D3D12LookDevPT.exe --mcp-server --mcp-port 8777 --mcp-token <token> --mcp-access confirm_mutations
```

主な MCP tools:

- `lookdevpt.get_stats`
- `lookdevpt.get_state`
- `lookdevpt.list_materials`
- `lookdevpt.capture_viewport`
- `lookdevpt.validate_action`
- `lookdevpt.set_scene`
- `lookdevpt.set_camera`
- `lookdevpt.set_material`
- `lookdevpt.set_lighting`
- `lookdevpt.set_path_tracing`
- `lookdevpt.set_restir`
- `lookdevpt.set_denoise`
- `lookdevpt.set_view`

## Action Layer

UI と MCP は同じ action layer を通します。

```cpp
D3D12PathTracingBackend::ApplyAction(method, params, diagnostics, validateOnly)
```

対応 action:

- `set_scene`
- `set_camera`
- `set_material`
- `set_lighting`
- `set_path_tracing`
- `set_restir`
- `set_denoise`
- `set_view`

`validateOnly=true` の場合は状態を変更せず validation だけ行います。MCP の `Confirm Mutations` mode では、mutation request が ImGui の `MCP Server` panel に承認待ちとして表示されます。

## 現在の制限

- v1 は static mesh 向けです。animation、skinning、morph target は対象外です。
- shader editing、procedural scene editing、remote MCP bind、TLS、OAuth は未実装です。
- 大きな scene asset は repository に含めません。
- FBX material は Assimp から取得できる範囲で PBR / legacy 値を best effort 変換します。

## メモ

最初は preview cube で renderer を確認できます。scene を読み込む場合は `Project > Open Scene...`、HDRI / environment texture を読み込む場合は `Project > Open Environment...` を使ってください。
