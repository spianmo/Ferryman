import { useEffect, useRef, useState } from "react";

import { useI18n } from "../i18n";
import type { SessionInfo } from "../types";
import { cn } from "../util/cn";
import { getLogSocket, type LogSocketStatus } from "../ws/logSocket";

type Props = {
  session: SessionInfo;
};

type LogLevel = "info" | "warn" | "error" | "debug" | "other";

type LogLine = {
  id: string;
  level: LogLevel;
  text: string;
};

const FETCH_LINES = 400;
const MAX_BUFFER_LINES = 2000;

function asString(value: unknown) {
  if (typeof value === "string") return value;
  if (value == null) return "";
  try {
    return JSON.stringify(value);
  } catch {
    return String(value);
  }
}

function normalizeLevel(raw: string): LogLevel {
  const value = raw.toLowerCase();
  if (value === "info") return "info";
  if (value === "warn" || value === "warning") return "warn";
  if (value === "error") return "error";
  if (value === "debug" || value === "trace") return "debug";
  return "other";
}

function formatTime(ts: string) {
  if (!ts) return "--:--:--";
  const date = new Date(ts);
  if (Number.isNaN(date.getTime())) return ts;
  return date.toLocaleTimeString([], { hour12: false });
}

function shortSession(session: string) {
  if (!session) return "-";
  return session.slice(0, 10);
}

function toLogLine(item: Record<string, unknown>): LogLine {
  const ts = asString(item.ts);
  const levelRaw = asString(item.level) || "info";
  const level = normalizeLevel(levelRaw);
  const action = asString(item.action);
  const detail = asString(item.detail);
  const session = asString(item.session);
  const id = [ts, levelRaw, session, action, detail].join("|");

  const text = `[${formatTime(ts)}] ${levelRaw.toUpperCase().padEnd(5)} [${shortSession(session)}] ${action}${detail ? ` ${detail}` : ""}`;
  return { id, level, text };
}

function parseLogItems(raw: unknown): Array<Record<string, unknown>> {
  if (Array.isArray(raw)) {
    return raw.filter((item): item is Record<string, unknown> => !!item && typeof item === "object");
  }
  if (typeof raw === "string") {
    try {
      const parsed = JSON.parse(raw) as unknown;
      if (Array.isArray(parsed)) {
        return parsed.filter((item): item is Record<string, unknown> => !!item && typeof item === "object");
      }
    } catch {
      return [];
    }
  }
  return [];
}

function parseLogItem(raw: unknown): Record<string, unknown> | null {
  if (raw && typeof raw === "object" && !Array.isArray(raw)) {
    return raw as Record<string, unknown>;
  }
  if (typeof raw === "string") {
    try {
      const parsed = JSON.parse(raw) as unknown;
      if (parsed && typeof parsed === "object" && !Array.isArray(parsed)) {
        return parsed as Record<string, unknown>;
      }
    } catch {
      return null;
    }
  }
  return null;
}

export default function LogsPage({ session }: Props) {
  const { t } = useI18n();
  const [lines, setLines] = useState<LogLine[]>([]);
  const [status, setStatus] = useState<LogSocketStatus>({ kind: "disconnected" });

  const seenRef = useRef<Set<string>>(new Set());
  const viewportRef = useRef<HTMLDivElement | null>(null);

  const replaceLines = (next: LogLine[]) => {
    const deduped: LogLine[] = [];
    const seen = new Set<string>();
    for (const line of next) {
      if (seen.has(line.id)) continue;
      seen.add(line.id);
      deduped.push(line);
    }
    const trimmed = deduped.slice(Math.max(0, deduped.length - MAX_BUFFER_LINES));
    seenRef.current = new Set(trimmed.map((line) => line.id));
    setLines(trimmed);
  };

  const appendLines = (incoming: LogLine[]) => {
    if (incoming.length === 0) return;
    setLines((prev) => {
      const next = [...prev];
      for (const line of incoming) {
        if (seenRef.current.has(line.id)) continue;
        seenRef.current.add(line.id);
        next.push(line);
      }
      if (next.length <= MAX_BUFFER_LINES) {
        return next;
      }
      const trimmed = next.slice(next.length - MAX_BUFFER_LINES);
      seenRef.current = new Set(trimmed.map((line) => line.id));
      return trimmed;
    });
  };

  useEffect(() => {
    const socket = getLogSocket();
    seenRef.current = new Set();
    setLines([]);

    const unsubscribeStatus = socket.subscribeStatus((next) => {
      setStatus(next);
    });
    const unsubscribeMessages = socket.subscribeMessages((payload) => {
      const eventType = String(payload.event ?? "");
      if (eventType === "logs_snapshot") {
        const snapshot = parseLogItems(payload.items).map(toLogLine);
        replaceLines(snapshot);
        return;
      }
      if (eventType === "log_entry") {
        const item = parseLogItem(payload.item);
        if (!item) return;
        appendLines([toLogLine(item)]);
      }
    });

    socket.start(session);
    socket.requestTail(FETCH_LINES);

    return () => {
      unsubscribeMessages();
      unsubscribeStatus();
      socket.stop();
      setStatus({ kind: "disconnected" });
    };
  }, [session]);

  useEffect(() => {
    const viewport = viewportRef.current;
    if (!viewport) return;
    viewport.scrollTop = viewport.scrollHeight;
  }, [lines]);

  const statusLabel =
    status.kind === "error" && status.message ? status.message : t(`terminal.${status.kind}`);
  const dotKind =
    status.kind === "connected"
      ? "active"
      : status.kind === "connecting"
        ? "connecting"
        : status.kind === "error" || status.kind === "failed"
          ? "error"
          : "idle";

  return (
    <section className="flex h-full max-h-full min-h-0 flex-col overflow-hidden rounded-3xl bg-white/70 p-4 shadow-soft ring-1 ring-slate-200/70 backdrop-blur dark:bg-slate-900/55 dark:ring-slate-800/70">
      <div className="flex flex-wrap items-start justify-between gap-3">
        <div className="inline-flex items-center gap-2">
          <h2 className="inline-flex items-center gap-2 text-base font-semibold tracking-tight text-slate-900 dark:text-slate-50">
            {t("logs.title")}
          </h2>
          <span className="inline-flex h-2.5 w-2.5 shrink-0 items-center justify-center">
            <span
              className={cn(
                "block h-2.5 w-2.5 rounded-full border border-white/90 transition-all duration-300 ease-out dark:border-slate-900/90",
                dotKind === "active" && "scale-100 bg-emerald-500 shadow-[0_0_0_3px_rgba(16,185,129,0.20)]",
                dotKind === "connecting" && "animate-pulse scale-95 bg-amber-400",
                dotKind === "error" && "scale-100 bg-rose-500 shadow-[0_0_0_3px_rgba(244,63,94,0.16)]",
                dotKind === "idle" && "scale-90 bg-slate-400 dark:bg-slate-500"
              )}
              title={statusLabel}
              aria-label={`${t("terminal.status")}: ${statusLabel}`}
            />
          </span>
          <span className="inline-flex items-center rounded-full bg-white/70 px-2 py-0.5 font-mono text-[11px] text-slate-600 shadow-sm ring-1 ring-slate-200/60 dark:bg-slate-950/30 dark:text-slate-200 dark:ring-slate-800/70">
            {lines.length}
          </span>
        </div>
      </div>

      <div
        ref={viewportRef}
        className="mt-4 min-h-0 flex-1 overflow-auto rounded-2xl bg-slate-950 p-3 font-mono text-[12px] leading-relaxed shadow-inner ring-1 ring-slate-800/80"
      >
        {lines.length === 0 ? (
          <div className="grid h-full w-full place-items-center text-sm text-slate-300">
            {t("logs.empty")}
          </div>
        ) : (
          lines.map((line, idx) => (
            <div
              key={`${line.id}-${idx}`}
              className={cn(
                "whitespace-pre-wrap break-words",
                line.level === "error" && "text-rose-300",
                line.level === "warn" && "text-amber-300",
                line.level === "debug" && "text-sky-300",
                (line.level === "info" || line.level === "other") && "text-emerald-200"
              )}
            >
              {line.text}
            </div>
          ))
        )}
      </div>
    </section>
  );
}
