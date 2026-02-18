import React, { createContext, useContext, useMemo, useState } from "react";

export type Lang = "zh-CN" | "en";

type Dict = Record<string, string>;

const DICTS: Record<Lang, Dict> = {
  "en": {
    "app.name": "Ferryman",
    "nav.files": "Files",
    "nav.terminal": "Terminal",
    "nav.tasks": "Tasks",
    "nav.screen": "Screen",
    "nav.logs": "Logs",
    "nav.logout": "Log out",
    "top.search": "Search…",
    "top.theme": "Theme",
    "top.lang": "Language",
    "theme.light": "Light",
    "theme.dark": "Dark",
    "common.save": "Save",
    "common.go": "Go",
    "common.refresh": "Refresh",
    "common.up": "Up",
    "common.on": "On",
    "common.off": "Off",

    "login.title": "Ferryman",
    "login.subtitle": "Enter the Access Key to start a session.",
    "login.access_key": "Access Key",
    "login.placeholder": "e.g. f41f…",
    "login.submit": "Sign in",
    "login.submitting": "Signing in…",

    "toast.login_ok": "Signed in.",
    "toast.login_failed": "Invalid access key.",
    "toast.session_expired": "Session expired. Please sign in again.",
    "toast.request_failed": "Request failed.",
    "toast.saved": "Saved.",
    "toast.task_started": "Task started.",

    "files.title": "Files",
    "files.editor": "Editor",
    "files.view_list": "List view",
    "files.view_grid": "Grid view",
    "files.empty": "No entries.",
    "files.no_match": "No matches.",
    "files.select_file": "Select a file to view/edit.",
    "files.saved_path": "Saved: {path}",

    "tasks.title": "Tasks",
    "tasks.output": "Output",
    "tasks.command_placeholder": "Type a command",
    "tasks.run": "Run",
    "tasks.empty": "No tasks yet.",
    "tasks.select_hint": "Select a task to view output.",

    "terminal.title": "Terminal",
    "terminal.status": "Status",
    "terminal.connect": "Connect",
    "terminal.close": "Close",
    "terminal.id": "Terminal ID",
    "terminal.disconnected": "Disconnected",
    "terminal.connecting": "Connecting…",
    "terminal.connected": "Connected",
    "terminal.closed": "Closed",
    "terminal.failed": "Failed",

    "screen.title": "Screen",
    "screen.join": "Join",
    "screen.share": "Share",
    "screen.native_start": "Start native",
    "screen.native_stop": "Stop native",
    "screen.fullscreen": "Fullscreen",
    "screen.exit_fullscreen": "Exit fullscreen",
    "screen.keyboard": "Keyboard",
    "screen.me": "Me",
    "screen.peers": "Peers",
    "screen.native": "Native",
    "screen.native_stream": "Native stream",
    "screen.codec": "Codec",
    "screen.fps": "FPS",
    "screen.resolution": "Resolution",
    "screen.resolution_full": "Original",
    "screen.resolution_balanced": "Balanced",
    "screen.resolution_performance": "Performance",
    "screen.bitrate": "Bitrate",
    "screen.bitrate_sd": "SD",
    "screen.bitrate_hd": "HD",
    "screen.bitrate_uhd": "UHD",
    "screen.real_fps": "Real FPS",
    "screen.latency": "Latency",
    "screen.codec_jpeg": "JPEG",
    "screen.codec_h264": "H.264",
    "screen.codec_h265": "H.265",
    "screen.codec_vp8": "VP8",
    "screen.codec_vp9": "VP9",
    "screen.native_wait": "Waiting for frames…",
    "screen.native_start_hint": "Start native stream",

    "logs.title": "Logs",
    "logs.refresh": "Refresh",
    "logs.empty": "No logs yet.",
  },
  "zh-CN": {
    "app.name": "Ferryman",
    "nav.files": "文件",
    "nav.terminal": "终端",
    "nav.tasks": "任务",
    "nav.screen": "屏幕",
    "nav.logs": "日志",
    "nav.logout": "退出登录",
    "top.search": "搜索…",
    "top.theme": "主题",
    "top.lang": "语言",
    "theme.light": "浅色",
    "theme.dark": "深色",
    "common.save": "保存",
    "common.go": "前往",
    "common.refresh": "刷新",
    "common.up": "上级",
    "common.on": "开启",
    "common.off": "关闭",

    "login.title": "Ferryman",
    "login.subtitle": "输入 Access Key 以建立会话凭证。",
    "login.access_key": "Access Key",
    "login.placeholder": "例如：f41f…",
    "login.submit": "登录",
    "login.submitting": "验证中…",

    "toast.login_ok": "登录成功。",
    "toast.login_failed": "Access Key 无效。",
    "toast.session_expired": "会话已过期，请重新登录。",
    "toast.request_failed": "请求失败。",
    "toast.saved": "已保存。",
    "toast.task_started": "任务已启动。",

    "files.title": "文件",
    "files.editor": "编辑",
    "files.view_list": "列表视图",
    "files.view_grid": "网格视图",
    "files.empty": "目录为空。",
    "files.no_match": "未匹配到文件。",
    "files.select_file": "选择一个文件进行查看与编辑。",
    "files.saved_path": "已保存：{path}",

    "tasks.title": "任务",
    "tasks.output": "输出",
    "tasks.command_placeholder": "输入命令",
    "tasks.run": "执行",
    "tasks.empty": "暂无任务。",
    "tasks.select_hint": "选择任务查看输出。",

    "terminal.title": "远程终端",
    "terminal.status": "状态",
    "terminal.connect": "连接",
    "terminal.close": "关闭",
    "terminal.id": "终端 ID",
    "terminal.disconnected": "未连接",
    "terminal.connecting": "连接中…",
    "terminal.connected": "已连接",
    "terminal.closed": "连接已关闭",
    "terminal.failed": "连接失败",

    "screen.title": "屏幕",
    "screen.join": "加入",
    "screen.share": "共享",
    "screen.native_start": "启动原生监控",
    "screen.native_stop": "停止原生监控",
    "screen.fullscreen": "全屏",
    "screen.exit_fullscreen": "退出全屏",
    "screen.keyboard": "键盘",
    "screen.me": "我",
    "screen.peers": "在线 Peer",
    "screen.native": "原生监控",
    "screen.native_stream": "原生桌面流（来自 Ferryman 后端）",
    "screen.codec": "编码器",
    "screen.fps": "帧率",
    "screen.resolution": "分辨率",
    "screen.resolution_full": "原始",
    "screen.resolution_balanced": "均衡",
    "screen.resolution_performance": "流畅",
    "screen.bitrate": "码率",
    "screen.bitrate_sd": "标清",
    "screen.bitrate_hd": "高清",
    "screen.bitrate_uhd": "超清",
    "screen.real_fps": "真实帧率",
    "screen.latency": "延迟",
    "screen.codec_jpeg": "JPEG",
    "screen.codec_h264": "H.264",
    "screen.codec_h265": "H.265",
    "screen.codec_vp8": "VP8",
    "screen.codec_vp9": "VP9",
    "screen.native_wait": "等待屏幕帧…",
    "screen.native_start_hint": "暂无原生屏幕帧，点击“启动原生监控”。",

    "logs.title": "运行日志",
    "logs.refresh": "刷新",
    "logs.empty": "暂无日志。",
  },
};

function detectLang(): Lang {
  const raw = (typeof navigator !== "undefined" ? navigator.language : "en").toLowerCase();
  if (raw.startsWith("zh")) return "zh-CN";
  return "en";
}

function format(template: string, vars?: Record<string, string | number>) {
  if (!vars) return template;
  return template.replace(/\{(\w+)\}/g, (_, key: string) => String(vars[key] ?? `{${key}}`));
}

type I18nApi = {
  lang: Lang;
  setLang: (next: Lang) => void;
  t: (key: string, vars?: Record<string, string | number>) => string;
};

const I18nContext = createContext<I18nApi | null>(null);

const STORAGE_KEY = "ferryman.lang";

export function I18nProvider({ children }: { children: React.ReactNode }) {
  const [lang, setLangState] = useState<Lang>(() => {
    const stored = localStorage.getItem(STORAGE_KEY) as Lang | null;
    return stored === "en" || stored === "zh-CN" ? stored : detectLang();
  });

  const api = useMemo<I18nApi>(() => {
    return {
      lang,
      setLang: (next) => {
        setLangState(next);
        localStorage.setItem(STORAGE_KEY, next);
      },
      t: (key, vars) => {
        const dict = DICTS[lang] ?? DICTS.en;
        const fallback = DICTS.en[key];
        const raw = dict[key] ?? fallback ?? key;
        return format(raw, vars);
      },
    };
  }, [lang]);

  return <I18nContext.Provider value={api}>{children}</I18nContext.Provider>;
}

export function useI18n() {
  const ctx = useContext(I18nContext);
  if (!ctx) throw new Error("useI18n must be used within I18nProvider");
  return ctx;
}
