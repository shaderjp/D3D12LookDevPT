# Optional DLSS Ray Reconstruction

DLSS is optional. D3D12LookDevPT defaults to the internal denoiser, and the app must keep building and running on non-NVIDIA GPUs, unsupported drivers, and builds made with `EnableDLSS=false`.

## Dependencies

The repository tracks NVIDIA dependencies as submodules:

- `ThirdParty/Streamline`: NVIDIA Streamline SDK pinned to `v2.12.0`
- `ThirdParty/DLSS`: NVIDIA DLSS SDK pinned to `v310.7.0`

Initialize them with:

```powershell
git submodule update --init --recursive
```

The DLSS SDK contains `nvngx_dlss.dll` and `nvngx_dlssd.dll`. The Streamline source checkout does not always contain prebuilt `sl.interposer.dll` binaries, so the app treats the Streamline runtime as optional at startup. Put Streamline runtime DLLs in either:

```text
Bin/x64/<Config>/Streamline/
```

or:

```text
ThirdParty/Streamline/bin/x64/
```

`Scripts/CheckSetup.ps1` reports missing DLSS runtime files as warnings by default. Use strict mode when validating a DLSS machine:

```powershell
.\Scripts\CheckSetup.ps1 -CheckDLSS
```

## Build Switch

DLSS is enabled at compile time by default:

```powershell
msbuild .\D3D12LookDevPT.sln /m /p:Configuration=Debug /p:Platform=x64
```

Dependency-free builds can disable it:

```powershell
msbuild .\D3D12LookDevPT.sln /m /p:Configuration=Debug /p:Platform=x64 /p:EnableDLSS=false
```

With `EnableDLSS=false`, the app exposes DLSS status as `compiled=false` and keeps using the internal denoiser.

## Runtime Behavior

The `Denoise` panel has a `Denoise Backend` combo:

- `Internal`: existing temporal/A-Trous denoiser
- `DLSS Ray Reconstruction`: opt-in DLSS-RR path
- `Off`: noisy progressive path tracing output

If DLSS-RR cannot run, the selected backend remains `dlss_rr`, but the active backend falls back to `internal`. The Denoise panel and MCP stats expose:

- compiled state
- Streamline runtime DLL status
- adapter support
- DLSS mode
- recommended render resolution
- output resolution
- last error
- fallback reason

![D3D12LookDevPT Denoise panel showing DLSS Ray Reconstruction fallback status](../images/screenshot004-dlss-denoise-fallback.png)

The Denoise panel keeps the selected DLSS-RR backend visible even when the active backend falls back to `internal`, making missing runtime DLLs or unsupported adapter status explicit during local setup checks.

## MCP Examples

Select DLSS-RR and Quality mode:

```json
{
  "method": "set_denoise",
  "params": {
    "backend": "dlss_rr",
    "dlssMode": "quality",
    "resetDlss": true
  }
}
```

Return to the internal denoiser:

```json
{
  "method": "set_denoise",
  "params": {
    "backend": "internal",
    "preset": "interactive_stable"
  }
}
```

Disable denoising:

```json
{
  "method": "set_denoise",
  "params": {
    "backend": "off"
  }
}
```

Read status with `lookdevpt.get_stats` or `lookdevpt.get_state` and inspect `denoiser.dlss` / `denoise.dlss`.

## Current Integration Notes

The integration is intentionally safe-first. It dynamically probes Streamline and reports support without making the app depend on Streamline DLLs at process load time. The internal denoiser remains the guaranteed rendering path. Full DLSS-RR resource tagging and evaluation require valid Streamline runtime DLLs and the DLSS-RR feature path to be ready on the target machine.
