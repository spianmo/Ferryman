<p align="center">
    <img src="banner.png" style="border-radius: 12px;" alt="Ferryman banner">
</p>

# Ferryman

![C++20](https://img.shields.io/badge/C%2B%2B-20-00599C)
![CMake](https://img.shields.io/badge/CMake-3.20%2B-064F8C)
![React](https://img.shields.io/badge/React-18-61DAFB)
![Vite](https://img.shields.io/badge/Vite-5-646CFF)
![Native Stream](https://img.shields.io/badge/Native%20Stream-JPEG%2FH264%2FH265%2FVP8%2FVP9-0EA5E9)
![Platforms](https://img.shields.io/badge/Platforms-macOS%20%7C%20Linux%20%7C%20Windows-334155)

English | [中文](README_CN.md)

Ferryman is a **single-process, single-binary remote access host** for LAN usage.
It provides a browser control plane for files, terminal, async tasks, logs, WebRTC signaling, and native screen streaming.

## Highlights

- C++20 backend with modular services.
- HTTP + WebSocket server based on `libhv`.
- Native screen capture + input injection:
  - macOS: ScreenCaptureKit + ApplicationServices
  - Linux: X11 capture + XTest input
  - Windows: GDI capture + SendInput
- Native screen stream over WebSocket binary frames.
- Screen codecs: `jpeg`, `h264`, `h265`, `vp8`, `vp9` (if ffmpeg encoders are available).
- Runtime profiles:
  - FPS: `1..60`
  - Resolution tiers: `full(100%)`, `balanced(75%)`, `performance(50%)`
  - Bitrate tiers: `sd(1.5Mbps)`, `hd(3Mbps)`, `uhd(6Mbps)`
- Built-in browser app (Vite + React + TypeScript), embedded into backend at build time.
- First-run bootstrap config at `~/.ferryman/config.ini`.

## Architecture

```text
Browser (React/Vite)
  |- /api/*  (HTTP)
  |- /ws/terminal (WebSocket)
  |- /ws/webrtc   (WebSocket)
  `- /ws/logs     (WebSocket)

Ferryman (single process)
  |- SessionManager / Auth (access key)
  |- FileService
  |- PtyManager
  |- TaskManager
  |- AuditLogger
  |- WebRtcSignalingService
  `- ScreenService + VideoEncoder (ffmpeg)
```

## Quick Start

### 1) Install dependencies

```bash
make deps
```

Optional proxy mode:

```bash
make deps-proxy
```

### 2) Build frontend assets

```bash
make frontend
```

### 3) Build backend

```bash
make build
```

### 4) Run

```bash
make run
```

On first run, Ferryman generates and prints an access key, and writes config to:

- `~/.ferryman/config.ini`

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
ws_port=18080
```

Note:

- HTTP and WebSocket share the same listener port at runtime.

## HTTP API

| Method | Path | Description |
|---|---|---|
| `POST` | `/api/auth/login` | Access key login |
| `GET` | `/api/session/me` | Session info |
| `GET` | `/api/files/list` | List directory |
| `GET` | `/api/files/read` | Read file |
| `POST` | `/api/files/write` | Write file |
| `POST` | `/api/tasks/start` | Start async task |
| `GET` | `/api/tasks/list` | List tasks |
| `GET` | `/api/tasks/get` | Task detail/output |
| `GET` | `/api/logs/tail` | Tail runtime audit logs |
| `GET` | `/api/screen/capabilities` | Screen capability negotiation |
| `POST` | `/api/screen/input` | Native input injection |
| `GET` | `/api/health` | Health check |

## WebSocket Channels

### `/ws/terminal`

Actions:

- `open`
- `attach`
- `input`
- `resize`
- `close`

### `/ws/webrtc`

Actions:

- `join` (room signaling)
- `signal` (SDP/ICE forwarding)
- `native_subscribe`
- `native_unsubscribe`
- `input_event`

### `/ws/logs`

Actions:

- `tail`
- `snapshot`

## Native Screen Streaming

- Transport: WebSocket binary packet (`FRM1` header)
- Codec IDs:
  - `1`: JPEG
  - `2`: H.264
  - `3`: H.265
  - `4`: VP8
  - `5`: VP9
- Backend negotiates codec/fps/resolution/bitrate based on active subscribers.

If ffmpeg is unavailable, native video encoding is disabled and capability negotiation falls back accordingly.

## Build Notes

- `vcpkg` manifest mode via `vcpkg.json`
- Frontend assets are embedded by `cmake/EmbedAssets.cmake`
- On macOS, native screen and input features require system permissions:
  - Screen Recording
  - Accessibility

## Project Layout

- `include/ferryman/*`: headers
- `src/*`: C++ implementation
- `frontend/*`: React/Vite control panel
- `scripts/make_deps.sh`: dependency bootstrap
- `CONTRIBUTING.md`: contribution workflow

## Contributing

Please read [CONTRIBUTING.md](CONTRIBUTING.md) before opening a PR.

## License

A top-level project license file is not included yet.
