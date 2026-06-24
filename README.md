# D3D12LookDevPT

Documentation: English / [日本語](README.ja.md)

D3D12LookDevPT is a Direct3D 12 / DXR look-development path tracing sandbox. It uses rasterization only for the swapchain copy and ImGui UI; the scene image is rendered by a progressive DXR path tracer with Baseline PT, ReSTIR GI, ReSTIR DI, and combined ReSTIR GI + DI modes.

## Preview

![D3D12LookDevPT rendering Bistro Exterior with ReSTIR GI + DI and the MCP Server panel](images/screenshot001.png)

The screenshot above uses the Bistro Exterior scene as a local test asset. Large scene assets are not stored in this repository; see [Asset Setup](docs/assets.md) for download and placement notes.

## Scope

- Direct3D 12 Agility SDK and DXC from NuGet.
- Assimp scene import for static glTF/GLB/FBX/OBJ meshes.
- DirectXTex texture loading for PNG/JPEG/TGA/DDS/HDR paths and compressed embedded textures extracted by Assimp.
- PBR-oriented material slots: base color, normal, roughness, metallic, occlusion, emissive, alpha mask, texture overrides, material variants, and presets.
- DXR BLAS/TLAS, shader tables, progressive accumulation, debug views, ReSTIR reservoirs, and lightweight AOV denoising.
- Project files saved as `.lookdevpt.json`.
- A local MCP server for automation via the same validation-oriented action layer used by the UI.

## UI

The app starts at 1920 x 1080. The first frame creates a main ImGui DockSpace with panels for `Viewport`, `Scene`, `Material`, `Lighting`, `Path Tracing`, `ReSTIR`, `Denoise`, and `Diagnostics / Stats`. UI text uses an 18 px default font with scaled spacing. The central dock node is pass-through so the DXR output remains visible behind docked tools. The material panel has `Properties`, `Textures`, `Variants`, and `Presets` tabs for PBR factor edits, texture slot overrides, A/B review snapshots, user presets, and material focus display.

Changing the display resolution resizes the DXGI swapchain, RTVs, DXR output, accumulation, reservoir, and denoise resources together so ImGui rendering and hit testing stay in sync.

## MCP / Action Layer

`D3D12PathTracingBackend::ApplyAction(method, params, diagnostics, validateOnly)` currently accepts scene, camera, material, lighting, path tracing, ReSTIR, denoise, view, material texture, material variant, material view, and color-management actions.

The dockable `MCP Server` panel can start a localhost MCP endpoint at `http://127.0.0.1:<port>/mcp`. The server is disabled by default, uses a bearer token stored in `%APPDATA%\D3D12LookDevPT\settings.json`, and supports read-only, confirm-mutations, and allow-mutations access modes. MCP mutations are queued onto the main thread before they touch D3D12 or ImGui state.

MCP also exposes project save/load, camera fitting, display resolution changes, reset tools, debug-view capture packs, material texture/variant controls, capture resources, and reusable prompts. See [MCP Server](docs/mcp.md) for VS Code configuration, JSON-RPC examples, tools/resources/prompts, screenshots of an MCP-driven camera/denoise workflow, and troubleshooting.

## Build

Clone submodules before building:

```powershell
git submodule update --init --recursive --depth 1
```

Check the local setup:

```powershell
.\Scripts\CheckSetup.ps1
```

Visual Studio 2026 Insiders:

```powershell
& "C:\Program Files\Microsoft Visual Studio\18\Insiders\MSBuild\Current\Bin\amd64\MSBuild.exe" .\D3D12LookDevPT.sln /m /p:Configuration=Debug /p:Platform=x64
```

Visual Studio 2022:

```powershell
& "C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\amd64\MSBuild.exe" .\D3D12LookDevPT.sln /m /p:Configuration=Debug /p:Platform=x64
```

The project auto-selects `v145` on VS 2026 and `v143` otherwise. Third-party static libraries are built by `BuildThirdParty.ps1`.

Large sample assets are not stored in this repository. See [Asset Setup](docs/assets.md) for download and placement notes. Download Bistro v5.2 or other test scenes separately and place them next to the solution, for example:

```text
D3D12LookDevPT/
  Bistro_v5_2/
    BistroExterior.fbx
    Textures/
```

Scenes can be loaded at startup for debugging:

```powershell
.\Bin\x64\Debug\D3D12LookDevPT.exe --scene .\Bistro_v5_2\BistroExterior.fbx
```

Load diagnostics are appended to `%TEMP%\D3D12LookDevPT.log`.

Validated locally in this checkout:

- VS 2022 `v143`: Debug x64, Release x64.
- MCP HTTP smoke tests: initialize, tools/list, get_stats, resources/read, set_view, capture_viewport.
- The project is configured to select VS 2026 `v145` when available, but this environment only has VS 2022 installed.

## Notes

The first viewport opens on a preview cube so the renderer can be validated before a scene is loaded. Use `Project > Open Scene...` for glTF/GLB/FBX/OBJ and `Project > Open Environment...` for HDRI/environment textures.
