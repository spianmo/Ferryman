import { useCallback, useEffect, useRef, useState } from "react";
import { FiTerminal, FiPower, FiLink2 } from "react-icons/fi";
import { Terminal } from "xterm";
import { FitAddon } from "xterm-addon-fit";
import "xterm/css/xterm.css";

import type { SessionInfo } from "../types";
import { useI18n } from "../i18n";
import { useTheme } from "../theme";
import { decodeBase64Utf8, encodeBase64Utf8 } from "../util/codec";
import { getTerminalSocket, type TerminalSocketStatus } from "../ws/terminalSocket";

type Props = {
  session: SessionInfo;
};

function terminalTheme(mode: "light" | "dark") {
  return mode === "dark"
    ? { background: "#0a0a0a", foreground: "#e5e7eb", cursor: "#f5f5f5" }
    : { background: "#ffffff", foreground: "#111111", cursor: "#1f2937" };
}

export default function TerminalPage({ session }: Props) {
  const { t } = useI18n();
  const { theme } = useTheme();
  const themeRef = useRef(theme);

  const hostRef = useRef<HTMLDivElement | null>(null);
  const termRef = useRef<Terminal | null>(null);
  const fitRef = useRef<FitAddon | null>(null);
  const [status, setStatus] = useState<TerminalSocketStatus>({ kind: "disconnected" });
  const [terminalId, setTerminalId] = useState("");

  useEffect(() => {
    if (!hostRef.current) return;

    let disposed = false;
    let initialFitTimer: number | null = null;
    let onResize: (() => void) | null = null;
    let dataDisposable: { dispose: () => void } | null = null;

    const initTimer = window.setTimeout(() => {
      if (disposed) return;
      const host = hostRef.current;
      if (!host) return;

      const terminal = new Terminal({
        convertEol: true,
        cursorBlink: true,
        fontFamily: "IBM Plex Mono, ui-monospace, SFMono-Regular, Menlo, monospace",
        fontSize: 13,
        theme: terminalTheme(themeRef.current),
      });
      const fitAddon = new FitAddon();
      terminal.loadAddon(fitAddon);
      terminal.open(host);

      const safeFit = () => {
        if (disposed) return;
        try {
          fitAddon.fit();
        } catch {
          // xterm can race with React StrictMode throwaway mounts in dev.
        }
      };
      const syncResize = () => {
        getTerminalSocket().send({ action: "resize", cols: terminal.cols, rows: terminal.rows });
      };
      initialFitTimer = window.setTimeout(() => {
        // Delay initial fit so React StrictMode's throwaway mount can cleanly unmount.
        safeFit();
        syncResize();
      }, 0);

      dataDisposable = terminal.onData((data) => {
        getTerminalSocket().send({ action: "input", data: encodeBase64Utf8(data) });
      });

      termRef.current = terminal;
      fitRef.current = fitAddon;

      onResize = () => {
        safeFit();
        syncResize();
      };
      window.addEventListener("resize", onResize);
    }, 0);

    return () => {
      disposed = true;
      window.clearTimeout(initTimer);
      if (initialFitTimer !== null) {
        window.clearTimeout(initialFitTimer);
      }
      if (onResize) {
        window.removeEventListener("resize", onResize);
      }
      dataDisposable?.dispose();

      const terminal = termRef.current;
      termRef.current = null;
      fitRef.current = null;
      if (terminal) {
        // xterm internally schedules viewport sync with setTimeout while opening.
        // Dispose one tick later to avoid reading renderer dimensions after disposal.
        window.setTimeout(() => terminal.dispose(), 0);
      }
    };
  }, []);

  useEffect(() => {
    themeRef.current = theme;
    const terminal = termRef.current;
    if (!terminal) return;
    terminal.options.theme = terminalTheme(theme);
  }, [theme]);

  useEffect(() => {
    const socket = getTerminalSocket();
    const unsubscribeStatus = socket.subscribeStatus((next) => {
      setStatus(next);
    });

    const unsubscribeMessages = socket.subscribeMessages((payload) => {
      const eventType = String(payload.event ?? "");
      if (eventType === "connected") {
        const cols = termRef.current?.cols ?? 120;
        const rows = termRef.current?.rows ?? 30;
        socket.send({ action: "open", cols, rows });
      } else if (eventType === "terminal_open") {
        const id = String(payload.terminal_id ?? "");
        setTerminalId(id);
      } else if (eventType === "terminal_output") {
        const data = String(payload.data ?? "");
        termRef.current?.write(decodeBase64Utf8(data));
      } else if (eventType === "terminal_closed") {
        termRef.current?.writeln("\r\n[terminal closed]");
        setTerminalId("");
      }
    });

    socket.connect(session);
    return () => {
      unsubscribeMessages();
      unsubscribeStatus();
      socket.disconnect();
      setTerminalId("");
    };
  }, [session]);

  const connect = useCallback(() => {
    getTerminalSocket().connect(session);
  }, [session]);

  const close = () => {
    const socket = getTerminalSocket();
    socket.send({ action: "close" });
    socket.disconnect();
    setTerminalId("");
  };

  const statusText =
    status.kind === "error" && status.message ? status.message : t(`terminal.${status.kind}`);

  return (
    <section className="flex h-full min-h-[460px] flex-col overflow-y-auto rounded-3xl bg-white/70 p-4 shadow-soft ring-1 ring-slate-200/70 backdrop-blur dark:bg-neutral-900/55 dark:ring-neutral-800/70">
      <div className="flex items-start justify-between gap-3">
        <h2 className="inline-flex items-center gap-2 text-base font-semibold tracking-tight text-slate-900 dark:text-neutral-50">
          <FiTerminal /> {t("terminal.title")}
        </h2>
        <div className="flex flex-wrap items-center gap-2">
          <button
            className="inline-flex h-10 items-center gap-2 rounded-2xl bg-slate-900 px-4 text-sm font-semibold text-white shadow-sm hover:bg-slate-800 dark:bg-neutral-50 dark:text-neutral-900 dark:hover:bg-white"
            onClick={connect}
          >
            <FiLink2 /> {t("terminal.connect")}
          </button>
          <button
            className="inline-flex h-10 items-center gap-2 rounded-2xl bg-slate-100 px-4 text-sm font-semibold text-slate-700 hover:bg-slate-200 dark:bg-neutral-800 dark:text-neutral-100 dark:hover:bg-neutral-700"
            onClick={close}
          >
            <FiPower /> {t("terminal.close")}
          </button>
        </div>
      </div>

      <div className="mt-3 flex flex-wrap items-center gap-2">
        <span className="inline-flex items-center gap-2 rounded-full bg-white/70 px-3 py-1 text-xs font-semibold text-slate-700 shadow-sm ring-1 ring-slate-200/60 dark:bg-neutral-950/30 dark:text-neutral-100 dark:ring-neutral-800/70">
          {t("terminal.status")}: {statusText}
        </span>
        {terminalId ? (
          <span className="inline-flex items-center gap-2 rounded-full bg-white/70 px-3 py-1 font-mono text-[11px] text-slate-600 shadow-sm ring-1 ring-slate-200/60 dark:bg-neutral-950/30 dark:text-neutral-200 dark:ring-neutral-800/70">
            {t("terminal.id")}: {terminalId}
          </span>
        ) : null}
      </div>

      <div
        ref={hostRef}
        className="mt-4 min-h-0 flex-1 overflow-hidden rounded-2xl bg-white shadow-sm ring-1 ring-slate-200/70 dark:bg-neutral-950/40 dark:ring-neutral-800/70"
      />
    </section>
  );
}
