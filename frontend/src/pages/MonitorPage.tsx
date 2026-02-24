import { useEffect, useMemo, useRef, useState } from "react";
import {
  area,
  axisBottom,
  axisLeft,
  curveMonotoneX,
  line as d3Line,
  scaleLinear,
  scaleTime,
  select,
  timeFormat,
} from "d3";
import {
  FiActivity,
  FiClock,
  FiCpu,
  FiHardDrive,
  FiMonitor,
  FiRefreshCw,
} from "react-icons/fi";

import { useI18n } from "../i18n";
import type { MonitorSnapshot, SessionInfo } from "../types";
import { cn } from "../util/cn";
import { getMonitorSocket, type MonitorSocketStatus } from "../ws/monitorSocket";

type Props = {
  session: SessionInfo;
};

const HISTORY_LIMIT = 90;

type TrendPoint = {
  tsMs: number;
  value: number | null;
};

function asRecord(value: unknown): Record<string, unknown> | null {
  if (!value || typeof value !== "object" || Array.isArray(value)) return null;
  return value as Record<string, unknown>;
}

function asString(value: unknown) {
  if (typeof value === "string") return value;
  if (typeof value === "number") return Number.isFinite(value) ? String(value) : "";
  if (typeof value === "boolean") return value ? "true" : "false";
  return "";
}

function asNumber(value: unknown, fallback = 0) {
  if (typeof value === "number" && Number.isFinite(value)) return value;
  if (typeof value === "string") {
    const parsed = Number(value);
    if (Number.isFinite(parsed)) return parsed;
  }
  return fallback;
}

function asNullableNumber(value: unknown) {
  if (value == null) return null;
  const parsed = asNumber(value, Number.NaN);
  return Number.isFinite(parsed) ? parsed : null;
}

function asNumberArray(value: unknown) {
  if (!Array.isArray(value)) return [];
  const out: number[] = [];
  for (const item of value) {
    const parsed = asNumber(item, Number.NaN);
    if (Number.isFinite(parsed)) {
      out.push(parsed);
    }
  }
  return out;
}

function clampPercent(value: number) {
  if (!Number.isFinite(value)) return 0;
  if (value < 0) return 0;
  if (value > 100) return 100;
  return value;
}

function parseSnapshot(raw: unknown): MonitorSnapshot | null {
  const root = asRecord(raw);
  if (!root) return null;
  const device = asRecord(root.device);
  const cpu = asRecord(root.cpu);
  const gpu = asRecord(root.gpu);
  const boot = asRecord(root.boot);
  const memory = asRecord(root.memory);
  const disk = asRecord(root.disk);
  if (!device || !cpu || !gpu || !boot || !memory || !disk) {
    return null;
  }

  return {
    ts_ms: Math.round(asNumber(root.ts_ms, Date.now())),
    device: {
      name: asString(device.name),
      model: asString(device.model),
      configuration: asString(device.configuration),
      os: asString(device.os),
      architecture: asString(device.architecture),
      cpu_model: asString(device.cpu_model),
      gpu_model: asString(device.gpu_model),
      logical_cores: Math.max(0, Math.round(asNumber(device.logical_cores, 0))),
      physical_cores: Math.max(0, Math.round(asNumber(device.physical_cores, 0))),
    },
    cpu: {
      base_frequency_mhz: asNumber(cpu.base_frequency_mhz, 0),
      frequency_mhz: asNumber(cpu.frequency_mhz, 0),
      total_load_percent: clampPercent(asNumber(cpu.total_load_percent, 0)),
      per_core_load_percent: asNumberArray(cpu.per_core_load_percent).map(clampPercent),
    },
    gpu: {
      model: asString(gpu.model),
      load_percent: asNullableNumber(gpu.load_percent),
    },
    boot: {
      uptime_seconds: Math.max(0, Math.round(asNumber(boot.uptime_seconds, 0))),
      started_at_ms: Math.max(0, Math.round(asNumber(boot.started_at_ms, 0))),
    },
    memory: {
      total_bytes: Math.max(0, Math.round(asNumber(memory.total_bytes, 0))),
      used_bytes: Math.max(0, Math.round(asNumber(memory.used_bytes, 0))),
      free_bytes: Math.max(0, Math.round(asNumber(memory.free_bytes, 0))),
      used_percent: clampPercent(asNumber(memory.used_percent, 0)),
    },
    disk: {
      total_bytes: Math.max(0, Math.round(asNumber(disk.total_bytes, 0))),
      used_bytes: Math.max(0, Math.round(asNumber(disk.used_bytes, 0))),
      free_bytes: Math.max(0, Math.round(asNumber(disk.free_bytes, 0))),
      used_percent: clampPercent(asNumber(disk.used_percent, 0)),
    },
  };
}

function formatBytes(bytes: number) {
  if (!Number.isFinite(bytes) || bytes <= 0) return "0 B";
  const units = ["B", "KB", "MB", "GB", "TB", "PB"] as const;
  let value = bytes;
  let idx = 0;
  while (value >= 1024 && idx < units.length - 1) {
    value /= 1024;
    idx += 1;
  }
  const digits = value >= 100 || idx === 0 ? 0 : 1;
  return `${value.toFixed(digits)} ${units[idx]}`;
}

function formatPercent(value: number | null) {
  if (value == null || !Number.isFinite(value)) return "--";
  return `${clampPercent(value).toFixed(1)}%`;
}

function formatUptime(seconds: number) {
  if (!Number.isFinite(seconds) || seconds <= 0) return "0s";
  const days = Math.floor(seconds / 86400);
  const hours = Math.floor((seconds % 86400) / 3600);
  const minutes = Math.floor((seconds % 3600) / 60);
  const secs = Math.floor(seconds % 60);
  const parts: string[] = [];
  if (days > 0) parts.push(`${days}d`);
  if (hours > 0 || days > 0) parts.push(`${hours}h`);
  if (minutes > 0 || hours > 0 || days > 0) parts.push(`${minutes}m`);
  parts.push(`${secs}s`);
  return parts.join(" ");
}

function statusClass(status: MonitorSocketStatus) {
  if (status.kind === "connected") return "bg-emerald-500";
  if (status.kind === "connecting") return "bg-amber-500";
  if (status.kind === "failed" || status.kind === "error") return "bg-rose-500";
  return "bg-slate-400";
}

function statusLabel(status: MonitorSocketStatus, t: (key: string) => string) {
  if (status.kind === "connected") return t("monitor.status_connected");
  if (status.kind === "connecting") return t("monitor.status_connecting");
  if (status.kind === "failed") return t("monitor.status_failed");
  if (status.kind === "error") return status.message || t("monitor.status_error");
  if (status.kind === "closed") return t("monitor.status_closed");
  return t("monitor.status_disconnected");
}

function appendHistory(previous: TrendPoint[], tsMs: number, value: number | null) {
  const next = [...previous, { tsMs, value }];
  if (next.length <= HISTORY_LIMIT) return next;
  return next.slice(next.length - HISTORY_LIMIT);
}

type TrendProps = {
  points: TrendPoint[];
  strokeColor: string;
  fillColor: string;
};

function TrendChart({ points, strokeColor, fillColor }: TrendProps) {
  const containerRef = useRef<HTMLDivElement | null>(null);
  const svgRef = useRef<SVGSVGElement | null>(null);
  const [chartWidth, setChartWidth] = useState(0);

  useEffect(() => {
    const container = containerRef.current;
    if (!container) return;

    const update = () => {
      setChartWidth(Math.max(container.clientWidth, 280));
    };
    update();

    if (typeof ResizeObserver === "undefined") {
      window.addEventListener("resize", update);
      return () => {
        window.removeEventListener("resize", update);
      };
    }

    const observer = new ResizeObserver(() => {
      update();
    });
    observer.observe(container);
    return () => {
      observer.disconnect();
    };
  }, []);

  useEffect(() => {
    const svgNode = svgRef.current;
    if (!svgNode || chartWidth <= 0) return;

    const width = chartWidth;
    const height = 152;
    const margin = { top: 8, right: 10, bottom: 24, left: 34 };
    const innerWidth = Math.max(1, width - margin.left - margin.right);
    const innerHeight = Math.max(1, height - margin.top - margin.bottom);

    const isDark = document.documentElement.classList.contains("dark");
    const axisColor = isDark ? "#8f8f8f" : "#64748b";
    const gridColor = isDark ? "#2a2a2a" : "#e2e8f0";

    const samples = points
      .filter((item) => Number.isFinite(item.tsMs))
      .map((item) => ({
        time: new Date(item.tsMs),
        value: item.value == null || !Number.isFinite(item.value) ? null : clampPercent(item.value),
      }));
    const validPoints = samples.filter((item): item is { time: Date; value: number } => item.value !== null);

    const now = Date.now();
    let minTs = now - 1000;
    let maxTs = now;
    if (samples.length > 0) {
      minTs = samples[0].time.getTime();
      maxTs = samples[samples.length - 1].time.getTime();
    }
    if (maxTs <= minTs) {
      maxTs = minTs + 1000;
    }

    const xScale = scaleTime().domain([new Date(minTs), new Date(maxTs)]).range([0, innerWidth]);
    const yScale = scaleLinear().domain([0, 100]).range([innerHeight, 0]);
    const formatTime = timeFormat("%H:%M:%S");

    const svg = select(svgNode);
    svg.selectAll("*").remove();
    svg
      .attr("viewBox", `0 0 ${width} ${height}`)
      .attr("preserveAspectRatio", "none");

    const plot = svg.append("g").attr("transform", `translate(${margin.left},${margin.top})`);

    plot
      .selectAll(".grid-line")
      .data(yScale.ticks(5))
      .join("line")
      .attr("class", "grid-line")
      .attr("x1", 0)
      .attr("x2", innerWidth)
      .attr("y1", (d) => yScale(d))
      .attr("y2", (d) => yScale(d))
      .attr("stroke", gridColor)
      .attr("stroke-width", 1);

    const xAxis = axisBottom(xScale)
      .ticks(Math.max(3, Math.floor(innerWidth / 110)))
      .tickFormat((d) => formatTime(d as Date));
    const yAxis = axisLeft(yScale)
      .ticks(5)
      .tickFormat((d) => `${Math.round(Number(d))}%`);

    const xAxisGroup = plot
      .append("g")
      .attr("transform", `translate(0,${innerHeight})`)
      .call(xAxis);
    const yAxisGroup = plot.append("g").call(yAxis);

    xAxisGroup.selectAll("path, line").attr("stroke", axisColor);
    yAxisGroup.selectAll("path, line").attr("stroke", axisColor);
    xAxisGroup.selectAll("text").attr("fill", axisColor).attr("font-size", 10);
    yAxisGroup.selectAll("text").attr("fill", axisColor).attr("font-size", 10);

    if (validPoints.length > 1) {
      const areaPath = area<{ time: Date; value: number }>()
        .x((d) => xScale(d.time))
        .y0(innerHeight)
        .y1((d) => yScale(d.value))
        .curve(curveMonotoneX);
      plot
        .append("path")
        .datum(validPoints)
        .attr("fill", fillColor)
        .attr("fill-opacity", 0.18)
        .attr("d", areaPath);
    }

    if (validPoints.length > 0) {
      const linePath = d3Line<{ time: Date; value: number }>()
        .x((d) => xScale(d.time))
        .y((d) => yScale(d.value))
        .curve(curveMonotoneX);
      plot
        .append("path")
        .datum(validPoints)
        .attr("fill", "none")
        .attr("stroke", strokeColor)
        .attr("stroke-width", 2.2)
        .attr("stroke-linecap", "round")
        .attr("stroke-linejoin", "round")
        .attr("d", linePath);

      const latest = validPoints[validPoints.length - 1];
      if (latest) {
        plot
          .append("circle")
          .attr("cx", xScale(latest.time))
          .attr("cy", yScale(latest.value))
          .attr("r", 3)
          .attr("fill", strokeColor);
      }
    }
  }, [chartWidth, fillColor, points, strokeColor]);

  return (
    <div ref={containerRef} className="w-full">
      <svg ref={svgRef} className="h-[152px] w-full" />
    </div>
  );
}

type UsageBarProps = {
  label: string;
  value: number;
  barClass: string;
};

function UsageBar({ label, value, barClass }: UsageBarProps) {
  return (
    <div className="space-y-1.5">
      <div className="flex items-center justify-between text-xs text-slate-500 dark:text-neutral-400">
        <span>{label}</span>
        <span>{formatPercent(value)}</span>
      </div>
      <div className="h-2 rounded-full bg-slate-200 dark:bg-neutral-800">
        <div
          className={cn("h-2 rounded-full transition-all duration-300", barClass)}
          style={{ width: `${clampPercent(value)}%` }}
        />
      </div>
    </div>
  );
}

type LoadStyle = {
  strokeColor: string;
  fillColor: string;
  barClass: string;
};

function styleForLoad(value: number | null | undefined): LoadStyle {
  if (value == null || !Number.isFinite(value)) {
    return {
      strokeColor: "#64748b",
      fillColor: "#64748b",
      barClass: "bg-slate-500",
    };
  }
  const load = clampPercent(value);
  if (load >= 85) {
    return {
      strokeColor: "#ef4444",
      fillColor: "#ef4444",
      barClass: "bg-rose-500",
    };
  }
  if (load >= 60) {
    return {
      strokeColor: "#f59e0b",
      fillColor: "#f59e0b",
      barClass: "bg-amber-500",
    };
  }
  return {
    strokeColor: "#10b981",
    fillColor: "#10b981",
    barClass: "bg-emerald-500",
  };
}

export default function MonitorPage({ session }: Props) {
  const { t } = useI18n();
  const socket = useMemo(() => getMonitorSocket(), []);

  const [status, setStatus] = useState<MonitorSocketStatus>({ kind: "disconnected" });
  const [snapshot, setSnapshot] = useState<MonitorSnapshot | null>(null);
  const [cpuHistory, setCpuHistory] = useState<TrendPoint[]>([]);
  const [gpuHistory, setGpuHistory] = useState<TrendPoint[]>([]);
  const [memoryHistory, setMemoryHistory] = useState<TrendPoint[]>([]);
  const [diskHistory, setDiskHistory] = useState<TrendPoint[]>([]);

  useEffect(() => {
    setSnapshot(null);
    setCpuHistory([]);
    setGpuHistory([]);
    setMemoryHistory([]);
    setDiskHistory([]);

    const unsubscribeStatus = socket.subscribeStatus((next) => {
      setStatus(next);
    });
    const unsubscribeMessages = socket.subscribeMessages((payload) => {
      if (asString(payload.event) !== "monitor_snapshot") {
        return;
      }
      const parsed = parseSnapshot(payload.snapshot);
      if (!parsed) {
        return;
      }
      setSnapshot(parsed);
      setCpuHistory((prev) => appendHistory(prev, parsed.ts_ms, parsed.cpu.total_load_percent));
      setGpuHistory((prev) => appendHistory(prev, parsed.ts_ms, parsed.gpu.load_percent));
      setMemoryHistory((prev) => appendHistory(prev, parsed.ts_ms, parsed.memory.used_percent));
      setDiskHistory((prev) => appendHistory(prev, parsed.ts_ms, parsed.disk.used_percent));
    });

    socket.start(session);
    return () => {
      unsubscribeMessages();
      unsubscribeStatus();
      socket.stop();
    };
  }, [session, socket]);

  const coreLoads = snapshot?.cpu.per_core_load_percent ?? [];
  const currentCpuFreqMHz = snapshot?.cpu.frequency_mhz ?? 0;
  const cpuBaseFreqMHz = snapshot?.cpu.base_frequency_mhz ?? 0;
  const latestTs = snapshot?.ts_ms ?? 0;
  const latestTimeLabel = latestTs > 0
    ? new Date(latestTs).toLocaleTimeString([], { hour12: false })
    : "--:--:--";
  const cpuStyle = styleForLoad(snapshot?.cpu.total_load_percent);
  const gpuStyle = styleForLoad(snapshot?.gpu.load_percent);
  const memoryStyle = styleForLoad(snapshot?.memory.used_percent);
  const diskStyle = styleForLoad(snapshot?.disk.used_percent);

  return (
    <div className="h-full min-h-0 overflow-visible">
      <div className="h-full min-h-0 overscroll-contain overflow-y-auto px-5 pb-6 pt-3">
        <div className="space-y-4">
        <section className="rounded-3xl bg-white/75 p-4 shadow-soft ring-1 ring-slate-200/70 backdrop-blur dark:bg-neutral-900/60 dark:ring-neutral-800/80">
          <div className="flex flex-wrap items-center justify-between gap-3">
            <div className="flex items-center gap-2">
              <FiMonitor className="text-slate-500 dark:text-neutral-300" />
              <h2 className="text-sm font-semibold text-slate-900 dark:text-neutral-50">{t("monitor.title")}</h2>
            </div>
            <div className="flex items-center gap-4">
              <div className="flex items-center gap-2 text-xs text-slate-500 dark:text-neutral-400">
                <span className={cn("h-2.5 w-2.5 rounded-full", statusClass(status))} />
                <span>{statusLabel(status, t)}</span>
              </div>
              <button
                className="inline-flex items-center gap-1 rounded-xl border border-slate-200 bg-white px-2.5 py-1.5 text-xs font-semibold text-slate-700 transition-colors hover:bg-slate-50 dark:border-neutral-700 dark:bg-neutral-950 dark:text-neutral-100 dark:hover:bg-neutral-900"
                onClick={() => socket.requestSnapshot()}
              >
                <FiRefreshCw />
                {t("monitor.refresh")}
              </button>
            </div>
          </div>
          <div className="mt-4 grid gap-3 sm:grid-cols-2 xl:grid-cols-4">
            <div className="rounded-2xl bg-slate-100/80 p-3 dark:bg-neutral-900">
              <div className="text-[11px] uppercase tracking-wide text-slate-500 dark:text-neutral-400">{t("monitor.device_name")}</div>
              <div className="mt-1 truncate text-sm font-semibold text-slate-900 dark:text-neutral-50">
                {snapshot?.device.name || "--"}
              </div>
            </div>
            <div className="rounded-2xl bg-slate-100/80 p-3 dark:bg-neutral-900">
              <div className="text-[11px] uppercase tracking-wide text-slate-500 dark:text-neutral-400">{t("monitor.device_model")}</div>
              <div className="mt-1 truncate text-sm font-semibold text-slate-900 dark:text-neutral-50">
                {snapshot?.device.model || "--"}
              </div>
            </div>
            <div className="rounded-2xl bg-slate-100/80 p-3 dark:bg-neutral-900">
              <div className="text-[11px] uppercase tracking-wide text-slate-500 dark:text-neutral-400">{t("monitor.device_config")}</div>
              <div className="mt-1 truncate text-sm font-semibold text-slate-900 dark:text-neutral-50">
                {snapshot?.device.configuration || "--"}
              </div>
            </div>
            <div className="rounded-2xl bg-slate-100/80 p-3 dark:bg-neutral-900">
              <div className="text-[11px] uppercase tracking-wide text-slate-500 dark:text-neutral-400">{t("monitor.uptime")}</div>
              <div className="mt-1 flex items-center gap-1 text-sm font-semibold text-slate-900 dark:text-neutral-50">
                <FiClock className="opacity-70" />
                <span>{formatUptime(snapshot?.boot.uptime_seconds ?? 0)}</span>
              </div>
            </div>
          </div>
        </section>

        <section className="grid gap-4 xl:grid-cols-3">
          <div className="rounded-3xl bg-white/75 p-4 shadow-soft ring-1 ring-slate-200/70 backdrop-blur dark:bg-neutral-900/60 dark:ring-neutral-800/80 xl:col-span-2">
            <div className="flex items-center justify-between">
              <div className="flex items-center gap-2 text-sm font-semibold text-slate-900 dark:text-neutral-50">
                <FiCpu />
                <span>{t("monitor.cpu")}</span>
              </div>
              <div className="text-xs text-slate-500 dark:text-neutral-400">
                {t("monitor.updated_at")}: {latestTimeLabel}
              </div>
            </div>
            <div className="mt-3 grid gap-3 sm:grid-cols-3">
              <div className="rounded-2xl bg-slate-100/80 p-3 dark:bg-neutral-900">
                <div className="text-xs text-slate-500 dark:text-neutral-400">{t("monitor.cpu_total_load")}</div>
                <div className="mt-1 text-2xl font-semibold text-slate-900 dark:text-neutral-50">
                  {formatPercent(snapshot?.cpu.total_load_percent ?? 0)}
                </div>
              </div>
              <div className="rounded-2xl bg-slate-100/80 p-3 dark:bg-neutral-900">
                <div className="text-xs text-slate-500 dark:text-neutral-400">{t("monitor.cpu_freq")}</div>
                <div className="mt-1 text-2xl font-semibold text-slate-900 dark:text-neutral-50">
                  {currentCpuFreqMHz > 0 ? `${currentCpuFreqMHz.toFixed(0)} MHz` : "--"}
                </div>
              </div>
              <div className="rounded-2xl bg-slate-100/80 p-3 dark:bg-neutral-900">
                <div className="text-xs text-slate-500 dark:text-neutral-400">{t("monitor.cpu_base_freq")}</div>
                <div className="mt-1 text-2xl font-semibold text-slate-900 dark:text-neutral-50">
                  {cpuBaseFreqMHz > 0 ? `${cpuBaseFreqMHz.toFixed(0)} MHz` : "--"}
                </div>
              </div>
            </div>
            <div className="mt-4">
              <div className="mb-1 text-xs text-slate-500 dark:text-neutral-400">{t("monitor.cpu_history")}</div>
              <TrendChart points={cpuHistory} strokeColor={cpuStyle.strokeColor} fillColor={cpuStyle.fillColor} />
            </div>
            <div className="mt-4">
              <div className="mb-2 text-xs text-slate-500 dark:text-neutral-400">{t("monitor.cpu_per_core")}</div>
              {coreLoads.length > 0 ? (
                <div className="grid gap-2 sm:grid-cols-2 xl:grid-cols-3">
                  {coreLoads.map((load, idx) => (
                    <UsageBar
                      key={`core-${idx + 1}`}
                      label={`${t("monitor.core")} ${idx + 1}`}
                      value={load}
                      barClass={styleForLoad(load).barClass}
                    />
                  ))}
                </div>
              ) : (
                <div className="text-xs text-slate-500 dark:text-neutral-400">{t("monitor.no_data")}</div>
              )}
            </div>
          </div>

          <div className="space-y-4">
            <div className="rounded-3xl bg-white/75 p-4 shadow-soft ring-1 ring-slate-200/70 backdrop-blur dark:bg-neutral-900/60 dark:ring-neutral-800/80">
              <div className="flex items-center gap-2 text-sm font-semibold text-slate-900 dark:text-neutral-50">
                <FiActivity />
                <span>{t("monitor.gpu")}</span>
              </div>
              <div className="mt-3 rounded-2xl bg-slate-100/80 p-3 dark:bg-neutral-900">
                <div className="text-xs text-slate-500 dark:text-neutral-400">{t("monitor.gpu_model")}</div>
                <div className="mt-1 truncate text-sm font-semibold text-slate-900 dark:text-neutral-50">
                  {snapshot?.gpu.model || snapshot?.device.gpu_model || "--"}
                </div>
              </div>
              <div className="mt-3 rounded-2xl bg-slate-100/80 p-3 dark:bg-neutral-900">
                <div className="text-xs text-slate-500 dark:text-neutral-400">{t("monitor.gpu_load")}</div>
                <div className="mt-1 text-2xl font-semibold text-slate-900 dark:text-neutral-50">
                  {formatPercent(snapshot?.gpu.load_percent ?? null)}
                </div>
              </div>
              <div className="mt-3">
                <div className="mb-1 text-xs text-slate-500 dark:text-neutral-400">{t("monitor.gpu_history")}</div>
                <TrendChart points={gpuHistory} strokeColor={gpuStyle.strokeColor} fillColor={gpuStyle.fillColor} />
              </div>
            </div>

            <div className="rounded-3xl bg-white/75 p-4 shadow-soft ring-1 ring-slate-200/70 backdrop-blur dark:bg-neutral-900/60 dark:ring-neutral-800/80">
              <div className="flex items-center gap-2 text-sm font-semibold text-slate-900 dark:text-neutral-50">
                <FiMonitor />
                <span>{t("monitor.system")}</span>
              </div>
              <div className="mt-3 space-y-2 text-xs text-slate-600 dark:text-neutral-300">
                <div className="flex justify-between gap-2">
                  <span>{t("monitor.os")}</span>
                  <span className="truncate text-right">{snapshot?.device.os || "--"}</span>
                </div>
                <div className="flex justify-between gap-2">
                  <span>{t("monitor.arch")}</span>
                  <span>{snapshot?.device.architecture || "--"}</span>
                </div>
                <div className="flex justify-between gap-2">
                  <span>{t("monitor.logical_cores")}</span>
                  <span>{snapshot?.device.logical_cores ?? 0}</span>
                </div>
                <div className="flex justify-between gap-2">
                  <span>{t("monitor.physical_cores")}</span>
                  <span>{snapshot?.device.physical_cores ?? 0}</span>
                </div>
              </div>
            </div>
          </div>

          <div className="rounded-3xl bg-white/75 p-4 shadow-soft ring-1 ring-slate-200/70 backdrop-blur dark:bg-neutral-900/60 dark:ring-neutral-800/80">
            <div className="flex items-center gap-2 text-sm font-semibold text-slate-900 dark:text-neutral-50">
              <FiActivity />
              <span>{t("monitor.memory")}</span>
            </div>
            <div className="mt-3 space-y-2">
              <UsageBar
                label={t("monitor.memory_used")}
                value={snapshot?.memory.used_percent ?? 0}
                barClass={memoryStyle.barClass}
              />
              <div className="text-xs text-slate-500 dark:text-neutral-400">
                {formatBytes(snapshot?.memory.used_bytes ?? 0)} / {formatBytes(snapshot?.memory.total_bytes ?? 0)}
              </div>
            </div>
            <div className="mt-3">
              <div className="mb-1 text-xs text-slate-500 dark:text-neutral-400">{t("monitor.memory_history")}</div>
              <TrendChart points={memoryHistory} strokeColor={memoryStyle.strokeColor} fillColor={memoryStyle.fillColor} />
            </div>
          </div>

          <div className="rounded-3xl bg-white/75 p-4 shadow-soft ring-1 ring-slate-200/70 backdrop-blur dark:bg-neutral-900/60 dark:ring-neutral-800/80">
            <div className="flex items-center gap-2 text-sm font-semibold text-slate-900 dark:text-neutral-50">
              <FiHardDrive />
              <span>{t("monitor.disk")}</span>
            </div>
            <div className="mt-3 space-y-2">
              <UsageBar
                label={t("monitor.disk_used")}
                value={snapshot?.disk.used_percent ?? 0}
                barClass={diskStyle.barClass}
              />
              <div className="text-xs text-slate-500 dark:text-neutral-400">
                {formatBytes(snapshot?.disk.used_bytes ?? 0)} / {formatBytes(snapshot?.disk.total_bytes ?? 0)}
              </div>
            </div>
            <div className="mt-3">
              <div className="mb-1 text-xs text-slate-500 dark:text-neutral-400">{t("monitor.disk_history")}</div>
              <TrendChart points={diskHistory} strokeColor={diskStyle.strokeColor} fillColor={diskStyle.fillColor} />
            </div>
          </div>
        </section>
        </div>
      </div>
    </div>
  );
}
