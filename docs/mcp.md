# MCP Server

D3D12LookDevPT includes a local MCP server for inspecting and controlling the running renderer from tools such as VS Code, Codex, or custom JSON-RPC clients. The server exposes the same validation-oriented action layer used by the ImGui UI.

Japanese documentation: [MCP サーバー](mcp.ja.md)

## Availability And Security

- Endpoint: `http://127.0.0.1:<port>/mcp`
- Default port: `8777`
- Bind address: `127.0.0.1` only
- Transport: Streamable HTTP-style JSON-RPC over `POST /mcp`
- Protocol versions accepted: `2025-11-25`, `2025-06-18`
- Authentication: `Authorization: Bearer <token>` is required
- Session: `initialize` returns `MCP-Session-Id`; all later requests must send it
- Server-Sent Events: not implemented; `GET /mcp` returns `405 Method Not Allowed`

The bearer token and MCP settings are stored in:

```text
%APPDATA%\D3D12LookDevPT\settings.json
```

This file is user-local. Do not copy the token into `.lookdevpt.json`, README files, screenshots, issue comments, or committed VS Code settings.

The server accepts browser/client `Origin` values only when absent, `null`, `http://127.0.0.1:*`, or `http://localhost:*`. Other origins are rejected with `403`.

## Starting The Server

Use the dockable `MCP Server` panel:

- `Start Server` / `Stop Server`
- `Port`
- `Request Timeout`
- `Access Mode`
- `Copy Token`
- `Regenerate Token`
- pending approvals and recent request log

The server is disabled by default. It can also be started from the command line:

```powershell
.\Bin\x64\Debug\D3D12LookDevPT.exe --mcp-server --mcp-port 8777 --mcp-token <token> --mcp-access confirm_mutations
```

Access modes:

- `read_only`: read tools work; mutation tools are rejected.
- `confirm_mutations`: mutation tools wait for approval in the ImGui `MCP Server` panel.
- `allow_mutations`: mutation tools execute without UI approval.

The mutation queue is processed on the main thread and has a limit of 16 queued requests. Mutations never touch D3D12 or ImGui state directly from the HTTP server thread.

## VS Code Configuration

VS Code stores MCP server configuration in `mcp.json`, either in `.vscode/mcp.json` or in the user profile. VS Code's current MCP configuration reference uses `type`, `url`, and `headers` for HTTP servers, with optional `inputs` for secrets.

Example `.vscode/mcp.json`:

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

Use `MCP: List Servers` to start or restart the server entry after editing the file. Use `MCP: Reset Cached Tools` if the tool list changes after rebuilding D3D12LookDevPT.

Notes:

- Start D3D12LookDevPT and its MCP server before starting the VS Code MCP entry.
- If the token is regenerated in ImGui, restart the VS Code MCP server entry and enter the new token.
- This server supports HTTP POST JSON-RPC. Clients that require SSE-only MCP will not work.

## JSON-RPC Flow

Every client should initialize first, keep the returned session id, then send `notifications/initialized`.

PowerShell example:

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

Call a read tool:

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

End a session:

```powershell
Invoke-WebRequest -Uri $endpoint -Method Delete -Headers $sessionHeaders
```

## Tools

Read tools:

- `lookdevpt.get_stats`: returns adapter, DXR tier, resolution, scene counts, accumulated samples, active mode, reservoir count, denoiser status, and MCP queue state.
- `lookdevpt.get_state`: returns scene path, project path, camera, lighting, path tracing, ReSTIR, denoise, and view state.
- `lookdevpt.list_materials`: returns material names and editable PBR factors.
- `lookdevpt.capture_viewport`: captures the current final/debug viewport as PNG and returns an inline `image/png` plus `lookdevpt://captures/latest.png`.

Validation:

- `lookdevpt.validate_action`: accepts `{ "method": "...", "params": { ... } }` and runs the same action path with `validateOnly=true`.

Mutation tools:

- `lookdevpt.set_scene`
- `lookdevpt.set_camera`
- `lookdevpt.set_material`
- `lookdevpt.set_lighting`
- `lookdevpt.set_path_tracing`
- `lookdevpt.set_restir`
- `lookdevpt.set_denoise`
- `lookdevpt.set_view`

Tool results primarily use `structuredContent`. A text content summary is also included for compatibility.

## Resources

- `lookdevpt://state`: current state JSON.
- `lookdevpt://stats`: current stats JSON.
- `lookdevpt://materials`: material list JSON.
- `lookdevpt://actions/schema`: action names and JSON input schemas.
- `lookdevpt://captures/latest.png`: most recent PNG capture.

Example resource read:

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

## Common Operations

Get camera:

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

Set camera:

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

Load Bistro:

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

Set ReSTIR GI + DI:

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

Set the interactive denoise preset:

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

Capture the viewport:

```json
{
  "jsonrpc": "2.0",
  "id": 15,
  "method": "tools/call",
  "params": {
    "name": "lookdevpt.capture_viewport",
    "arguments": {}
  }
}
```

## Troubleshooting

- `401 Unauthorized`: token mismatch. Copy the token from the ImGui `MCP Server` panel and restart the client connection.
- `403 Forbidden`: client sent a disallowed `Origin` header.
- `400 Unsupported MCP-Protocol-Version`: use `2025-11-25` or `2025-06-18`.
- `400 MCP-Session-Id is required`: call `initialize` first, then send the returned `MCP-Session-Id`.
- `404 Unknown MCP session`: the session was deleted or the app/server restarted. Initialize again.
- `405 Method Not Allowed` on `GET`: expected; this server does not implement SSE.
- Mutation request hangs in `confirm_mutations`: approve or reject it in the ImGui `MCP Server` panel before the request timeout.
- `MCP mutation queue is full`: wait for pending requests to finish, approve/reject pending mutations, or restart the server.
- `lookdevpt://captures/latest.png` fails: call `lookdevpt.capture_viewport` once before reading the resource.

## References

- [MCP lifecycle 2025-11-25](https://modelcontextprotocol.io/specification/2025-11-25/basic/lifecycle)
- [VS Code MCP configuration reference](https://code.visualstudio.com/docs/agents/reference/mcp-configuration)
