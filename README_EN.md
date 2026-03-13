<p align="center">
    <img src="banner.png" style="border-radius: 12px;" alt="Ferryman banner">
</p>

# Ferryman

![C++20](https://img.shields.io/badge/C%2B%2B-20-00599C)
![CMake](https://img.shields.io/badge/CMake-3.20%2B-064F8C)
![React](https://img.shields.io/badge/React-18-61DAFB)
![Vite](https://img.shields.io/badge/Vite-5-646CFF)
![Native Stream](https://img.shields.io/badge/Native%20Stream-JPEG%2FH264%2FH265%2FVP8%2FVP9%2FAV1-0EA5E9)
![Platforms](https://img.shields.io/badge/Platforms-macOS%20%7C%20Linux%20%7C%20Windows-334155)

English | [中文](README_CN.md)

Ferryman turns a machine you trust into a browser-accessible **AI coding control plane**.
When Claude / Codex / Cursor / Gemini / OpenCode style CLI agents are running long tasks on a remote host, you should not need to babysit the machine or bounce between SSH, a web IDE, a remote desktop tool, and infra dashboards. AI also does not magically become self-explanatory just because you left your desk, grabbed coffee, or checked on it from your phone mid-walk. Ferryman keeps that workflow in one place: check progress, approve actions, browse and edit workspace files, take over the terminal, and jump into IDE, screen streaming, or host operations when needed.

Ferryman treats **mobile-friendly access** as a first-class requirement.
With a public FerrymanProxy or your own FerrymanProxy deployed on a public Linux server, you can bring your VibeCode terminal with you to practically any place with a browser. For developers who keep wondering whether the remote AI task back at home or in the office has started improvising, that peace of mind matters.

It is not just a web terminal and not just another remote desktop.
Ferryman is built for the new **AI agent + remote host** workflow: agents keep working, humans step in occasionally, and every intervention needs full context, enough control, and an auditable trail. It can also surface and import existing local Codex / Claude / Cursor / Gemini / OpenCode sessions so you can continue from the context already in your head instead of reconstructing it from memory.

## Why Developers Care About Ferryman

- **Built around AI coding rather than generic remote access**: CodeAgent, terminal, files, Git, attachments, event streams, and permission interactions live in one workflow.
- **Mobile-friendly when you need to step in fast**: phones and tablets remain useful for checking, approving, and steering long-running tasks away from your desk.
- **FerrymanProxy makes global reach practical**: once a public FerrymanProxy entry point is in place, the machine at home or in the office is no longer trapped inside one LAN.
- **Local session continuation feels natural**: existing CLI-agent transcripts can be imported so the browser UI starts with real context instead of a blank slate.
- **Brings development and operations into one entry point**: switch between `code-server`, screen control, Docker, VMs, tunnel management, and system monitoring from the same browser UI.
- **Low-friction and self-host friendly**: single process, single binary, designed for LAN/private-network deployment with minimal runtime dependencies and clear auditability.

## Coming Soon

- **Ralph Wiggum Loop**: a clearer rhythm for agent collaboration, with less "what is it secretly doing now?" and more "what is it about to do next?"
- **Developer Kanban**: a better surface for watching sessions, tasks, blockers, and next actions across multiple AI workstreams.

Ferryman combines:

- a built-in **CodeAgent** panel for Claude / Codex / Cursor / Gemini / OpenCode session workflows
- file browsing, file search, upload/attachment flow, and workspace-scoped read/write
- PTY terminal sessions and async task execution
- Git-oriented coding context such as status / diff browsing inside a session workspace
- built-in **code-server** management and embedded IDE view
- runtime logs and audit stream
- WebRTC signaling + native screen streaming and remote input injection
- Docker container management (lifecycle/metrics/logs/files)
- Dockurr VM management (create/start/stop/restart/logs/inspect)
- built-in tunnel mapping panel (FerrymanProxy integration)
- realtime device monitor dashboard (CPU/GPU/memory/disk)

The project keeps frontend and backend in one repository, uses explicit HTTP/WebSocket contracts, and pulls remote AI coding, host control, and infrastructure operations into one self-hostable browser workspace.

## Core Capabilities

### Access and Session Model

- First run bootstraps `~/.ferryman/config.ini` with a generated `access_key`.
- Login uses access key + session token (`X-Session-Token`) for all protected HTTP/WS channels.
- Multiple users can log in at the same time.
- Terminal/task contexts are scoped by session token (`owner_token`) for isolation and traceability.
- Login currently grants command/screen authorization by default (no extra manual approval step).

### Remote AI Coding

- **Built-in CodeAgent panel (hapi-compatible)**:
  - integrated directly into Ferryman UI (`#/codeagent`), rendered without iframe, independent from `code-server`
  - C++ backend endpoint set compatible with hapi web API (`/api/auth`, `/api/sessions`, `/api/events`, `/api/machines`, etc.)
  - supports Claude / Codex / Cursor / Gemini / OpenCode command templates
- **Session-based workflow**:
  - create sessions by machine, workspace directory, agent flavor, model, and permission mode
  - session lifecycle controls: resume / abort / archive / rename / delete
  - runner/session state tracking with realtime event delivery
- **Interactive coding controls**:
  - permission approvals/denials from browser UI
  - ask-user-input / question-answer flows for plan-mode style interactions
  - per-session model controls, including reasoning effort and Codex fast mode
- **Workspace context for coding**:
  - directory tree, file read/browse, file search, and session file views
  - file upload + attachment flow for prompts
  - Git status / diff browsing in the session workspace
  - dedicated session terminal over WebSocket
- **External agent history discovery**:
  - discover and import existing local transcripts from Codex / Claude / Cursor / Gemini / OpenCode
  - continue remote work from previously created local CLI agent sessions

### Runtime and Development Environment

- **Transport layer**: `libhv` HTTP + WebSocket server (single listener; WS and HTTP share one port at runtime).
- **JSON payloads**: parsed/serialized with `nlohmann/json`.
- **File operations**: list/read/write under workspace root (`$HOME` by default), with path boundary checks.
- **Terminal**: child process + PTY (`forkpty`), ANSI passthrough, browser rendering via `xterm.js` (including 256-color support).
- **Tasks**: async command execution with status lifecycle (`queued/running/succeeded/failed`), polling and output retrieval.
- **Logs**:
  - immediate backend output (`stdout/stderr`)
  - in-memory tail buffer via `/api/logs/tail`
  - realtime WS push via `/ws/logs`
- **code-server panel**:
  - detect host install state and support one-click install from UI
  - launch/restart `code-server` with configurable port and HTTP/HTTPS
  - TLS modes: `ferryman`, `selfsigned`, `custom`; runtime log: `~/.ferryman/logs/codeserver.log`

### Host and Infrastructure Operations

- **Dockurr VM manager**:
  - create/list/start/stop/restart Windows/macOS VMs
  - startup/runtime logs and inspect output
  - Linux hosts can trigger one-click KVM installation from UI when `/dev/kvm` is unavailable
- **Docker manager**:
  - container list + start/stop/restart
  - CPU/memory/network/block I/O metrics and process view
  - inspect/logs and in-container file list/read/write/upload/download
- **Tunnel (NAT traversal) panel**:
  - FerrymanProxy host/port/token configuration
  - mapping CRUD (`tcp`/`udp`) with enable/disable + online test
  - local listening ports table (address/port/process/pid)
- **Realtime monitor panel**:
  - device snapshots over `/ws/monitor`
  - CPU/GPU/memory/disk cards and trend charts
- **Screen + remote control**:
  - WebRTC room signaling (`join` / `signal`) channel
  - native screen stream over WS binary frames (`FRM1`)
  - keyboard/mouse event uplink and native input injection
  - soft-key combos for Ctrl/Alt/Meta plus Tab/Esc/system-attention shortcuts
  - drag-and-drop file transfer with conflict strategy + chunked upload session APIs
  - codec/fps/resolution/bitrate negotiation for native stream subscribers

### Screen Backends

- macOS: ScreenCaptureKit + ApplicationServices
- Linux: X11 capture + XTest input
- Windows: GDI capture + SendInput
- Encoders:
  - always available: `jpeg`
  - when ffmpeg is available: `h264`, `h265`, `vp8`, `vp9`, `av1`
- Runtime profiles:
  - FPS: `1..60`
  - Resolution tiers: `full(100%)`, `balanced(75%)`, `performance(50%)`
  - Bitrate tiers: `sd(1.5Mbps)`, `hd(3Mbps)`, `uhd(6Mbps)`

## Architecture

```text
Browser (React/Vite)
  |- /api/*  (HTTP)
  |- /ws/terminal (WebSocket)
  |- /ws/codeagent/terminal (WebSocket)
  |- /ws/codeagent/events   (WebSocket)
  |- /ws/webrtc   (WebSocket)
  |- /ws/logs     (WebSocket)
  |- /ws/dockurr  (WebSocket)
  |- /ws/monitor  (WebSocket)
  `- /ws/tunnel   (WebSocket)

Ferryman (single process)
  |- SessionManager / Auth (access key)
  |- CodeAgentManager
  |- FileService
  |- PtyManager
  |- TaskManager
  |- AuditLogger
  |- DockurrManager
  |- DockerManager
  |- TunnelManager
  |- SystemMonitor
  |- WebRtcSignalingService
  `- ScreenService + VideoEncoder (ffmpeg)
```

## CodeAgent Integration

- The CodeAgent module is a standalone panel in Ferryman UI (`#/codeagent`) and serves as Ferryman's primary **remote AI coding workspace**.
- The CodeAgent backend is fully implemented in C++ (`CodeAgentManager` + HTTP handlers in `ServerApp`) and runs in parallel with `code-server` without shared process/state coupling.
- Copied hapi frontend source is merged into `frontend/src/codeagent*` and built together with Ferryman UI rather than embedded through an iframe.
- Sessions can be created with explicit workspace, agent flavor, model, and permission mode, then controlled remotely through browser-based approvals, file views, Git context, terminal access, and realtime event streams.
- Ferryman can also surface and import local agent transcripts, making the CodeAgent panel a bridge between existing CLI-agent workflows and browser-based remote continuation.

## Repository Layout

- `include/ferryman/*`: backend headers
- `src/*`: backend implementation
- `frontend/*`: Vite + React + TypeScript control panel
- `cmake/EmbedAssets.cmake`: embed `frontend/dist` into generated C++ source
- `scripts/make_deps.sh`: dependency bootstrap
- `Makefile`: one-command workflows

## Build and Run

### 0) Install C++ dependencies (vcpkg)

```bash
make deps
```

`make deps` includes:

- local downloads cache: `.vcpkg-downloads`
- local binary cache: `.vcpkg-binary-cache`
- archive prefetch + SHA-512 verification (nlohmann-json / meson / ffmpeg), with mirror fallback URLs

Optional proxy mode (if local `useProxy` command exists):

```bash
make deps-proxy
```

Optional mirror/proxy envs:

- `FERRYMAN_USE_PROXY=1`
- `NLOHMANN_JSON_URL=<mirror-url>`
- `MESON_URL=<mirror-url>`
- `FFMPEG_URL=<mirror-url>`
- `GITHUB_MIRROR_PREFIX=<prefix>`
- `VCPKG_ASSET_SOURCES=<asset-source-config>` (passed through to `X_VCPKG_ASSET_SOURCES`)

For Windows single-exe builds without third-party DLLs, use a static triplet:

```powershell
$env:VCPKG_TARGET_TRIPLET = "x64-windows-static"
cmake -S . -B build -A x64 `
  -DCMAKE_BUILD_TYPE=Release `
  -DCMAKE_TOOLCHAIN_FILE="$env:VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" `
  -DVCPKG_TARGET_TRIPLET=$env:VCPKG_TARGET_TRIPLET
cmake --build build --config Release --parallel
```

### 1) Build frontend assets

```bash
make frontend
```

### 2) Build backend

```bash
make build
```

### 3) Run

```bash
make run
```

On first run, Ferryman generates and prints an access key, and writes config to `~/.ferryman/config.ini`.

### One-command release build

```bash
make release
```

## Split Development Mode

Run backend and frontend separately.

Terminal 1:

```bash
make dev-backend
```

Terminal 2:

```bash
make dev-frontend
```

Open:

- `http://127.0.0.1:5173`

Optional proxy override:

```bash
cd frontend
VITE_BACKEND_HTTP_URL=http://127.0.0.1:28080 \
VITE_BACKEND_WS_URL=ws://127.0.0.1:28080 \
npm run dev -- --host
```

## Runtime Configuration

Default config file: `~/.ferryman/config.ini`

```ini
access_key=<generated>
http_host=0.0.0.0
http_port=18080
https_enabled=false
https_port=18443
tls_cert_file=
tls_key_file=
ws_port=18080
codeserver_port=13337
codeserver_https_enabled=true
codeserver_https_mode=ferryman
codeserver_https_cert_file=
codeserver_https_key_file=
tunnel_proxy_host=
tunnel_proxy_port=17000
tunnel_proxy_token=
tunnel_mappings_json=[]
```

Note:

- HTTP and WebSocket share the same listener port at runtime.
- `ws_port` is still written to config for backward compatibility, but runtime forces it to match `http_port`, so it is no longer independently configurable.
- Set `https_enabled=true` to enable HTTPS/WSS. HTTP/WS stay available on `http_port`.
- If `tls_cert_file`/`tls_key_file` are empty, Ferryman auto-generates `~/.ferryman/cert/server.crt` and `~/.ferryman/cert/server.key` on first HTTPS startup.
- Auto-generated certificate paths are written back into `~/.ferryman/config.ini` (`tls_cert_file` / `tls_key_file`).
- Ferryman also initializes `~/.ferryman/logs/` and reserves `audit.log` path for audit output.
- `codeserver_port/codeserver_https_enabled/codeserver_https_mode/codeserver_https_cert_file/codeserver_https_key_file` are used by the built-in code-server panel.
- `tunnel_proxy_host/tunnel_proxy_port/tunnel_proxy_token/tunnel_mappings_json` are used by the built-in tunnel panel for NAT traversal settings.

## FerrymanProxy (Linux)

`FerrymanProxy` is a standalone public proxy server (Linux-only) for Ferryman reverse TCP/UDP port mappings, and a key building block for making Ferryman feel natural on phones, tablets, and browsers outside your home network.

Build the standalone target:

```bash
cmake --build build --target FerrymanProxy -j
# or
make build-proxy
```

Run the proxy server:

```bash
./build/FerrymanProxy --bind 0.0.0.0 --control-port 17000 --admin-host 127.0.0.1 --admin-port 17001 --log-file /var/log/ferryman-proxy.log
```

One-click deployment on public Linux (install binary + systemd + firewall):

```bash
sudo ./scripts/deploy_ferryman_proxy.sh \
  --bin ./build/FerrymanProxy \
  --bind 0.0.0.0 \
  --control-port 17000 \
  --admin-host 127.0.0.1 \
  --admin-port 17001
```

CLI inspect current mappings and modes:

```bash
./build/FerrymanProxy --list --admin-host 127.0.0.1 --admin-port 17001
./build/FerrymanProxy --status --admin-host 127.0.0.1 --admin-port 17001
./build/FerrymanProxy --logs 200 --admin-host 127.0.0.1 --admin-port 17001
```

Enable at boot via systemd template:

```bash
sudo cp scripts/ferryman-proxy.service /etc/systemd/system/ferryman-proxy.service
sudo systemctl daemon-reload
sudo systemctl enable --now ferryman-proxy
sudo systemctl status ferryman-proxy
```

## HTTP API

### Core and Host Features

| Method | Path | Description |
|---|---|---|
| `POST` | `/api/auth/login` | Access key login |
| `GET` | `/api/session/me` | Session info + host capability flags (`host_os` / `docker_installed` / `codeserver_installed` / `kvm_installed`) |
| `GET` | `/api/health` | Health check |
| `GET` | `/api/files/list` | List directory |
| `GET` | `/api/files/read` | Read file |
| `POST` | `/api/files/write` | Write file |
| `POST` | `/api/tasks/start` | Start async task |
| `GET` | `/api/tasks/list` | List tasks |
| `GET` | `/api/tasks/get` | Task detail/output |
| `GET` | `/api/logs/tail` | Tail runtime audit logs |
| `POST` | `/api/codeserver/config` | Update code-server port/TLS config, persist, and restart |

### Dockurr and Docker

| Method | Path | Description |
|---|---|---|
| `GET` | `/api/dockurr/list` | List Dockurr VMs |
| `POST` | `/api/dockurr/create` | Create VM (windows/macos, version/ram/disk/persist/name) |
| `POST` | `/api/dockurr/start` | Start VM |
| `POST` | `/api/dockurr/stop` | Stop VM |
| `POST` | `/api/dockurr/restart` | Restart VM |
| `POST` | `/api/dockurr/delete` | Delete VM |
| `GET` | `/api/dockurr/logs` | Get VM logs |
| `GET` | `/api/dockurr/inspect` | Inspect VM metadata |
| `GET` | `/api/docker/list` | List Docker containers |
| `POST` | `/api/docker/service/start` | Attempt to start the local Docker service |
| `POST` | `/api/docker/start` | Start container |
| `POST` | `/api/docker/stop` | Stop container |
| `POST` | `/api/docker/restart` | Restart container |
| `GET` | `/api/docker/logs` | Container logs |
| `GET` | `/api/docker/inspect` | Container inspect output |
| `GET` | `/api/docker/stats` | Container CPU/memory/network/block metrics |
| `GET` | `/api/docker/processes` | Container process list |
| `GET` | `/api/docker/files/list` | List files inside container path |
| `GET` | `/api/docker/files/read` | Read file inside container |
| `POST` | `/api/docker/files/write` | Write file inside container |

### Screen and Tunnel

| Method | Path | Description |
|---|---|---|
| `GET` | `/api/screen/capabilities` | Screen capability negotiation |
| `GET` | `/api/screen/sources` | List available local screens/monitors |
| `POST` | `/api/screen/input` | Native input injection |
| `POST` | `/api/screen/upload/preflight` | Check transfer conflicts before upload |
| `POST` | `/api/screen/upload/begin` | Create upload session |
| `POST` | `/api/screen/upload/chunk` | Append chunk to upload session |
| `POST` | `/api/screen/upload/commit` | Finalize upload session |
| `POST` | `/api/screen/upload/cancel` | Cancel upload session |
| `GET` | `/api/tunnel/state` | Tunnel config + mapping runtime state |
| `POST` | `/api/tunnel/config` | Update FerrymanProxy host/port/token and persist |
| `POST` | `/api/tunnel/mapping/upsert` | Add or update one TCP/UDP mapping |
| `POST` | `/api/tunnel/mapping/delete` | Delete one mapping |
| `POST` | `/api/tunnel/mapping/test` | Test one mapping and return pass/fail detail |
| `GET` | `/api/tunnel/ports` | List local listening ports/process/pid |

### CodeAgent (hapi-compatible)

| Method | Path | Description |
|---|---|---|
| `GET` | `/api/codeagent/runner/state` | Fetch aggregate runner state |
| `POST` | `/api/bind` | Establish CodeAgent bearer binding |
| `GET` | `/api/events` | Fetch CodeAgent event stream |
| `POST` | `/api/visibility` | Update event visibility |
| `GET` | `/api/sessions` | List sessions |
| `GET` | `/api/sessions/{sid}` | Fetch one session |
| `PATCH` | `/api/sessions/{sid}` | Rename session |
| `DELETE` | `/api/sessions/{sid}` | Delete session |
| `GET` | `/api/sessions/{sid}/messages` | Fetch session messages |
| `POST` | `/api/sessions/{sid}/messages` | Send session message |
| `POST` | `/api/sessions/{sid}/resume` | Resume session execution |
| `POST` | `/api/sessions/{sid}/abort` | Abort current execution |
| `POST` | `/api/sessions/{sid}/archive` | Archive session |
| `POST` | `/api/sessions/{sid}/permission-mode` | Update permission mode |
| `POST` | `/api/sessions/{sid}/model` | Update model |
| `POST` | `/api/sessions/{sid}/reasoning-effort` | Update reasoning effort |
| `POST` | `/api/sessions/{sid}/codex-fast` | Toggle Codex Fast mode |
| `POST` | `/api/sessions/{sid}/permissions/{rid}/approve` | Approve permission request |
| `POST` | `/api/sessions/{sid}/permissions/{rid}/deny` | Deny permission request |
| `GET` | `/api/sessions/{sid}/slash-commands` | Fetch slash command list |
| `GET` | `/api/sessions/{sid}/skills` | Fetch available skills |
| `GET` | `/api/sessions/{sid}/git-status` | Fetch session Git status |
| `GET` | `/api/sessions/{sid}/git-diff-numstat` | Fetch Git diff numstat |
| `GET` | `/api/sessions/{sid}/git-diff-file` | Fetch one file diff |
| `GET` | `/api/sessions/{sid}/file` | Read file in session workspace |
| `GET` | `/api/sessions/{sid}/files` | Search files in session workspace |
| `GET` | `/api/sessions/{sid}/directory` | List session workspace directory |
| `POST` | `/api/sessions/{sid}/upload` | Upload attachment into session |
| `POST` | `/api/sessions/{sid}/upload/delete` | Delete uploaded attachment |
| `GET` | `/api/machines` | List available machines |
| `POST` | `/api/machines/{mid}/spawn` | Spawn session on target machine |
| `POST` | `/api/machines/{mid}/paths/exists` | Batch-check path existence |
| `GET` | `/api/machines/{mid}/directory` | List target machine directory |

### Push and Voice

| Method | Path | Description |
|---|---|---|
| `GET` | `/api/push/vapid-public-key` | Fetch Web Push public key |
| `POST` | `/api/push/subscribe` | Register Web Push subscription |
| `DELETE` | `/api/push/subscribe` | Remove Web Push subscription |
| `POST` | `/api/voice/token` | Fetch realtime voice session token |

## WebSocket Channels

### `/ws/terminal`

Actions:

- `open`
- `attach`
- `input`
- `resize`
- `close`

### `/ws/codeagent/terminal`

Actions:

- `open`
- `input`
- `write`
- `resize`
- `close`

### `/ws/codeagent/events`

Server push only:

- CodeAgent session / machine / global event stream
- `heartbeat`

### `/ws/webrtc`

Actions:

- `join` (room signaling peer join)
- `signal` (SDP/ICE payload forwarding)
- `native_subscribe`
- `native_unsubscribe`
- `input_event`

### `/ws/logs`

Actions:

- `tail`
- `snapshot`

### `/ws/dockurr`

Actions:

- `list`
- `snapshot`
- `create`
- `start`
- `stop`
- `restart`
- `delete`
- `logs`
- `inspect`

### `/ws/monitor`

Actions:

- `snapshot`
- `refresh`
- `ping`

Server push:

- `monitor_snapshot`

### `/ws/tunnel`

Actions:

- `snapshot`
- `refresh`
- `ping`

Server push:

- `tunnel_snapshot`

## Native Screen Streaming

- Transport: WebSocket binary packet (`FRM1` header)
- Codec IDs:
  - `1`: JPEG
  - `2`: H.264
  - `3`: H.265
  - `4`: VP8
  - `5`: VP9
  - `6`: AV1
- Backend negotiates codec/fps/resolution/bitrate based on active subscribers.

If ffmpeg is unavailable, native video encoding is disabled and capability negotiation falls back accordingly.

## Security Model

- LAN-oriented deployment (default host: `0.0.0.0`).
- Access-key login required.
- Session token required for protected HTTP/WS endpoints.
- Login grants command/screen access by default (current behavior).
- Key actions are auditable through:
  - immediate backend console logs
  - in-memory log tail (`/api/logs/tail`, `/ws/logs`)
- Session-scoped ownership is applied to terminal/task operations.

## Build Notes

- `vcpkg` manifest mode via `vcpkg.json`
- Frontend assets are embedded by `cmake/EmbedAssets.cmake`
- If `libhv` is missing, backend still compiles but server startup fails with guidance.
- On macOS, native screen and input features require system permissions:
  - Screen Recording
  - Accessibility

## Contributing

Please read [CONTRIBUTING.md](CONTRIBUTING.md) before opening a PR.

## License

This project is licensed under the [MIT License](LICENSE).
