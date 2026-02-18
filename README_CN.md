<p align="center">
    <img src="banner.png" style="border-radius: 12px;" alt="Ferryman banner">
</p>

# Ferryman

"'![C++20](https://img.shields.io/badge/C%2B%2B-20-00599C)
![CMake](https://img.shields.io/badge/CMake-3.20%2B-064F8C)
![React](https://img.shields.io/badge/React-18-61DAFB)
![Vite](https://img.shields.io/badge/Vite-5-646CFF)
![Native Stream](https://img.shields.io/badge/Native%20Stream-JPEG%2FH264%2FH265%2FVP8%2FVP9-0EA5E9)
![Platforms](https://img.shields.io/badge/Platforms-macOS%20%7C%20Linux%20%7C%20Windows-334155)

[English](README.md) | 中文

Ferryman 是一个面向局域网场景的**单进程、单二进制远程访问宿主**。
它提供浏览器控制台，覆盖文件管理、远程终端、异步任务、日志、WebRTC 信令与原生屏幕流。

## 核心能力

- 基于 C++20 的模块化后端。
- 基于 `libhv` 的 HTTP + WebSocket 服务。
- 原生屏幕采集与输入注入：
  - macOS: ScreenCaptureKit + ApplicationServices
  - Linux: X11 Capture + XTest
  - Windows: GDI + SendInput
- 原生屏幕流通过 WebSocket 二进制帧传输。
- 屏幕编码支持：`jpeg`、`h264`、`h265`、`vp8`、`vp9`（依赖 ffmpeg 编码器可用性）。
- 运行时配置档位：
  - 帧率：`1..60`
  - 分辨率档：`full(100%)`、`balanced(75%)`、`performance(50%)`
  - 码率档：`sd(1.5Mbps)`、`hd(3Mbps)`、`uhd(6Mbps)`
- 前端（Vite + React + TypeScript）可在构建时嵌入后端二进制。
- 首次运行自动生成 `~/.ferryman/config.ini`。

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

## 快速开始

### 1) 安装依赖

```bash
make deps
```

可选代理模式：

```bash
make deps-proxy
```

### 2) 构建前端资源

```bash
make frontend
```

### 3) 构建后端

```bash
make build
```

### 4) 启动

```bash
make run
```

首次运行会打印并写入 Access Key，配置文件位置：

- `~/.ferryman/config.ini`

## 前后端分离开发模式

终端 1：

```bash
make dev-backend
```

终端 2：

```bash
make dev-frontend
```

浏览器打开：

- `http://127.0.0.1:5173`

可选代理地址覆盖：

```bash
cd frontend
VITE_BACKEND_HTTP_URL=http://127.0.0.1:28080 '"\\
VITE_BACKEND_WS_URL=ws://127.0.0.1:28080 \\
npm run dev -- --host
"'```

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
- `signal`（SDP/ICE 转发）
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

## 构建说明

- 使用 `vcpkg.json`（manifest mode）管理 C++ 依赖。
- 前端资源通过 `cmake/EmbedAssets.cmake` 嵌入后端。
- macOS 原生能力依赖系统权限：
  - 屏幕录制
  - 辅助功能

## 项目结构

- `include/ferryman/*`: 头文件
- `src/*`: C++ 实现
- `frontend/*`: React/Vite 控制台
- `scripts/make_deps.sh`: 依赖安装脚本
- `CONTRIBUTING.md`: 贡献指南

## 参与贡献

提交 PR 前请先阅读 [CONTRIBUTING.md](CONTRIBUTING.md)。

## License

当前仓库尚未提供顶层 LICENSE 文件。
