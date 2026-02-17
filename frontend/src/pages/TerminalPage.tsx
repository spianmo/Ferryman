import { useEffect, useRef, useState } from "react";
import { FiTerminal, FiPower, FiLink2 } from "react-icons/fi";
import { Terminal } from "xterm";
import { FitAddon } from "xterm-addon-fit";
import "xterm/css/xterm.css";

import type { SessionInfo } from "../types";
import { emitUnauthorized, wsUrl } from "../api/client";
import { useI18n } from "../i18n";
import { useTheme } from "../theme";
import { decodeBase64Utf8, encodeBase64Utf8 } from "../util/codec";

type Props = {
  session: SessionInfo;
};

export default function TerminalPage({ session }: Props) {
  const { t } = useI18n();
  const { theme } = useTheme();

  const hostRef = useRef<HTMLDivElement | null>(null);
  const termRef = useRef<Terminal | null>(null);
  const fitRef = useRef<FitAddon | null>(null);
  const wsRef = useRef<WebSocket | null>(null);
  const [status, setStatus] = useState<{ kind: "disconnected" | "connecting" | "connected" | "closed" | "failed" | "error"; message?: string }>({ kind: "disconnected" });
  const [terminalId, setTerminalId] = useState("");

  useEffect(() => {
    if (!hostRef.current) return;

    const darkTheme = {
      background: "#0b1220",
      foreground: "#e5e7eb",
      cursor: "#60a5fa",
    };
    const lightTheme = {
      background: "#ffffff",
      foreground: "#0b1220",
      cursor: "#1d4ed8",
    };

    const terminal = new Terminal({
      convertEol: true,
      cursorBlink: true,
      fontFamily: "IBM Plex Mono, ui-monospace, SFMono-Regular, Menlo, monospace",
      fontSize: 13,
      theme: theme === "dark" ? darkTheme : lightTheme,
    });
    const fitAddon = new FitAddon();
    terminal.loadAddon(fitAddon);
    terminal.open(hostRef.current);
    fitAddon.fit();

    const disposable = terminal.onData((data) => {
      const ws = wsRef.current;
      if (!ws || ws.readyState !== WebSocket.OPEN) return;
      ws.send(JSON.stringify({ action: "input", data: encodeBase64Utf8(data) }));
    });

    termRef.current = terminal;
    fitRef.current = fitAddon;

    const onResize = () => {
      fitAddon.fit();
      const ws = wsRef.current;
      if (ws && ws.readyState === WebSocket.OPEN) {
        ws.send(JSON.stringify({ action: "resize", cols: terminal.cols, rows: terminal.rows }));
      }
    };
    window.addEventListener("resize", onResize);

    return () => {
      window.removeEventListener("resize", onResize);
      disposable.dispose();
      terminal.dispose();
      termRef.current = null;
      fitRef.current = null;
    };
  }, []);

  useEffect(() => {
    const terminal = termRef.current;
    if (!terminal) return;
    terminal.setOption(
      "theme",
      theme === "dark"
        ? { background: "#0b1220", foreground: "#e5e7eb", cursor: "#60a5fa" }
        : { background: "#ffffff", foreground: "#0b1220", cursor: "#1d4ed8" }
    );
  }, [theme]);

  useEffect(() => {
    return () => {
      wsRef.current?.close();
      wsRef.current = null;
    };
  }, []);

  const connect = () => {
    if (wsRef.current && wsRef.current.readyState === WebSocket.OPEN) {
      return;
    }

    const ws = new WebSocket(wsUrl(session, "/ws/terminal"));
    wsRef.current = ws;
    setStatus({ kind: "connecting" });

    ws.onopen = () => {
      setStatus({ kind: "connected" });
    };

    ws.onmessage = (event) => {
      let payload: Record<string, unknown>;
      try {
        payload = JSON.parse(event.data as string) as Record<string, unknown>;
      } catch {
        return;
      }

      if (payload.ok === false) {
        if (String(payload.code ?? "") === "unauthorized") {
          const reason = payload.error;
          emitUnauthorized({ reason: typeof reason === "string" ? reason : undefined });
        }
        setStatus({ kind: "error", message: String(payload.error ?? "WS error") });
        return;
      }

      const eventType = String(payload.event ?? "");
      if (eventType === "connected") {
        const cols = termRef.current?.cols ?? 120;
        const rows = termRef.current?.rows ?? 30;
        ws.send(JSON.stringify({ action: "open", cols, rows }));
      } else if (eventType === "terminal_open") {
        const id = String(payload.terminal_id ?? "");
        setTerminalId(id);
        termRef.current?.writeln(`\r\n[session: ${id}]`);
      } else if (eventType === "terminal_output") {
        const data = String(payload.data ?? "");
        termRef.current?.write(decodeBase64Utf8(data));
      } else if (eventType === "terminal_closed") {
        termRef.current?.writeln("\r\n[terminal closed]");
        setTerminalId("");
      }
    };

    ws.onclose = () => {
      setStatus({ kind: "closed" });
    };

    ws.onerror = () => {
      setStatus({ kind: "failed" });
    };
  };

  const close = () => {
    const ws = wsRef.current;
    if (!ws || ws.readyState !== WebSocket.OPEN) return;
    ws.send(JSON.stringify({ action: "close" }));
    ws.close();
    wsRef.current = null;
    setStatus({ kind: "disconnected" });
  };

  const statusText =
    status.kind === "error" && status.message ? status.message : t(`terminal.${status.kind}`);

  return (
    <section className="rounded-3xl bg-white/70 p-4 shadow-soft ring-1 ring-slate-200/70 backdrop-blur dark:bg-slate-900/55 dark:ring-slate-800/70">
      <div className="flex items-start justify-between gap-3">
        <h2 className="inline-flex items-center gap-2 text-base font-semibold tracking-tight text-slate-900 dark:text-slate-50">
          <FiTerminal /> {t("terminal.title")}
        </h2>
        <div className="flex flex-wrap items-center gap-2">
          <button
            className="inline-flex h-10 items-center gap-2 rounded-2xl bg-slate-900 px-4 text-sm font-semibold text-white shadow-sm hover:bg-slate-800 dark:bg-slate-50 dark:text-slate-900 dark:hover:bg-white"
            onClick={connect}
          >
            <FiLink2 /> {t("terminal.connect")}
          </button>
          <button
            className="inline-flex h-10 items-center gap-2 rounded-2xl bg-slate-100 px-4 text-sm font-semibold text-slate-700 hover:bg-slate-200 dark:bg-slate-800 dark:text-slate-100 dark:hover:bg-slate-700"
            onClick={close}
          >
            <FiPower /> {t("terminal.close")}
          </button>
        </div>
      </div>

      <div className="mt-3 flex flex-wrap items-center gap-2">
        <span className="inline-flex items-center gap-2 rounded-full bg-white/70 px-3 py-1 text-xs font-semibold text-slate-700 shadow-sm ring-1 ring-slate-200/60 dark:bg-slate-950/30 dark:text-slate-100 dark:ring-slate-800/70">
          {t("terminal.status")}: {statusText}
        </span>
        {terminalId ? (
          <span className="inline-flex items-center gap-2 rounded-full bg-white/70 px-3 py-1 font-mono text-[11px] text-slate-600 shadow-sm ring-1 ring-slate-200/60 dark:bg-slate-950/30 dark:text-slate-200 dark:ring-slate-800/70">
            {t("terminal.id")}: {terminalId}
          </span>
        ) : null}
      </div>

      <div
        ref={hostRef}
        className="mt-4 h-[560px] overflow-hidden rounded-2xl bg-white shadow-sm ring-1 ring-slate-200/70 dark:bg-slate-950/40 dark:ring-slate-800/70"
      />
    </section>
  );
}
