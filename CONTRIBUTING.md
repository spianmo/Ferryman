# Contributing to Ferryman

Thank you for contributing to Ferryman.
This document describes the current workflow for code, docs, and bugfix contributions.

## Prerequisites

- CMake 3.20+
- A C++20-capable compiler
- Node.js 20+ and `npm` for frontend build/dev
- `vcpkg` (default path: `$HOME/vcpkg`, or set `VCPKG_ROOT`)

Platform notes:

- macOS: native capture uses ScreenCaptureKit, and local testing needs Screen Recording and Accessibility permissions.
- Linux: native capture/input uses X11 + XTest.
- Windows: native capture/input uses GDI + SendInput, and PTY support uses ConPTY.

## Local Setup

Install C++ dependencies:

```bash
make deps
```

Build frontend assets:

```bash
make frontend
```

Build backend:

```bash
make build
```

Run locally:

```bash
make run
```

Split frontend/backend dev mode:

```bash
# terminal 1
make dev-backend

# terminal 2
make dev-frontend
```

Useful extra targets:

- `make release`: release-style local build
- `make build-proxy`: build `FerrymanProxy` on Linux
- `make deps-proxy`: install deps through local `useProxy` helper when available

## Repository Overview

- `include/ferryman/*`: backend headers
- `src/codeagent/*`: CodeAgent session lifecycle, parsing, runtime, and local transcript integration
- `src/core/*`: config, auth/session, audit, and file services
- `src/docker/*`, `src/dockurr/*`, `src/tunnel/*`: host-side infrastructure integrations
- `src/pty/*`: PTY/terminal support across platforms
- `src/screenrtc/*`: native screen capture, encoding, WebRTC signaling, and monitor logic
- `src/web/*`: HTTP/WS server bootstrap and embedded asset serving
- `frontend/src/pages/*`: Ferryman shell pages
- `frontend/src/codeagent/*`: embedded CodeAgent app
- `frontend/src/codeagent-protocol/*`: shared protocol helpers/types for CodeAgent
- `scripts/*`: dependency/bootstrap/deployment helpers

## Development Guidelines

### 1) Keep changes focused

- Prefer small, reviewable PRs.
- Avoid mixing unrelated refactors and feature changes.

### 2) Follow existing style

- Match the style already used in touched files.
- Prefer clear naming and explicit error handling.
- Keep platform-specific logic isolated with platform guards.
- When changing behavior, update the nearest documentation or inline explanation instead of leaving implied behavior behind.

### 3) Keep backend, frontend, and docs in sync

If you change a protocol, route, or user-facing workflow, update both sides together.

For screen/native streaming changes, check these areas together:

- `src/web/serverapp/ServerAppWebSocket.inc`
- `src/screenrtc/ScreenService.cpp`
- `src/screenrtc/VideoEncoder.cpp`
- `frontend/src/pages/ScreenPage.tsx`
- `frontend/src/i18n.tsx`

For CodeAgent HTTP/WS or payload changes, check these areas together:

- `src/codeagent/*`
- `src/web/modules/RouteModules.cpp`
- `src/web/serverapp/ServerAppWebSocket.inc`
- `frontend/src/api/client.ts`
- `frontend/src/codeagent/*`
- `frontend/src/codeagent-protocol/*`

For user-facing docs, build steps, API descriptions, or feature summaries, keep both README files aligned:

- `README.md`
- `README_EN.md`

### 4) Keep i18n in sync

Any newly visible UI text should be added to the right language dictionaries:

- Ferryman shell UI: `frontend/src/i18n.tsx`
- CodeAgent UI: `frontend/src/codeagent/lib/locales/en.ts`
- CodeAgent UI: `frontend/src/codeagent/lib/locales/zh-CN.ts`

## Validation Before PR

Run at minimum for code changes:

```bash
make frontend
make build
```

If you touched `FerrymanProxy`, also build the proxy target on Linux:

```bash
make build-proxy
```

Recommended manual checks for affected areas:

- login/session flow
- CodeAgent session creation, resume, and permission approval flow
- terminal WebSocket interaction
- file browse/read/write
- screen start/stop, source selection, and codec fallback behavior when screen logic changes
- Docker, Dockurr, and tunnel flows when infrastructure modules change

If you are submitting a docs-only PR or could not run a relevant validation step, say that clearly in the PR description.

## Commit and PR Expectations

- Use clear commit messages in imperative form.
- Include a concise PR description covering:
  - what changed
  - why it changed
  - how it was validated
- For UI changes, include screenshots or short recordings when helpful.
- For protocol or config changes, describe compatibility impact and any migration notes.

## Security and Sensitive Data

- Do not commit secrets, access keys, or local runtime credentials.
- Do not commit local artifacts, caches, or logs such as:
  - `build/`
  - `cmake-build-*/`
  - `frontend/dist/`
  - `frontend/node_modules/`
  - `.npm-cache/`
  - `.npmrc.local`
  - `.vcpkg-downloads/`
  - `.vcpkg-binary-cache/`
  - `vcpkg_installed/`
  - `libhv.*.log`
  - runtime log dumps

## Questions

If anything is unclear, open an issue or start a discussion before making large or cross-cutting changes.
