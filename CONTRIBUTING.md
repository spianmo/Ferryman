# Contributing to Ferryman

Thank you for contributing to Ferryman.
This document describes the expected workflow for code, docs, and bugfix contributions.

## Prerequisites

- CMake 3.20+
- A C++20-capable compiler
- `npm` (for frontend build/dev)
- `vcpkg` (default path: `$HOME/vcpkg`, or set `VCPKG_ROOT`)

Platform notes:

- macOS: native capture uses ScreenCaptureKit
- Linux: native capture/input uses X11 + XTest
- Windows: native capture/input uses GDI + SendInput

## Local Setup

Install dependencies:

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

Run:

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

## Repository Overview

- `include/ferryman/*`: backend headers
- `src/*`: backend implementation
- `frontend/*`: React + TypeScript UI
- `cmake/EmbedAssets.cmake`: embeds `frontend/dist` into generated C++
- `scripts/*`: dependency/bootstrap helpers

## Development Guidelines

## 1) Keep changes focused

- Prefer small, reviewable PRs.
- Avoid mixing unrelated refactors and feature changes.

## 2) Follow existing style

- Match the style already used in touched files.
- Prefer clear naming and explicit error handling.
- Keep platform-specific logic isolated by platform guards.

## 3) Update both sides for protocol changes

If you change native screen streaming protocol, update both backend and frontend together:

- `src/web/ServerApp.cpp` (packet codec bytes, negotiation, WS routing)
- `src/web/ScreenService.cpp` (capabilities, encode targets)
- `frontend/src/pages/ScreenPage.tsx` (binary parser + decoder mapping)
- `frontend/src/i18n.tsx` (new user-facing labels)

## 4) Keep i18n in sync

Any newly visible UI text should be added to both language dictionaries in:

- `frontend/src/i18n.tsx`

## Validation Before PR

Run at minimum:

```bash
make build
cd frontend && npm run build
```

Recommended manual checks:

- login/session flow
- terminal websocket interaction
- file browse/read/write
- screen start/stop and codec switching

If screen streaming logic changed, verify fallback behavior (for example, unsupported codec -> jpeg).

## Commit and PR Expectations

- Use clear commit messages in imperative form.
- Include a concise PR description:
  - what changed
  - why it changed
  - how it was validated
- For UI changes, include screenshots or short recordings.
- For protocol changes, describe compatibility impact.

## Security and Sensitive Data

- Do not commit secrets, access keys, or local runtime credentials.
- Do not commit local artifacts/logs such as:
  - `build/`
  - `frontend/dist/`
  - `frontend/node_modules/`
  - runtime log dumps

## Questions

If anything is unclear, open an issue or start a discussion before large changes.
