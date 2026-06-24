# Optional DLSS Ray Reconstruction

DLSS は optional です。D3D12LookDevPT の default は従来の internal denoiser で、非 NVIDIA GPU、未対応 driver、`EnableDLSS=false` build でも build / 起動 / 描画できることを優先しています。

## 依存関係

NVIDIA 関連は submodule として管理します。

- `ThirdParty/Streamline`: NVIDIA Streamline SDK `v2.12.0`
- `ThirdParty/DLSS`: NVIDIA DLSS SDK `v310.7.0`

初期化:

```powershell
git submodule update --init --recursive
```

DLSS SDK には `nvngx_dlss.dll` と `nvngx_dlssd.dll` が含まれます。Streamline の source checkout には prebuilt `sl.interposer.dll` が含まれない場合があるため、起動時に runtime DLL を optional probe します。Streamline runtime DLL は以下のどちらかに置いてください。

```text
Bin/x64/<Config>/Streamline/
```

または:

```text
ThirdParty/Streamline/bin/x64/
```

`Scripts\CheckSetup.ps1` は通常、DLSS runtime 不足を warning として扱います。DLSS 環境を厳密に確認する場合:

```powershell
.\Scripts\CheckSetup.ps1 -CheckDLSS
```

## Build Switch

DLSS は default で compile 有効です。

```powershell
msbuild .\D3D12LookDevPT.sln /m /p:Configuration=Debug /p:Platform=x64
```

依存なし build では無効化できます。

```powershell
msbuild .\D3D12LookDevPT.sln /m /p:Configuration=Debug /p:Platform=x64 /p:EnableDLSS=false
```

`EnableDLSS=false` では MCP / UI の DLSS status は `compiled=false` になり、rendering は internal denoiser を使います。

## 実行時の挙動

`Denoise` panel に `Denoise Backend` combo があります。

- `Internal`: 既存の temporal / A-Trous denoiser
- `DLSS Ray Reconstruction`: opt-in の DLSS-RR path
- `Off`: denoise なしの progressive path tracing output

DLSS-RR が使えない場合でも、選択 backend は `dlss_rr` のまま保持し、実行 backend は `internal` に fallback します。Denoise panel と MCP stats には以下を表示します。

- compile 状態
- Streamline runtime DLL 状態
- adapter support
- DLSS mode
- 推奨 render resolution
- output resolution
- last error
- fallback reason

![DLSS Ray Reconstruction の fallback 状態を表示している D3D12LookDevPT の Denoise panel](../images/screenshot004-dlss-denoise-fallback.png)

Denoise panel は DLSS-RR を選択した状態を残したまま、実行 backend が `internal` に fallback していることを表示します。runtime DLL 不足や adapter support の状態を local setup 時に確認できます。

## MCP 例

DLSS-RR / Quality mode を選ぶ:

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

internal denoiser に戻す:

```json
{
  "method": "set_denoise",
  "params": {
    "backend": "internal",
    "preset": "interactive_stable"
  }
}
```

denoise を切る:

```json
{
  "method": "set_denoise",
  "params": {
    "backend": "off"
  }
}
```

状態確認は `lookdevpt.get_stats` または `lookdevpt.get_state` を呼び、`denoiser.dlss` / `denoise.dlss` を見ます。

## 現在の統合メモ

この統合は safety-first です。Streamline は dynamic probe し、process load 時に DLL へ依存しません。確実に描画できる path は internal denoiser です。DLSS-RR の完全な resource tagging / evaluation には、対象環境の Streamline runtime DLL と DLSS-RR feature path が揃っている必要があります。
