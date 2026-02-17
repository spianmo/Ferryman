# Ferryman

Ferryman is a single-process remote access and execution host for LAN usage.

## Features

- C++20 backend with modular architecture.
- libhv-based HTTP + WebSocket transport layer (resolved via vcpkg/system/vendor).
- nlohmann/json-based JSON parsing for HTTP and WebSocket payloads.
- First-run bootstrap in `~/.ferryman/config.ini`:
  - `access_key`
  - listen host/ports
  - audit log path (`~/.ferryman/logs/audit.log`)
- Browser control plane:
  - access-key login + session token
  - login后即默认可用（不需要额外手动授权）
  - file browsing/read/write
  - PTY terminal sessions (`forkpty`) with ANSI stream passthrough
  - async task execution + status polling + output retrieval
  - runtime log tail（内存缓存）+ 后端实时 stdout/stderr 输出
  - WebRTC signaling channel + input event uplink
  - native screen capture stream (JPEG frame push over WS) + native input injection:
    - macOS: `ScreenCaptureKit`
    - Linux: `X11` capture + `XTest` input
    - Windows: `GDI` capture + `SendInput`
  - browser keyboard event mapping to native key events (modifiers/function keys included)
  - Linux/Windows capture encoding uses ffmpeg (`libavcodec`/`libswscale`)
- Frontend stack: Vite + React + TypeScript + react-icons + xterm.js.
- Frontend build output can be embedded into backend binary at build time.

## Repository Layout

- `include/ferryman/*`: backend headers
- `src/*`: backend implementation
- `frontend/*`: Vite React control panel
- `cmake/EmbedAssets.cmake`: embed `frontend/dist` assets into generated C++ source
- `Makefile`: one-command workflows

## Build

### 0) Install C++ dependencies (vcpkg)

```bash
make deps
```

`make deps` now uses:
- local downloads cache: `.vcpkg-downloads`
- local binary cache: `.vcpkg-binary-cache`
- `nlohmann-json` prefetch + SHA-512 verification (with mirror fallback URLs)

Optional proxy mode (uses local `useProxy` command):

```bash
make deps-proxy
```

Optional mirror/proxy envs:
- `FERRYMAN_USE_PROXY=1` enable `useProxy`
- `NLOHMANN_JSON_URL=<mirror-url>` override json archive source
- `MESON_URL=<mirror-url>` override meson archive source
- `FFMPEG_URL=<mirror-url>` override ffmpeg archive source
- `GITHUB_MIRROR_PREFIX=<prefix>` prepend a mirror prefix for GitHub URLs
- `VCPKG_ASSET_SOURCES=<asset-source-config>` pass through to `X_VCPKG_ASSET_SOURCES`

### 1) Frontend

```bash
make frontend
```

### 2) Backend

```bash
make build
```

### 3) Run

```bash
make run
```

On first run, access key is printed to stdout and written to `~/.ferryman/config.ini`.

## Frontend debug (split mode)

Run backend and frontend in separate terminals:

Terminal 1 (backend only):

```bash
make dev-backend
```

Terminal 2 (Vite dev server on `:5173`):

```bash
make dev-frontend
```

Then open `http://127.0.0.1:5173`.

In dev mode, Vite proxies:
- `/api/*` -> `http://127.0.0.1:18080`
- `/ws/*` -> `ws://127.0.0.1:18080`

Override proxy targets if needed:

```bash
cd frontend
VITE_BACKEND_HTTP_URL=http://127.0.0.1:28080 \
VITE_BACKEND_WS_URL=ws://127.0.0.1:28080 \
npm run dev -- --host
```

## One-command release build

```bash
make release
```

## Notes

- The project supports vcpkg manifest mode via `vcpkg.json`.
- If libhv is unavailable, backend still compiles, but runtime server start will fail and print guidance.
- On macOS, native input injection requires Accessibility permission for the Ferryman process.
- On macOS, native capture requires Screen Recording permission for the Ferryman process.

## Security Model

- LAN-oriented deployment (default host `0.0.0.0`).
- Access-key login required.
- Login grants access (no extra manual "authorize command/screen" gates).
- Key actions are auditable via:
  - immediate backend console logs (stdout/stderr)
  - in-memory log tail (`/api/logs/tail`)
