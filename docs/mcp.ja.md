# MCP サーバー

D3D12LookDevPT には、実行中の renderer を VS Code、Codex、独自 JSON-RPC client などから参照・操作するための local MCP server が入っています。ImGui UI と同じ validation-oriented action layer を経由します。

English documentation: [MCP Server](mcp.md)

## MCP 経由の操作例

下の screenshot は、MCP 対応 client から自然言語で camera と denoise の変更を指示し、D3D12LookDevPT 側で `lookdevpt.set_camera`、`lookdevpt.set_denoise`、`lookdevpt.get_state` が反映された流れです。

![MCP で camera と denoise を変更した後の D3D12LookDevPT](../images/screenshot002.png)

上の viewport は MCP 経由の変更後の renderer です。Bearer token の値は redacted 済みです。実 token は screenshot や project file に入れないでください。

![Codex から MCP camera と denoise 変更を指示している例](../images/screenshot003.png)

client 側では先に validation を通し、mutation tool を適用し、その後 `get_state` で renderer state が同じ値になったことを確認できます。

## 利用条件とセキュリティ

- Endpoint: `http://127.0.0.1:<port>/mcp`
- Default port: `8777`
- Bind address: `127.0.0.1` のみ
- Transport: Streamable HTTP 形式の JSON-RPC over `POST /mcp`
- 対応 protocol version: `2025-11-25`、`2025-06-18`
- 認証: `Authorization: Bearer <token>` が必須
- Session: `initialize` の response header で `MCP-Session-Id` を返し、以後の request では同じ header が必須
- Server-Sent Events: 未実装。`GET /mcp` は `405 Method Not Allowed`

Bearer token と MCP 設定は以下に保存されます。

```text
%APPDATA%\D3D12LookDevPT\settings.json
```

この file は user-local です。token を `.lookdevpt.json`、README、screenshot、issue comment、commit 済み VS Code 設定に入れないでください。

`Origin` header は absent、`null`、`http://127.0.0.1:*`、`http://localhost:*` だけ許可します。それ以外は `403` になります。

## サーバーの起動

dockable な `MCP Server` panel から操作できます。

- `Start Server` / `Stop Server`
- `Port`
- `Request Timeout`
- `Access Mode`
- `Copy Token`
- `Regenerate Token`
- pending approvals と recent request log

server は default disabled です。command line から明示的に起動することもできます。

```powershell
.\Bin\x64\Debug\D3D12LookDevPT.exe --mcp-server --mcp-port 8777 --mcp-token <token> --mcp-access confirm_mutations
```

Access mode:

- `read_only`: read tool は使えます。mutation tool は拒否されます。
- `confirm_mutations`: mutation tool は ImGui の `MCP Server` panel で Approve されるまで待ちます。
- `allow_mutations`: mutation tool を UI 承認なしで実行します。

mutation queue は main thread で処理され、同時に保持できる request は 16 件までです。HTTP server thread から D3D12 / ImGui state を直接触りません。

## VS Code 設定

VS Code の MCP server 設定は、workspace の `.vscode/mcp.json` または user profile の `mcp.json` に保存します。現在の VS Code MCP configuration reference では、HTTP server に `type`、`url`、`headers` を使い、secret には optional の `inputs` を使えます。

`.vscode/mcp.json` の例:

```json
{
  "inputs": [
    {
      "type": "promptString",
      "id": "lookdevpt-token",
      "description": "D3D12LookDevPT MCP bearer token",
      "password": true
    }
  ],
  "servers": {
    "d3d12LookDevPT": {
      "type": "http",
      "url": "http://127.0.0.1:8777/mcp",
      "headers": {
        "Authorization": "Bearer ${input:lookdevpt-token}",
        "MCP-Protocol-Version": "2025-11-25"
      }
    }
  }
}
```

編集後は VS Code の `MCP: List Servers` から server entry を start / restart してください。D3D12LookDevPT を rebuild して tool list が変わった場合は `MCP: Reset Cached Tools` を実行してください。

注意:

- VS Code 側の server entry を start する前に、D3D12LookDevPT 本体と MCP server を起動しておきます。
- ImGui で token を regenerate した場合は、VS Code 側の MCP server entry を restart し、新しい token を入力します。
- この server は HTTP POST JSON-RPC 対応です。SSE-only の MCP client では利用できません。

## JSON-RPC の流れ

client は最初に `initialize` を呼び、返ってきた session id を保持し、その後 `notifications/initialized` を送ります。

PowerShell 例:

```powershell
$endpoint = "http://127.0.0.1:8777/mcp"
$token = "<token>"
$headers = @{
  "Authorization" = "Bearer $token"
  "MCP-Protocol-Version" = "2025-11-25"
}

$initBody = @{
  jsonrpc = "2.0"
  id = 1
  method = "initialize"
  params = @{
    protocolVersion = "2025-11-25"
    capabilities = @{}
    clientInfo = @{ name = "manual-client"; version = "1.0" }
  }
} | ConvertTo-Json -Depth 10 -Compress

$init = Invoke-WebRequest -Uri $endpoint -Method Post -Headers $headers -ContentType "application/json" -Body $initBody
$sessionId = [string]$init.Headers["MCP-Session-Id"][0]

$sessionHeaders = @{
  "Authorization" = "Bearer $token"
  "MCP-Protocol-Version" = "2025-11-25"
  "MCP-Session-Id" = $sessionId
}

$initialized = @{
  jsonrpc = "2.0"
  method = "notifications/initialized"
  params = @{}
} | ConvertTo-Json -Depth 10 -Compress

Invoke-WebRequest -Uri $endpoint -Method Post -Headers $sessionHeaders -ContentType "application/json" -Body $initialized
```

read tool の呼び出し:

```powershell
$body = @{
  jsonrpc = "2.0"
  id = 2
  method = "tools/call"
  params = @{
    name = "lookdevpt.get_state"
    arguments = @{}
  }
} | ConvertTo-Json -Depth 10 -Compress

Invoke-WebRequest -Uri $endpoint -Method Post -Headers $sessionHeaders -ContentType "application/json" -Body $body
```

session の終了:

```powershell
Invoke-WebRequest -Uri $endpoint -Method Delete -Headers $sessionHeaders
```

## Tools

Read tools:

- `lookdevpt.get_stats`: adapter、DXR tier、resolution、scene counts、accumulated samples、active mode、reservoir count、denoiser state、MCP queue state を返します。
- `lookdevpt.get_state`: scene path、project path、camera、lighting、path tracing、ReSTIR、denoise、view state を返します。
- `lookdevpt.list_materials`: material 名、使用数、編集可能な PBR factor、texture slot 状態を返します。
- `lookdevpt.list_debug_views`: debug view の id、label、key を返します。
- `lookdevpt.list_render_modes`: render mode の label と action value を返します。
- `lookdevpt.get_diagnostics`: scene / project / capture / MCP diagnostics を返します。
- `lookdevpt.capture_viewport`: 現在の final/debug viewport を PNG として取得し、inline `image/png` と `lookdevpt://captures/latest.png` を返します。
- `lookdevpt.capture_debug_pack`: 最大 8 個の debug view を PNG capture し、それぞれの resource link を返します。

Validation:

- `lookdevpt.validate_action`: `{ "method": "...", "params": { ... } }` を受け取り、同じ action path を `validateOnly=true` で実行します。
- `lookdevpt.run_actions`: 複数の action-layer call をまとめて validation / apply します。validation 失敗時は一切 mutation しません。

Mutation tools:

- `lookdevpt.reset_accumulation`
- `lookdevpt.reset_denoise_history`
- `lookdevpt.reset_reservoirs`
- `lookdevpt.reset_camera_view`
- `lookdevpt.set_camera_speed`
- `lookdevpt.fit_camera_to_scene`
- `lookdevpt.set_display_resolution`
- `lookdevpt.load_project`
- `lookdevpt.save_project`
- `lookdevpt.save_project_as`
- `lookdevpt.set_scene`
- `lookdevpt.set_camera`
- `lookdevpt.set_material`
- `lookdevpt.set_material_texture`
- `lookdevpt.reset_material`
- `lookdevpt.save_material_variant`
- `lookdevpt.apply_material_variant`
- `lookdevpt.delete_material_variant`
- `lookdevpt.set_material_view`
- `lookdevpt.set_color_management`
- `lookdevpt.set_lighting`
- `lookdevpt.set_path_tracing`
- `lookdevpt.set_restir`
- `lookdevpt.set_denoise`
- `lookdevpt.set_view`

tool result は主に `structuredContent` を使います。互換用に text summary も含めます。

## Resources

- `lookdevpt://state`: 現在の state JSON。
- `lookdevpt://stats`: 現在の stats JSON。
- `lookdevpt://diagnostics`: scene、project、capture、MCP diagnostics。
- `lookdevpt://materials`: material list JSON。
- `lookdevpt://materials/{index}`: 1 material の JSON object。
- `lookdevpt://materials/{index}/textures`: 1 material の source/current/override texture slot。
- `lookdevpt://material-variants`: 保存済み per-material variant snapshot。
- `lookdevpt://material-presets`: built-in / user material preset。
- `lookdevpt://debug-views`: debug view の id、label、key。
- `lookdevpt://render-modes`: render mode と `set_path_tracing.mode` の value。
- `lookdevpt://project`: 現在の project path と dirty flag。
- `lookdevpt://scene/summary`: scene counts、bounds、lights、asset paths。
- `lookdevpt://actions/schema`: action 名と JSON input schema。
- `lookdevpt://captures/index`: memory 上の capture history。
- `lookdevpt://captures/latest.png`: 最新の PNG capture。
- `lookdevpt://captures/{id}.png`: `capture_viewport` または `capture_debug_pack` の PNG。

Resource templates:

- `lookdevpt://captures/{id}.png`
- `lookdevpt://materials/{index}`
- `lookdevpt://materials/{index}/textures`

Prompts:

- `lookdevpt.inspect_scene`: state / stats / materials / diagnostics を読み、scene を要約します。
- `lookdevpt.tune_denoise`: validation を通して安定した denoise 設定を提案・適用します。
- `lookdevpt.setup_camera_shot`: scene bounds と state を使って camera shot を作ります。
- `lookdevpt.capture_debug_review`: debug pack を capture し、見える問題を要約します。

resource read 例:

```json
{
  "jsonrpc": "2.0",
  "id": 3,
  "method": "resources/read",
  "params": {
    "uri": "lookdevpt://actions/schema"
  }
}
```

## よく使う操作

camera の取得:

```json
{
  "jsonrpc": "2.0",
  "id": 10,
  "method": "tools/call",
  "params": {
    "name": "lookdevpt.get_state",
    "arguments": {}
  }
}
```

camera の設定:

```json
{
  "jsonrpc": "2.0",
  "id": 11,
  "method": "tools/call",
  "params": {
    "name": "lookdevpt.set_camera",
    "arguments": {
      "position": [-14.7075, 7.99065, -11.7407],
      "yaw": 0.456,
      "pitch": -0.144733
    }
  }
}
```

Bistro の読み込み:

```json
{
  "jsonrpc": "2.0",
  "id": 12,
  "method": "tools/call",
  "params": {
    "name": "lookdevpt.set_scene",
    "arguments": {
      "scenePath": "D:\\Git\\D3D12LookDevPT\\Bistro_v5_2\\BistroExterior.fbx"
    }
  }
}
```

ReSTIR GI + DI に切り替え:

```json
{
  "jsonrpc": "2.0",
  "id": 13,
  "method": "tools/call",
  "params": {
    "name": "lookdevpt.set_path_tracing",
    "arguments": {
      "mode": "restir_gi_di",
      "samplesPerFrame": 2,
      "maxBounces": 4,
      "radianceClamp": 8.0
    }
  }
}
```

interactive denoise preset の設定:

```json
{
  "jsonrpc": "2.0",
  "id": 14,
  "method": "tools/call",
  "params": {
    "name": "lookdevpt.set_denoise",
    "arguments": {
      "preset": "interactive_stable",
      "temporalStability": true,
      "jitterMode": "stable16",
      "movingJitterScale": 0.25,
      "resetHistory": true
    }
  }
}
```

material factor の設定:

```json
{
  "jsonrpc": "2.0",
  "id": 15,
  "method": "tools/call",
  "params": {
    "name": "lookdevpt.set_material",
    "arguments": {
      "index": 0,
      "baseColor": [0.9, 0.76, 0.54, 1.0],
      "roughness": 0.42,
      "metallic": 0.0
    }
  }
}
```

material texture slot の override / clear:

```json
{
  "jsonrpc": "2.0",
  "id": 16,
  "method": "tools/call",
  "params": {
    "name": "lookdevpt.set_material_texture",
    "arguments": {
      "index": 0,
      "slot": "baseColor",
      "path": "D:\\LookDevTextures\\paint_basecolor.png"
    }
  }
}
```

slot override を消す場合は `"clear": true`、import 元の texture に戻す場合は `"resetToSource": true` を使います。

material variant の保存と適用:

```json
{
  "jsonrpc": "2.0",
  "id": 17,
  "method": "tools/call",
  "params": {
    "name": "lookdevpt.save_material_variant",
    "arguments": {
      "index": 0,
      "variant": "warm rough"
    }
  }
}
```

```json
{
  "jsonrpc": "2.0",
  "id": 18,
  "method": "tools/call",
  "params": {
    "name": "lookdevpt.apply_material_variant",
    "arguments": {
      "index": 0,
      "variant": "warm rough"
    }
  }
}
```

material focus と final view transform の設定:

```json
{
  "jsonrpc": "2.0",
  "id": 19,
  "method": "tools/call",
  "params": {
    "name": "lookdevpt.run_actions",
    "arguments": {
      "actions": [
        {
          "method": "set_material_view",
          "params": { "selectedMaterial": 0, "focusMode": "dim" }
        },
        {
          "method": "set_color_management",
          "params": { "toneMapper": "aces", "exposure": 0.0, "gamma": 2.2 }
        }
      ],
      "validateOnly": false,
      "stopOnError": true
    }
  }
}
```

viewport capture:

```json
{
  "jsonrpc": "2.0",
  "id": 20,
  "method": "tools/call",
  "params": {
    "name": "lookdevpt.capture_viewport",
    "arguments": {}
  }
}
```

validation 付き batch 実行:

```json
{
  "jsonrpc": "2.0",
  "id": 21,
  "method": "tools/call",
  "params": {
    "name": "lookdevpt.run_actions",
    "arguments": {
      "actions": [
        {
          "method": "set_path_tracing",
          "params": { "mode": "restir_gi_di", "samplesPerFrame": 2 }
        },
        {
          "method": "set_denoise",
          "params": { "preset": "interactive_stable", "resetHistory": true }
        }
      ],
      "validateOnly": false,
      "stopOnError": true
    }
  }
}
```

debug review pack の capture:

```json
{
  "jsonrpc": "2.0",
  "id": 22,
  "method": "tools/call",
  "params": {
    "name": "lookdevpt.capture_debug_pack",
    "arguments": {
      "views": [
        "Final",
        "Base Color",
        "World Normal",
        "Roughness",
        "Metallic",
        "Direct Signal",
        "Indirect Signal",
        "History Confidence"
      ]
    }
  }
}
```

dialog なしで project 保存:

```json
{
  "jsonrpc": "2.0",
  "id": 23,
  "method": "tools/call",
  "params": {
    "name": "lookdevpt.save_project_as",
    "arguments": {
      "path": "D:\\Git\\D3D12LookDevPT\\projects\\bistro.lookdevpt.json"
    }
  }
}
```

## Troubleshooting

- `401 Unauthorized`: token が一致していません。ImGui の `MCP Server` panel から token を copy し、client connection を再起動してください。
- `403 Forbidden`: client が許可されていない `Origin` header を送っています。
- `400 Unsupported MCP-Protocol-Version`: `2025-11-25` または `2025-06-18` を使ってください。
- `400 MCP-Session-Id is required`: 最初に `initialize` を呼び、返ってきた `MCP-Session-Id` を送ってください。
- `404 Unknown MCP session`: session が削除されたか、app/server が再起動されています。再度 initialize してください。
- `GET` の `405 Method Not Allowed`: 想定通りです。この server は SSE を実装していません。
- `confirm_mutations` で mutation request が止まる: timeout 前に ImGui の `MCP Server` panel で Approve / Reject してください。
- `MCP mutation queue is full`: pending request が終わるのを待つか、pending mutation を approve/reject するか、server を再起動してください。
- `lookdevpt://captures/latest.png` が読めない: 先に `lookdevpt.capture_viewport` を 1 回呼んでください。

## References

- [MCP lifecycle 2025-11-25](https://modelcontextprotocol.io/specification/2025-11-25/basic/lifecycle)
- [VS Code MCP configuration reference](https://code.visualstudio.com/docs/agents/reference/mcp-configuration)
