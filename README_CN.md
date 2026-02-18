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

[English](README.md) | 中文

Ferryman 是一个面向局域网场景的**单进程、单二进制远程访问与执行宿主程序**。  
启动后会在本机拉起轻量级 HTTP/WebSocket 服务，并以内嵌方式托管 Web 控制面板，支持通过浏览器完成：

- 文件系统浏览、读写
- PTY 终端会话
- 异步任务执行
- 运行日志/审计日志查看
- WebRTC 信令 + 原生屏幕流 + 远程输入回注

项目采用前后端同仓库结构，通过明确的 HTTP/WebSocket 协议交互，强调最小依赖、可审计与可扩展。

## 核心能力

### 访问与会话模型

- 首次运行会自动生成 `~/.ferryman/config.ini`，写入 `access_key`。
- 登录后获取会话令牌，后续受保护 HTTP/WS 接口通过 `X-Session-Token` 鉴权。
- 支持多人同时登录。
- 终端/任务等运行上下文按会话令牌（`owner_token`）隔离，便于区分与追踪。
- 当前行为为登录即默认授予命令与屏幕权限（无需额外手动授权步骤）。

### 运行时能力

- **传输层**：基于 `libhv` 的 HTTP + WebSocket 服务（运行时 HTTP/WS 共享同一监听端口）。
- **JSON 处理**：基于 `nlohmann/json`。
- **文件能力**：在工作根目录（默认 `$HOME`）下进行目录列举与文件读写，并带路径越界检查。
- **终端能力**：基于子进程 + PTY（`forkpty`），透传 ANSI 控制序列，前端通过 `xterm.js` 渲染（含 256 色支持）。
- **任务能力**：异步命令执行，状态流转（`queued/running/succeeded/failed`），支持轮询与输出获取。
- **日志能力**：
  - 后端即时控制台输出（`stdout/stderr`）
  - `/api/logs/tail` 内存日志尾部读取
  - `/ws/logs` 实时推送
- **屏幕与远控能力**：
  - WebRTC 房间信令（`join` / `signal`）
  - 原生屏幕流（WS 二进制 `FRM1`）
  - 键鼠事件上行 + 本地输入注入
  - 原生流按订阅者协商 codec/fps/分辨率/码率

### 屏幕采集后端

- macOS: ScreenCaptureKit + ApplicationServices
- Linux: X11 Capture + XTest
- Windows: GDI + SendInput
- 编码器支持：
  - 始终可用：`jpeg`
  - ffmpeg 可用时：`h264`、`h265`、`vp8`、`vp9`
- 运行时配置档位：
  - 帧率：`1..60`
  - 分辨率档：`full(100%)`、`balanced(75%)`、`performance(50%)`
  - 码率档：`sd(1.5Mbps)`、`hd(3Mbps)`、`uhd(6Mbps)`

## 架构概览

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

## 仓库结构

- `include/ferryman/*`: 后端头文件
- `src/*`: 后端实现
- `frontend/*`: Vite + React + TypeScript 控制台
- `cmake/EmbedAssets.cmake`: 将 `frontend/dist` 嵌入 C++ 代码
- `scripts/make_deps.sh`: 依赖安装脚本
- `Makefile`: 一键工作流入口

## 构建与运行

### 0) 安装 C++ 依赖（vcpkg）

```bash
make deps
```

`make deps` 具备：

- 本地下载缓存：`.vcpkg-downloads`
- 本地二进制缓存：`.vcpkg-binary-cache`
- nlohmann-json / meson / ffmpeg 预拉取 + SHA-512 校验 + 镜像回退

可选代理模式（本机存在 `useProxy` 命令时生效）：

```bash
make deps-proxy
```

可选镜像/代理环境变量：

- `FERRYMAN_USE_PROXY=1`
- `NLOHMANN_JSON_URL=<mirror-url>`
- `MESON_URL=<mirror-url>`
- `FFMPEG_URL=<mirror-url>`
- `GITHUB_MIRROR_PREFIX=<prefix>`
- `VCPKG_ASSET_SOURCES=<asset-source-config>`（透传为 `X_VCPKG_ASSET_SOURCES`）

### 1) 构建前端资源

```bash
make frontend
```

### 2) 构建后端

```bash
make build
```

### 3) 运行

```bash
make run
```

首次运行会打印 Access Key，并写入 `~/.ferryman/config.ini`。

### 一键发布构建

```bash
make release
```

## 前后端分离开发模式

终端 1（后端）：

```bash
make dev-backend
```

终端 2（前端）：

```bash
make dev-frontend
```

浏览器访问：

- `http://127.0.0.1:5173`

可选代理目标覆盖：

```bash
cd frontend
VITE_BACKEND_HTTP_URL=http://127.0.0.1:28080 \
VITE_BACKEND_WS_URL=ws://127.0.0.1:28080 \
npm run dev -- --host
```

## 运行配置

默认配置文件：`~/.ferryman/config.ini`

```ini
access_key=<generated>
http_host=0.0.0.0
http_port=18080
ws_port=18080
```

说明：

- 运行时 HTTP 与 WebSocket 共享同一监听端口。
- 启动时会初始化 `~/.ferryman/logs/`，并预留 `audit.log` 路径。

## HTTP API

| 方法 | 路径 | 说明 |
|---|---|---|
| `POST` | `/api/auth/login` | Access Key 登录 |
| `GET` | `/api/session/me` | 查询会话信息 |
| `GET` | `/api/files/list` | 目录列表 |
| `GET` | `/api/files/read` | 读取文件 |
| `POST` | `/api/files/write` | 写入文件 |
| `POST` | `/api/tasks/start` | 启动异步任务 |
| `GET` | `/api/tasks/list` | 任务列表 |
| `GET` | `/api/tasks/get` | 任务详情/输出 |
| `GET` | `/api/logs/tail` | 拉取运行日志 |
| `GET` | `/api/screen/capabilities` | 屏幕能力协商 |
| `POST` | `/api/screen/input` | 注入原生输入 |
| `GET` | `/api/health` | 健康检查 |

## WebSocket 通道

### `/ws/terminal`

支持动作：

- `open`
- `attach`
- `input`
- `resize`
- `close`

### `/ws/webrtc`

支持动作：

- `join`（房间信令）
- `signal`（SDP/ICE 信令转发）
- `native_subscribe`
- `native_unsubscribe`
- `input_event`

### `/ws/logs`

支持动作：

- `tail`
- `snapshot`

## 原生屏幕流协议

- 传输：WebSocket 二进制包（`FRM1` header）
- 编码 ID：
  - `1`: JPEG
  - `2`: H.264
  - `3`: H.265
  - `4`: VP8
  - `5`: VP9
- 后端会根据订阅者动态协商 codec/fps/分辨率/码率。

如果 ffmpeg 不可用，则原生视频编码能力会被禁用，并在能力协商中回退。

## 安全模型

- 默认面向局域网部署（默认监听 `0.0.0.0`）。
- 必须通过 Access Key 登录。
- 受保护 HTTP/WS 接口必须携带会话令牌。
- 当前行为为登录即授予命令/屏幕权限（无二次手动授权闸门）。
- 核心操作可通过以下路径审计：
  - 后端即时控制台日志
  - 内存日志尾部（`/api/logs/tail`、`/ws/logs`）
- 终端/任务使用会话级上下文隔离。

## 构建说明

- 使用 `vcpkg.json`（manifest mode）管理 C++ 依赖。
- 前端资源通过 `cmake/EmbedAssets.cmake` 嵌入后端。
- 当 `libhv` 缺失时，后端仍可编译，但运行时服务启动会失败并输出提示。
- macOS 原生能力依赖系统权限：
  - 屏幕录制
  - 辅助功能

## 参与贡献

提交 PR 前请先阅读 [CONTRIBUTING.md](CONTRIBUTING.md)。

## License

本项目使用 [MIT License](LICENSE)。
