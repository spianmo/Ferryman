import { useCallback, useEffect, useMemo, useRef, useState } from "react";
import { FiCode, FiExternalLink, FiMaximize2, FiMinimize2, FiRefreshCw, FiX } from "react-icons/fi";

import { getTask, startTask } from "../api/client";
import { useI18n } from "../i18n";
import { toast } from "../toast";
import type { SessionInfo } from "../types";
import { decodeBase64Utf8 } from "../util/codec";
import { cn } from "../util/cn";

type Props = {
  session: SessionInfo;
  hostOs: string;
  codeServerInstalled: boolean;
};

type FullscreenDocument = Document & {
  webkitFullscreenElement?: Element | null;
  webkitExitFullscreen?: () => Promise<void> | void;
};

type FullscreenPanel = HTMLDivElement & {
  webkitRequestFullscreen?: () => Promise<void> | void;
};

const DEFAULT_CODE_SERVER_PORT = 13337;
const CODE_SERVER_PORT_STORAGE_KEY = "ferryman.codeserver.port";

function parsePort(value: string): number | null {
  const trimmed = value.trim();
  if (!/^\d+$/.test(trimmed)) {
    return null;
  }
  const parsed = Number(trimmed);
  if (!Number.isInteger(parsed) || parsed < 1 || parsed > 65535) {
    return null;
  }
  return parsed;
}

function loadStoredPortInput() {
  if (typeof window === "undefined") {
    return String(DEFAULT_CODE_SERVER_PORT);
  }
  const raw = window.localStorage.getItem(CODE_SERVER_PORT_STORAGE_KEY);
  if (!raw) {
    return String(DEFAULT_CODE_SERVER_PORT);
  }
  const parsed = parsePort(raw);
  return parsed == null ? String(DEFAULT_CODE_SERVER_PORT) : String(parsed);
}

function buildInstallCodeServerCommand(hostOs: string, port: number) {
  if (hostOs === "linux" || hostOs === "macos" || hostOs === "freebsd") {
    return `
set -e
echo "[codeserver] checking current status..."
if ! command -v code-server >/dev/null 2>&1; then
  echo "[codeserver] installing via official script..."
  curl -fsSL https://code-server.dev/install.sh | sh
else
  echo "[codeserver] code-server already installed."
fi

if ! command -v code-server >/dev/null 2>&1; then
  echo "[codeserver] code-server not found after installation."
  exit 1
fi

echo "[codeserver] starting code-server on 0.0.0.0:${port} ..."
if command -v pkill >/dev/null 2>&1; then
  pkill -f "code-server.*--bind-addr 0.0.0.0:${port}" || true
fi
mkdir -p "\${HOME}/.ferryman/logs"
nohup code-server --bind-addr 0.0.0.0:${port} --auth none > "\${HOME}/.ferryman/logs/codeserver.log" 2>&1 &
sleep 2

if command -v curl >/dev/null 2>&1; then
  if curl -fsS "http://127.0.0.1:${port}/healthz" >/dev/null 2>&1; then
    echo "[codeserver] health check passed."
  else
    echo "[codeserver] health check failed. check \${HOME}/.ferryman/logs/codeserver.log"
  fi
fi

code-server --version || true
echo "[codeserver] done."
`.trim();
  }

  if (hostOs === "windows") {
    return [
      "powershell -NoProfile -NonInteractive -ExecutionPolicy Bypass -Command",
      `"`,
      "$ErrorActionPreference='Stop';",
      "Write-Host '[codeserver] checking current status...';",
      "if (-not (Get-Command code-server -ErrorAction SilentlyContinue)) {",
      "  if (-not (Get-Command npm -ErrorAction SilentlyContinue)) {",
      "    Write-Host '[codeserver] npm not found.';",
      "    exit 1",
      "  };",
      "  npm install -g code-server",
      "} else {",
      "  Write-Host '[codeserver] code-server already installed.'",
      "};",
      "$exe = (Get-Command code-server -ErrorAction Stop).Source;",
      `$args = '--bind-addr','0.0.0.0:${port}','--auth','none';`,
      "Start-Process -FilePath $exe -ArgumentList $args -WindowStyle Hidden;",
      "Start-Sleep -Seconds 2;",
      "code-server --version;",
      "Write-Host '[codeserver] done.'",
      `"`,
    ].join(" ");
  }

  return "";
}

function panelUrlForCurrentHost(port: number) {
  if (typeof window === "undefined") {
    return `http://127.0.0.1:${port}/`;
  }
  const hostname = window.location.hostname;
  const wrappedHost = hostname.includes(":") ? `[${hostname}]` : hostname;
  return `http://${wrappedHost}:${port}/`;
}

function getFullscreenElement() {
  const doc = document as FullscreenDocument;
  return document.fullscreenElement ?? doc.webkitFullscreenElement ?? null;
}

export default function CodeServerPage({ session, hostOs, codeServerInstalled }: Props) {
  const { t } = useI18n();
  const panelRef = useRef<HTMLDivElement | null>(null);
  const installViewportRef = useRef<HTMLPreElement | null>(null);
  const installNotifiedRef = useRef(false);

  const [installedState, setInstalledState] = useState(codeServerInstalled);
  const [installDialogOpen, setInstallDialogOpen] = useState(false);
  const [installTaskId, setInstallTaskId] = useState("");
  const [installStatus, setInstallStatus] = useState("");
  const [installLogs, setInstallLogs] = useState("");
  const [installRunning, setInstallRunning] = useState(false);
  const [iframeKey, setIframeKey] = useState(0);
  const [panelFullscreen, setPanelFullscreen] = useState(false);
  const [portInput, setPortInput] = useState(() => loadStoredPortInput());

  const parsedPort = useMemo(() => parsePort(portInput), [portInput]);
  const effectivePort = parsedPort ?? DEFAULT_CODE_SERVER_PORT;
  const panelUrl = useMemo(() => panelUrlForCurrentHost(effectivePort), [effectivePort]);

  const installSupportedHost = hostOs === "linux" || hostOs === "macos" || hostOs === "windows" || hostOs === "freebsd";
  const canInstallCodeServer = installSupportedHost && !installedState;

  useEffect(() => {
    setInstalledState(codeServerInstalled);
  }, [codeServerInstalled]);

  useEffect(() => {
    const viewport = installViewportRef.current;
    if (!viewport) {
      return;
    }
    viewport.scrollTop = viewport.scrollHeight;
  }, [installLogs]);

  useEffect(() => {
    if (typeof window === "undefined") {
      return;
    }
    if (parsedPort == null) {
      window.localStorage.removeItem(CODE_SERVER_PORT_STORAGE_KEY);
      return;
    }
    window.localStorage.setItem(CODE_SERVER_PORT_STORAGE_KEY, String(parsedPort));
  }, [parsedPort]);

  useEffect(() => {
    const onFullscreenChange = () => {
      const panel = panelRef.current;
      setPanelFullscreen(panel != null && getFullscreenElement() === panel);
    };

    onFullscreenChange();
    document.addEventListener("fullscreenchange", onFullscreenChange);
    document.addEventListener("webkitfullscreenchange", onFullscreenChange);
    return () => {
      document.removeEventListener("fullscreenchange", onFullscreenChange);
      document.removeEventListener("webkitfullscreenchange", onFullscreenChange);
    };
  }, []);

  const openInNewTab = useCallback(() => {
    window.open(panelUrl, "_blank", "noopener,noreferrer");
  }, [panelUrl]);

  const togglePanelFullscreen = useCallback(async () => {
    const panel = panelRef.current as FullscreenPanel | null;
    const doc = document as FullscreenDocument;
    if (!panel) {
      return;
    }

    try {
      const current = getFullscreenElement();
      if (current === panel) {
        if (typeof document.exitFullscreen === "function") {
          await document.exitFullscreen();
          return;
        }
        if (typeof doc.webkitExitFullscreen === "function") {
          await doc.webkitExitFullscreen();
          return;
        }
        toast.error(t("codeserver.fullscreen_unsupported"));
        return;
      }

      if (current) {
        if (typeof document.exitFullscreen === "function") {
          await document.exitFullscreen();
        } else if (typeof doc.webkitExitFullscreen === "function") {
          await doc.webkitExitFullscreen();
        }
      }

      if (typeof panel.requestFullscreen === "function") {
        await panel.requestFullscreen();
        return;
      }
      if (typeof panel.webkitRequestFullscreen === "function") {
        await panel.webkitRequestFullscreen();
        return;
      }

      toast.error(t("codeserver.fullscreen_unsupported"));
    } catch {
      toast.error(t("codeserver.fullscreen_failed"));
    }
  }, [t]);

  const runInstallCodeServer = useCallback(async () => {
    if (installRunning) {
      return;
    }
    if (parsedPort == null) {
      toast.error(t("codeserver.invalid_port"));
      return;
    }
    const command = buildInstallCodeServerCommand(hostOs, parsedPort);
    if (!command) {
      toast.error(t("codeserver.install_unsupported"));
      return;
    }

    setInstallDialogOpen(true);
    setInstallTaskId("");
    setInstallStatus("queued");
    setInstallLogs("");
    setInstallRunning(true);
    installNotifiedRef.current = false;

    const res = await startTask(session.token, command);
    if (!res.ok || !res.task_id) {
      setInstallStatus("failed");
      setInstallRunning(false);
      setInstallLogs(`[error] ${res.error ?? t("toast.request_failed")}`);
      toast.error(res.error ?? t("toast.request_failed"));
      return;
    }
    setInstallTaskId(res.task_id);
  }, [hostOs, installRunning, parsedPort, session.token, t]);

  useEffect(() => {
    if (!installDialogOpen || !installTaskId) {
      return;
    }
    let stopped = false;
    const poll = async () => {
      const res = await getTask(session.token, installTaskId);
      if (stopped) {
        return;
      }
      if (!res.ok) {
        setInstallStatus("failed");
        setInstallRunning(false);
        setInstallLogs((prev) => {
          if (prev.includes("[error]")) {
            return prev;
          }
          const suffix = `[error] ${res.error ?? t("toast.request_failed")}`;
          return prev ? `${prev}\n${suffix}` : suffix;
        });
        if (!installNotifiedRef.current) {
          installNotifiedRef.current = true;
          toast.error(res.error ?? t("toast.request_failed"));
        }
        return;
      }

      const output = decodeBase64Utf8(res.output_base64 ?? "");
      setInstallLogs(output);
      const nextStatus = typeof res.status === "string" ? res.status : "";
      setInstallStatus(nextStatus);
      const finished = nextStatus === "succeeded" || nextStatus === "failed";
      if (!finished) {
        return;
      }

      setInstallRunning(false);
      if (installNotifiedRef.current) {
        return;
      }
      installNotifiedRef.current = true;
      if (nextStatus === "succeeded") {
        setInstalledState(true);
        setIframeKey((prev) => prev + 1);
        toast.success(t("codeserver.install_success"));
      } else {
        toast.error(t("codeserver.install_failed"));
      }
    };

    void poll();
    const timer = window.setInterval(() => {
      void poll();
    }, 1000);
    return () => {
      stopped = true;
      window.clearInterval(timer);
    };
  }, [installDialogOpen, installTaskId, session.token, t]);

  return (
    <>
      <div className="grid h-full min-h-[380px] grid-cols-1">
        <section className="flex min-h-0 min-w-0 flex-col overflow-hidden rounded-3xl bg-white/75 p-4 shadow-soft ring-1 ring-slate-200/70 backdrop-blur dark:bg-neutral-900/60 dark:ring-neutral-800/80">
          <div className="flex flex-wrap items-center justify-between gap-2">
            <div>
              <h3 className="inline-flex items-center gap-2 text-sm font-semibold text-slate-900 dark:text-neutral-50">
                <FiCode />
                {t("codeserver.title")}
              </h3>
              <div className="mt-1 text-xs text-slate-500 dark:text-neutral-400">{t("codeserver.subtitle")}</div>
            </div>

            <div className="flex flex-wrap items-center gap-2">
              <div className="inline-flex items-center gap-2">
                <span className="text-xs text-slate-500 dark:text-neutral-400">{t("codeserver.port_label")}</span>
                <input
                  type="text"
                  inputMode="numeric"
                  value={portInput}
                  onChange={(event) => setPortInput(event.target.value.replace(/[^\d]/g, "").slice(0, 5))}
                  className={cn(
                    "h-8 w-24 rounded-xl border bg-white px-2.5 text-xs shadow-sm outline-none dark:bg-neutral-950/40",
                    parsedPort == null
                      ? "border-rose-300 text-rose-700 focus:border-rose-400 dark:border-rose-700 dark:text-rose-300"
                      : "border-slate-200 text-slate-700 focus:border-slate-300 dark:border-neutral-800 dark:text-neutral-100 dark:focus:border-neutral-700"
                  )}
                />
              </div>
              {canInstallCodeServer ? (
                <button
                  className={cn(
                    "inline-flex h-8 items-center rounded-xl px-3 text-xs font-semibold transition-colors",
                    installRunning
                      ? "cursor-wait bg-slate-200 text-slate-500 dark:bg-neutral-800 dark:text-neutral-500"
                      : "bg-amber-100 text-amber-900 hover:bg-amber-200 dark:bg-amber-900/40 dark:text-amber-300 dark:hover:bg-amber-900/60"
                  )}
                  onClick={() => void runInstallCodeServer()}
                  disabled={installRunning}
                >
                  {installRunning ? t("codeserver.installing") : t("codeserver.install")}
                </button>
              ) : null}

              {installedState ? (
                <>
                  <button
                    className="grid h-8 w-8 place-items-center rounded-xl bg-slate-100 text-slate-700 transition-colors hover:bg-slate-200 dark:bg-neutral-800 dark:text-neutral-100 dark:hover:bg-neutral-700"
                    onClick={() => setIframeKey((prev) => prev + 1)}
                    title={t("codeserver.reload")}
                  >
                    <FiRefreshCw />
                  </button>
                  <button
                    className="inline-flex h-8 items-center gap-1 rounded-xl bg-slate-100 px-3 text-xs font-semibold text-slate-700 transition-colors hover:bg-slate-200 dark:bg-neutral-800 dark:text-neutral-100 dark:hover:bg-neutral-700"
                    onClick={() => void togglePanelFullscreen()}
                  >
                    {panelFullscreen ? <FiMinimize2 /> : <FiMaximize2 />}
                    {panelFullscreen ? t("codeserver.exit_fullscreen") : t("codeserver.fullscreen")}
                  </button>
                  <button
                    className="inline-flex h-8 items-center gap-1 rounded-xl bg-slate-900 px-3 text-xs font-semibold text-white transition-colors hover:bg-slate-800 dark:bg-neutral-50 dark:text-neutral-900 dark:hover:bg-white"
                    onClick={openInNewTab}
                  >
                    <FiExternalLink />
                    {t("codeserver.open")}
                  </button>
                </>
              ) : null}
            </div>
          </div>

          {parsedPort == null ? (
            <div className="mt-2 text-xs text-rose-600 dark:text-rose-300">{t("codeserver.invalid_port")}</div>
          ) : null}

          {!installedState ? (
            <div className="mt-4 rounded-2xl border border-dashed border-slate-200 p-6 text-sm text-slate-600 dark:border-neutral-800 dark:text-neutral-300">
              <p>{installSupportedHost ? t("codeserver.not_installed") : t("codeserver.install_unsupported")}</p>
            </div>
          ) : (
            <>
              <div className="mt-3 truncate font-mono text-[11px] text-slate-500 dark:text-neutral-400" title={panelUrl}>
                {t("codeserver.panel_url")}: {panelUrl}
              </div>
              <div
                ref={panelRef}
                className="mt-3 min-h-0 flex-1 overflow-hidden rounded-2xl border border-slate-200 bg-white dark:border-neutral-800 dark:bg-neutral-950/30"
              >
                <iframe
                  key={iframeKey}
                  src={panelUrl}
                  title="code-server"
                  className="h-full w-full border-0"
                  loading="lazy"
                />
              </div>
            </>
          )}
        </section>
      </div>

      {installDialogOpen ? (
        <div
          className="fixed inset-0 z-[130] flex items-center justify-center bg-slate-900/45 p-4 backdrop-blur-[2px]"
          onClick={() => {
            if (!installRunning) {
              setInstallDialogOpen(false);
            }
          }}
        >
          <section
            className="flex max-h-[92dvh] w-full max-w-3xl min-w-0 flex-col rounded-3xl bg-white/95 p-4 shadow-2xl ring-1 ring-slate-200/80 dark:bg-neutral-900/95 dark:ring-neutral-700"
            onClick={(event) => event.stopPropagation()}
          >
            <div className="flex items-center justify-between gap-3">
              <h3 className="inline-flex items-center gap-2 text-sm font-semibold tracking-tight text-slate-900 dark:text-neutral-50">
                <FiCode className="text-[14px]" />
                {t("codeserver.install_dialog_title")}
              </h3>
              <div className="flex items-center gap-2">
                <span className="rounded-full bg-slate-100 px-2 py-0.5 text-[11px] font-semibold text-slate-600 dark:bg-neutral-800 dark:text-neutral-300">
                  {installStatus || "queued"}
                </span>
                <button
                  type="button"
                  className={cn(
                    "inline-flex h-9 items-center gap-1.5 rounded-xl px-3 text-xs font-semibold transition-colors",
                    installRunning
                      ? "cursor-not-allowed bg-slate-200 text-slate-500 dark:bg-neutral-800 dark:text-neutral-500"
                      : "bg-slate-100 text-slate-700 hover:bg-slate-200 dark:bg-neutral-800 dark:text-neutral-100 dark:hover:bg-neutral-700"
                  )}
                  disabled={installRunning}
                  onClick={() => setInstallDialogOpen(false)}
                >
                  <FiX />
                  {t("common.close")}
                </button>
              </div>
            </div>

            <pre
              ref={installViewportRef}
              className="mt-4 min-h-0 flex-1 overflow-auto whitespace-pre-wrap break-all rounded-2xl bg-neutral-950 p-3 font-mono text-[12px] leading-relaxed text-neutral-50"
            >
              {installLogs || t("codeserver.install_log_empty")}
            </pre>
          </section>
        </div>
      ) : null}
    </>
  );
}
