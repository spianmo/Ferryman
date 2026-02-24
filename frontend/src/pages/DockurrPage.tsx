import { useEffect, useMemo, useRef, useState, type FormEvent } from "react";
import { toast } from "../toast";
import {
  FiActivity,
  FiEye,
  FiHardDrive,
  FiInfo,
  FiPlayCircle,
  FiRefreshCw,
  FiRotateCw,
  FiServer,
  FiSquare,
} from "react-icons/fi";

import { useI18n } from "../i18n";
import type { DockurrVmInfo, SessionInfo } from "../types";
import { cn } from "../util/cn";
import { getDockurrSocket, type DockurrSocketStatus } from "../ws/dockurrSocket";

type Props = {
  session: SessionInfo;
};

type DetailMode = "logs" | "inspect";

type VersionOption = {
  value: string;
  label: string;
};

const WINDOWS_VERSIONS: VersionOption[] = [
  { value: "11", label: "Win 11 Pro" },
  { value: "11l", label: "Win 11 LTSC" },
  { value: "11e", label: "Win 11 Ent" },
  { value: "10", label: "Win 10 Pro" },
  { value: "10l", label: "Win 10 LTSC" },
  { value: "10e", label: "Win 10 Ent" },
  { value: "8e", label: "Win 8.1 Ent" },
  { value: "7u", label: "Win 7 Ult" },
  { value: "vu", label: "Vista Ult" },
  { value: "xp", label: "XP Pro" },
  { value: "2k", label: "2000 Pro" },
  { value: "2025", label: "Server 2025" },
  { value: "2022", label: "Server 2022" },
  { value: "2019", label: "Server 2019" },
  { value: "2016", label: "Server 2016" },
  { value: "2012", label: "Server 2012" },
  { value: "2008", label: "Server 2008" },
  { value: "2003", label: "Server 2003" },
];

const MACOS_VERSIONS: VersionOption[] = [
  { value: "15", label: "macOS Sequoia" },
  { value: "14", label: "macOS Sonoma" },
  { value: "13", label: "macOS Ventura" },
  { value: "12", label: "macOS Monterey" },
  { value: "11", label: "macOS Big Sur" },
];

const MAX_STARTUP_LINES = 800;

const actionButtonClass =
  "inline-flex h-8 items-center gap-1 rounded-xl px-2.5 text-xs font-semibold transition-colors";

type RuntimeLogLevel = "info" | "warn" | "error" | "debug" | "other";

type RuntimeLogEntry = {
  id: string;
  level: RuntimeLogLevel;
  text: string;
};

function asString(value: unknown) {
  if (typeof value === "string") return value;
  if (value == null) return "";
  try {
    return JSON.stringify(value);
  } catch {
    return String(value);
  }
}

function asBool(value: unknown) {
  if (typeof value === "boolean") return value;
  if (typeof value === "number") return value !== 0;
  if (typeof value === "string") {
    const lower = value.toLowerCase();
    return lower === "1" || lower === "true" || lower === "yes" || lower === "on";
  }
  return false;
}

function normalizeLogLevel(raw: string): RuntimeLogLevel {
  const lower = raw.toLowerCase();
  if (lower === "info") return "info";
  if (lower === "warn" || lower === "warning") return "warn";
  if (lower === "error") return "error";
  if (lower === "debug" || lower === "trace") return "debug";
  return "other";
}

function formatLogTime(ts: string) {
  if (!ts) return "--:--:--";
  const date = new Date(ts);
  if (Number.isNaN(date.getTime())) return ts;
  return date.toLocaleTimeString([], { hour12: false });
}

function buildRuntimeLogEntry(levelRaw: string, action: string, message: string, ts = ""): RuntimeLogEntry {
  const level = normalizeLogLevel(levelRaw);
  const normalizedAction = action || "dockurr";
  const normalizedMessage = message || "-";
  const timePart = formatLogTime(ts);
  const text = `[${timePart}] ${levelRaw.toUpperCase().padEnd(5)} [${normalizedAction}] ${normalizedMessage}`;
  return {
    id: `${ts}|${levelRaw}|${normalizedAction}|${normalizedMessage}|${Math.random().toString(36).slice(2, 8)}`,
    level,
    text,
  };
}

function parseVm(raw: unknown): DockurrVmInfo | null {
  if (!raw || typeof raw !== "object" || Array.isArray(raw)) return null;
  const source = raw as Record<string, unknown>;
  const name = asString(source.name).trim();
  if (!name) return null;
  return {
    id: asString(source.id),
    name,
    os: asString(source.os),
    image: asString(source.image),
    ports: asString(source.ports),
    running_for: asString(source.running_for),
    persistent: asBool(source.persistent),
    novnc_port: asString(source.novnc_port),
    desktop_port: asString(source.desktop_port),
  };
}

function parseVms(raw: unknown): DockurrVmInfo[] {
  if (!Array.isArray(raw)) {
    return [];
  }
  const parsed: DockurrVmInfo[] = [];
  for (const item of raw) {
    const vm = parseVm(item);
    if (vm) {
      parsed.push(vm);
    }
  }
  return parsed;
}

function osLabel(vm: DockurrVmInfo) {
  if (vm.os === "windows") return "Windows";
  if (vm.os === "macos") return "macOS";
  return vm.os || "unknown";
}

function makeRequestId(action: string) {
  return `${action}-${Date.now()}-${Math.random().toString(36).slice(2, 8)}`;
}

export default function DockurrPage({ session }: Props) {
  const { t } = useI18n();
  const socket = useMemo(() => getDockurrSocket(), []);

  const [vms, setVms] = useState<DockurrVmInfo[]>([]);
  const [selectedName, setSelectedName] = useState("");
  const [listError, setListError] = useState("");
  const [status, setStatus] = useState<DockurrSocketStatus>({ kind: "disconnected" });

  const [os, setOs] = useState<"windows" | "macos">("windows");
  const [version, setVersion] = useState(WINDOWS_VERSIONS[0]?.value ?? "11");
  const [ram, setRam] = useState("4G");
  const [name, setName] = useState("");
  const [persist, setPersist] = useState(false);
  const [createLoading, setCreateLoading] = useState(false);

  const [detailLoading, setDetailLoading] = useState(false);
  const [detailMode, setDetailMode] = useState<DetailMode | null>(null);
  const [detailText, setDetailText] = useState("");

  const [runtimeLogs, setRuntimeLogs] = useState<RuntimeLogEntry[]>([]);
  const startupViewportRef = useRef<HTMLDivElement | null>(null);
  const currentCreateRequestIdRef = useRef("");
  const pendingCreateNameRef = useRef("");

  const versionOptions = useMemo(() => (os === "windows" ? WINDOWS_VERSIONS : MACOS_VERSIONS), [os]);
  const selectedVm = useMemo(() => vms.find((vm) => vm.name === selectedName) ?? null, [selectedName, vms]);

  useEffect(() => {
    if (versionOptions.some((option) => option.value === version)) {
      return;
    }
    setVersion(versionOptions[0]?.value ?? "");
  }, [version, versionOptions]);

  useEffect(() => {
    const viewport = startupViewportRef.current;
    if (!viewport) return;
    viewport.scrollTop = viewport.scrollHeight;
  }, [runtimeLogs]);

  const appendRuntimeLog = (levelRaw: string, action: string, message: string, ts = "") => {
    setRuntimeLogs((prev) => {
      const next = [...prev, buildRuntimeLogEntry(levelRaw, action, message, ts)];
      if (next.length <= MAX_STARTUP_LINES) {
        return next;
      }
      return next.slice(next.length - MAX_STARTUP_LINES);
    });
  };

  const sendAction = (action: string, extra: Record<string, unknown>) => {
    const requestId = makeRequestId(action);
    const sent = socket.send({ action, request_id: requestId, ...extra });
    if (!sent) {
      toast.error(t("dockurr.ws_unavailable"));
      return "";
    }
    return requestId;
  };

  useEffect(() => {
    setVms([]);
    setSelectedName("");
    setListError("");
    setDetailText("");
    setDetailMode(null);
    setRuntimeLogs([]);
    setCreateLoading(false);
    currentCreateRequestIdRef.current = "";

    const unsubscribeStatus = socket.subscribeStatus((next) => {
      setStatus(next);
    });

    const unsubscribeMessages = socket.subscribeMessages((payload) => {
      const event = asString(payload.event);
      if (event === "dockurr_snapshot") {
        const next = parseVms(payload.vms);
        setListError("");
        setVms(next);
        setSelectedName((prev) => {
          if (prev && next.some((vm) => vm.name === prev)) {
            return prev;
          }
          return next[0]?.name ?? "";
        });
        return;
      }

      if (event === "dockurr_error") {
        const error = asString(payload.error) || t("toast.request_failed");
        setListError(error);
        appendRuntimeLog("error", "list", error, asString(payload.ts));
        return;
      }

      if (event === "dockurr_runtime_log") {
        appendRuntimeLog(
          asString(payload.level) || "info",
          asString(payload.action) || "dockurr",
          asString(payload.message) || asString(payload.detail),
          asString(payload.ts)
        );
        return;
      }

      if (event === "dockurr_startup_log") {
        const requestId = asString(payload.request_id);
        if (!requestId || requestId !== currentCreateRequestIdRef.current) {
          return;
        }
        const line = asString(payload.line);
        if (!line) return;
        appendRuntimeLog("info", "create.startup", line, asString(payload.ts));
        return;
      }

      if (event === "dockurr_startup_done") {
        const requestId = asString(payload.request_id);
        if (requestId && requestId === currentCreateRequestIdRef.current) {
          setCreateLoading(false);
          appendRuntimeLog(
            asBool(payload.success) ? "info" : "error",
            "create",
            asBool(payload.success) ? "startup log stream finished" : "startup failed",
            asString(payload.ts)
          );
        }
        return;
      }

      if (event !== "dockurr_action_result") {
        return;
      }

      const action = asString(payload.action);
      const success = asBool(payload.success);
      const accepted = asBool(payload.accepted);
      const error = asString(payload.error) || t("toast.request_failed");

      if (action === "create" && accepted) {
        return;
      }

      if (!success) {
        if (action === "create") {
          setCreateLoading(false);
        }
        if (action === "logs" || action === "inspect") {
          setDetailLoading(false);
        }
        appendRuntimeLog("error", action || "dockurr", error, asString(payload.ts));
        toast.error(error);
        return;
      }

      if (action === "create") {
        setCreateLoading(false);
        const vm = parseVm(payload.vm);
        const createdName = vm?.name || pendingCreateNameRef.current;
        toast.success(t("toast.dockurr_created", { name: createdName || t("dockurr.unnamed") }));
        setName("");
        pendingCreateNameRef.current = "";
        if (createdName) {
          setSelectedName(createdName);
        }
        return;
      }

      if (action === "stop") {
        toast.success(t("toast.dockurr_stopped", { name: asString(payload.name) }));
        return;
      }

      if (action === "restart") {
        toast.success(t("toast.dockurr_restarted", { name: asString(payload.name) }));
        return;
      }

      if (action === "logs") {
        setDetailLoading(false);
        setDetailMode("logs");
        setDetailText(asString(payload.logs));
        return;
      }

      if (action === "inspect") {
        setDetailLoading(false);
        setDetailMode("inspect");
        setDetailText(asString(payload.inspect));
      }
    });

    socket.start(session);
    return () => {
      unsubscribeMessages();
      unsubscribeStatus();
      socket.stop();
    };
  }, [session, socket]);

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

  const openVm = (vm: DockurrVmInfo) => {
    if (!vm.novnc_port) {
      toast.error(t("dockurr.open_unavailable"));
      return;
    }
    const protocol = window.location.protocol === "https:" ? "https:" : "http:";
    const host = window.location.hostname || "localhost";
    window.open(`${protocol}//${host}:${vm.novnc_port}/`, "_blank", "noopener,noreferrer");
  };

  const refreshVms = () => {
    const sent = sendAction("list", {});
    if (!sent) return;
  };

  const runStop = (vm: DockurrVmInfo) => {
    void sendAction("stop", { name: vm.name });
  };

  const runRestart = (vm: DockurrVmInfo) => {
    void sendAction("restart", { name: vm.name });
  };

  const loadDetail = (vm: DockurrVmInfo, mode: DetailMode) => {
    setSelectedName(vm.name);
    setDetailLoading(true);
    setDetailMode(mode);
    if (mode === "logs") {
      if (!sendAction("logs", { name: vm.name, tail: 120 })) {
        setDetailLoading(false);
      }
      return;
    }
    if (!sendAction("inspect", { name: vm.name })) {
      setDetailLoading(false);
    }
  };

  const submitCreate = (event: FormEvent<HTMLFormElement>) => {
    event.preventDefault();
    if (createLoading) {
      return;
    }

    setCreateLoading(true);
    pendingCreateNameRef.current = name.trim();
    appendRuntimeLog("info", "create", t("dockurr.startup_submitted"), new Date().toISOString());
    const requestId = sendAction("create", {
      os,
      version,
      ram,
      name: name.trim(),
      persist,
    });
    if (!requestId) {
      setCreateLoading(false);
      return;
    }
    currentCreateRequestIdRef.current = requestId;
  };

  return (
    <div className="grid min-h-0 grid-cols-1 gap-4 xl:h-full xl:min-h-[520px] xl:grid-cols-[1.45fr_1fr]">
      <div className="flex min-h-0 min-w-0 flex-col gap-4 xl:min-h-[520px]">
        <section className="flex min-h-[320px] flex-col rounded-3xl bg-white/70 p-4 shadow-soft ring-1 ring-slate-200/70 backdrop-blur dark:bg-neutral-900/55 dark:ring-neutral-800/70 xl:min-h-[360px] xl:flex-1">
          <div className="flex items-start justify-between gap-3">
            <div className="inline-flex items-center gap-2">
              <h2 className="inline-flex items-center gap-2 text-base font-semibold tracking-tight text-slate-900 dark:text-neutral-50">
                <FiServer className="text-[15px]" />
                {t("dockurr.title")}
              </h2>
              <span className="inline-flex h-2.5 w-2.5 shrink-0 items-center justify-center">
                <span
                  className={cn(
                    "block h-2.5 w-2.5 rounded-full border border-white/90 transition-all duration-300 ease-out dark:border-neutral-900/90",
                    dotKind === "active" && "scale-100 bg-emerald-500 shadow-[0_0_0_3px_rgba(16,185,129,0.20)]",
                    dotKind === "connecting" && "animate-pulse scale-95 bg-amber-400",
                    dotKind === "error" && "scale-100 bg-rose-500 shadow-[0_0_0_3px_rgba(244,63,94,0.16)]",
                    dotKind === "idle" && "scale-90 bg-slate-400 dark:bg-neutral-500"
                  )}
                  title={statusLabel}
                  aria-label={`${t("terminal.status")}: ${statusLabel}`}
                />
              </span>
            </div>

            <button
              className="inline-flex h-10 items-center gap-2 rounded-2xl bg-slate-100 px-4 text-sm font-semibold text-slate-700 hover:bg-slate-200 dark:bg-neutral-800 dark:text-neutral-100 dark:hover:bg-neutral-700"
              onClick={refreshVms}
            >
              <FiRefreshCw className={cn(status.kind === "connecting" && "animate-spin")} />
              {t("common.refresh")}
            </button>
          </div>

          {listError ? (
            <div className="mt-4 rounded-2xl border border-rose-200/70 bg-rose-50/80 px-3 py-2 text-sm text-rose-700 dark:border-rose-900/50 dark:bg-rose-950/25 dark:text-rose-300">
              {listError}
            </div>
          ) : null}

          <div className="mt-4 min-h-0 flex-1 space-y-2 overflow-auto p-1">
            {vms.map((vm) => {
              const selected = vm.name === selectedName;
              return (
                <article
                  key={vm.id || vm.name}
                  className={cn(
                    "rounded-2xl border border-transparent bg-white/75 p-3 shadow-sm ring-1 ring-slate-200/60 transition-colors dark:bg-neutral-950/30 dark:ring-neutral-800/70",
                    selected
                      ? "ring-2 ring-slate-900/70 dark:ring-neutral-50/70"
                      : "hover:bg-slate-50 dark:hover:bg-neutral-900/50"
                  )}
                  onClick={() => setSelectedName(vm.name)}
                >
                  <div className="flex flex-wrap items-start justify-between gap-2">
                    <div className="min-w-0">
                      <div className="truncate text-sm font-semibold text-slate-900 dark:text-neutral-50">
                        {vm.name}
                      </div>
                      <div className="mt-0.5 truncate text-xs text-slate-500 dark:text-neutral-400">
                        {vm.image}
                      </div>
                    </div>
                    <div className="inline-flex shrink-0 items-center gap-1 rounded-full bg-slate-100 px-2 py-0.5 text-[11px] font-semibold text-slate-700 dark:bg-neutral-800 dark:text-neutral-200">
                      <FiHardDrive className="text-[11px]" />
                      {vm.persistent ? t("dockurr.persistent_yes") : t("dockurr.persistent_no")}
                    </div>
                  </div>

                  <div className="mt-2 grid grid-cols-1 gap-1 text-xs text-slate-600 dark:text-neutral-300 sm:grid-cols-2">
                    <div className="truncate">
                      {t("dockurr.os")}: <span className="font-semibold">{osLabel(vm)}</span>
                    </div>
                    <div className="truncate">
                      {t("dockurr.running_for")}: <span className="font-semibold">{vm.running_for || "-"}</span>
                    </div>
                    <div className="truncate sm:col-span-2">
                      {t("dockurr.ports")}: <span className="font-mono text-[11px]">{vm.ports || "-"}</span>
                    </div>
                  </div>

                  <div className="mt-3 flex flex-wrap gap-1.5">
                    <button
                      className={cn(
                        actionButtonClass,
                        "bg-emerald-100 text-emerald-800 hover:bg-emerald-200 dark:bg-emerald-900/40 dark:text-emerald-300 dark:hover:bg-emerald-900/60"
                      )}
                      onClick={(event) => {
                        event.stopPropagation();
                        openVm(vm);
                      }}
                    >
                      <FiEye /> {t("dockurr.open")}
                    </button>
                    <button
                      className={cn(
                        actionButtonClass,
                        "bg-rose-100 text-rose-800 hover:bg-rose-200 dark:bg-rose-900/40 dark:text-rose-300 dark:hover:bg-rose-900/60"
                      )}
                      onClick={(event) => {
                        event.stopPropagation();
                        runStop(vm);
                      }}
                    >
                      <FiSquare /> {t("dockurr.stop")}
                    </button>
                    <button
                      className={cn(
                        actionButtonClass,
                        "bg-amber-100 text-amber-900 hover:bg-amber-200 dark:bg-amber-900/40 dark:text-amber-300 dark:hover:bg-amber-900/60"
                      )}
                      onClick={(event) => {
                        event.stopPropagation();
                        runRestart(vm);
                      }}
                    >
                      <FiRotateCw /> {t("dockurr.restart")}
                    </button>
                    <button
                      className={cn(
                        actionButtonClass,
                        "bg-sky-100 text-sky-800 hover:bg-sky-200 dark:bg-sky-900/40 dark:text-sky-300 dark:hover:bg-sky-900/60"
                      )}
                      onClick={(event) => {
                        event.stopPropagation();
                        loadDetail(vm, "logs");
                      }}
                    >
                      <FiPlayCircle /> {t("dockurr.logs")}
                    </button>
                    <button
                      className={cn(
                        actionButtonClass,
                        "bg-slate-200 text-slate-800 hover:bg-slate-300 dark:bg-neutral-800 dark:text-neutral-100 dark:hover:bg-neutral-700"
                      )}
                      onClick={(event) => {
                        event.stopPropagation();
                        loadDetail(vm, "inspect");
                      }}
                    >
                      <FiInfo /> {t("dockurr.inspect")}
                    </button>
                  </div>
                </article>
              );
            })}

            {vms.length === 0 ? (
              <div className="rounded-2xl border border-dashed border-slate-200 p-5 text-center text-sm text-slate-500 dark:border-neutral-800 dark:text-neutral-400">
                {t("dockurr.empty")}
              </div>
            ) : null}
          </div>
        </section>

        <section className="flex h-[240px] flex-col rounded-3xl bg-white/70 p-4 shadow-soft ring-1 ring-slate-200/70 backdrop-blur dark:bg-neutral-900/55 dark:ring-neutral-800/70 xl:min-h-[210px] xl:max-h-[290px]">
          <h3 className="inline-flex items-center gap-2 text-sm font-semibold tracking-tight text-slate-900 dark:text-neutral-50">
            <FiActivity className="text-[14px]" />
            {t("dockurr.startup_logs")}
          </h3>
          <div
            ref={startupViewportRef}
            className="mt-3 min-h-0 flex-1 overflow-auto rounded-2xl bg-neutral-950 p-3 font-mono text-[12px] leading-relaxed text-neutral-50 shadow-sm ring-1 ring-neutral-900/10"
          >
            {runtimeLogs.length === 0 ? (
              <div className="grid h-full w-full place-items-center text-sm text-neutral-300">
                {t("dockurr.startup_empty")}
              </div>
            ) : (
              runtimeLogs.map((item) => (
                <div
                  key={item.id}
                  className={cn(
                    "whitespace-pre-wrap break-words",
                    item.level === "error" && "text-rose-300",
                    item.level === "warn" && "text-amber-300",
                    item.level === "debug" && "text-neutral-300",
                    (item.level === "info" || item.level === "other") && "text-emerald-200"
                  )}
                >
                  {item.text}
                </div>
              ))
            )}
          </div>
        </section>
      </div>

      <div className="flex min-h-0 min-w-0 flex-col gap-4 xl:min-h-[520px]">
        <section className="rounded-3xl bg-white/70 p-4 shadow-soft ring-1 ring-slate-200/70 backdrop-blur dark:bg-neutral-900/55 dark:ring-neutral-800/70">
          <h3 className="text-sm font-semibold tracking-tight text-slate-900 dark:text-neutral-50">
            {t("dockurr.create")}
          </h3>

          <form className="mt-4 space-y-4" onSubmit={submitCreate}>
            <div className="grid grid-cols-1 gap-3 sm:grid-cols-2">
              <label className="text-xs font-semibold text-slate-600 dark:text-neutral-300">
                {t("dockurr.os")}
                <select
                  value={os}
                  onChange={(event) => setOs(event.target.value as "windows" | "macos")}
                  className="mt-1 w-full rounded-2xl border border-slate-200 bg-white px-3 py-2 text-sm text-slate-900 shadow-sm outline-none focus:border-slate-300 dark:border-neutral-800 dark:bg-neutral-950/40 dark:text-neutral-50 dark:focus:border-neutral-700"
                >
                  <option value="windows">Windows</option>
                  <option value="macos">macOS</option>
                </select>
              </label>

              <label className="text-xs font-semibold text-slate-600 dark:text-neutral-300">
                {t("dockurr.version")}
                <select
                  value={version}
                  onChange={(event) => setVersion(event.target.value)}
                  className="mt-1 w-full rounded-2xl border border-slate-200 bg-white px-3 py-2 text-sm text-slate-900 shadow-sm outline-none focus:border-slate-300 dark:border-neutral-800 dark:bg-neutral-950/40 dark:text-neutral-50 dark:focus:border-neutral-700"
                >
                  {versionOptions.map((option) => (
                    <option key={option.value} value={option.value}>
                      {option.label}
                    </option>
                  ))}
                </select>
              </label>
            </div>

            <div className="grid grid-cols-1 gap-3 sm:grid-cols-2">
              <label className="text-xs font-semibold text-slate-600 dark:text-neutral-300">
                {t("dockurr.ram")}
                <input
                  value={ram}
                  onChange={(event) => setRam(event.target.value)}
                  className="mt-1 w-full rounded-2xl border border-slate-200 bg-white px-3 py-2 text-sm text-slate-900 shadow-sm outline-none placeholder:text-slate-400 focus:border-slate-300 dark:border-neutral-800 dark:bg-neutral-950/40 dark:text-neutral-50 dark:placeholder:text-neutral-500 dark:focus:border-neutral-700"
                />
              </label>

              <label className="text-xs font-semibold text-slate-600 dark:text-neutral-300">
                {t("dockurr.name")}
                <input
                  value={name}
                  onChange={(event) => setName(event.target.value)}
                  placeholder={t("dockurr.name_placeholder")}
                  className="mt-1 w-full rounded-2xl border border-slate-200 bg-white px-3 py-2 text-sm text-slate-900 shadow-sm outline-none placeholder:text-slate-400 focus:border-slate-300 dark:border-neutral-800 dark:bg-neutral-950/40 dark:text-neutral-50 dark:placeholder:text-neutral-500 dark:focus:border-neutral-700"
                />
              </label>
            </div>

            <div className="flex flex-col gap-3 sm:flex-row sm:items-center sm:justify-between">
              <label className="inline-flex items-center gap-3 rounded-2xl border border-slate-200 bg-white/80 px-3 py-2 text-sm text-slate-700 shadow-sm dark:border-neutral-800 dark:bg-neutral-950/35 dark:text-neutral-200">
                <span className="font-semibold">{t("dockurr.persist")}</span>
                <span
                  className={cn(
                    "rounded-full px-2 py-0.5 text-[11px] font-semibold",
                    persist
                      ? "bg-emerald-100 text-emerald-700 dark:bg-emerald-900/35 dark:text-emerald-300"
                      : "bg-slate-200 text-slate-600 dark:bg-neutral-800 dark:text-neutral-300"
                  )}
                >
                  {persist ? t("common.on") : t("common.off")}
                </span>
                <span className="relative inline-flex h-6 w-11 shrink-0">
                  <input
                    type="checkbox"
                    checked={persist}
                    onChange={(event) => setPersist(event.target.checked)}
                    className="peer sr-only"
                  />
                  <span className="absolute inset-0 rounded-full bg-slate-300 transition-colors peer-checked:bg-slate-900 dark:bg-neutral-700 dark:peer-checked:bg-neutral-100" />
                  <span className="absolute left-0.5 top-0.5 h-5 w-5 rounded-full bg-white shadow-sm transition-transform peer-checked:translate-x-5 dark:bg-neutral-900 dark:peer-checked:bg-neutral-900" />
                </span>
              </label>

              <button
                type="submit"
                disabled={createLoading || status.kind !== "connected"}
                className={cn(
                  "inline-flex h-10 items-center justify-center gap-2 rounded-2xl px-4 text-sm font-semibold shadow-sm transition-colors sm:min-w-[166px]",
                  createLoading || status.kind !== "connected"
                    ? "cursor-not-allowed bg-slate-200 text-slate-500 dark:bg-neutral-800 dark:text-neutral-500"
                    : "bg-slate-900 text-white hover:bg-slate-800 dark:bg-neutral-50 dark:text-neutral-900 dark:hover:bg-white"
                )}
              >
                <FiPlayCircle /> {createLoading ? t("dockurr.creating") : t("dockurr.launch")}
              </button>
            </div>
          </form>
        </section>

        <section className="flex min-h-[220px] flex-col rounded-3xl bg-white/70 p-4 shadow-soft ring-1 ring-slate-200/70 backdrop-blur dark:bg-neutral-900/55 dark:ring-neutral-800/70 xl:min-h-[180px] xl:flex-1">
          <div className="flex flex-wrap items-start justify-between gap-2">
            <div>
              <h3 className="text-sm font-semibold tracking-tight text-slate-900 dark:text-neutral-50">
                {t("dockurr.detail")}
              </h3>
              <div className="mt-1 font-mono text-[11px] text-slate-500 dark:text-neutral-400">
                {selectedVm?.name ?? t("dockurr.select_hint")}
              </div>
            </div>
            {selectedVm ? (
              <div className="flex flex-wrap gap-1.5">
                <button
                  className={cn(
                    actionButtonClass,
                    detailMode === "logs"
                      ? "bg-slate-900 text-white dark:bg-neutral-50 dark:text-neutral-900"
                      : "bg-slate-100 text-slate-700 hover:bg-slate-200 dark:bg-neutral-800 dark:text-neutral-100 dark:hover:bg-neutral-700"
                  )}
                  onClick={() => loadDetail(selectedVm, "logs")}
                >
                  {t("dockurr.logs")}
                </button>
                <button
                  className={cn(
                    actionButtonClass,
                    detailMode === "inspect"
                      ? "bg-slate-900 text-white dark:bg-neutral-50 dark:text-neutral-900"
                      : "bg-slate-100 text-slate-700 hover:bg-slate-200 dark:bg-neutral-800 dark:text-neutral-100 dark:hover:bg-neutral-700"
                  )}
                  onClick={() => loadDetail(selectedVm, "inspect")}
                >
                  {t("dockurr.inspect")}
                </button>
              </div>
            ) : null}
          </div>

          <pre className="mt-4 min-h-0 flex-1 overflow-auto rounded-2xl bg-neutral-950 p-3 font-mono text-[12px] leading-relaxed text-neutral-50 shadow-sm ring-1 ring-neutral-900/10">
            {selectedVm == null
              ? t("dockurr.select_hint")
              : detailLoading
                ? t("dockurr.loading")
                : detailText || t("dockurr.detail_empty")}
          </pre>
        </section>
      </div>
    </div>
  );
}
