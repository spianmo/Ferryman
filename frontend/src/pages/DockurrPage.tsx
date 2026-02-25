import { useCallback, useEffect, useMemo, useRef, useState, type ChangeEvent, type FormEvent } from "react";
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
  FiChevronLeft,
  FiDownload,
  FiEye,
  FiFileText,
  FiFolder,
  FiHardDrive,
  FiInfo,
  FiPlus,
  FiRefreshCw,
  FiRotateCw,
  FiSave,
  FiServer,
  FiSquare,
  FiUpload,
  FiX,
} from "react-icons/fi";

import {
  getDockerContainerInspect,
  getDockerContainerLogs,
  getDockerContainerProcesses,
  getDockerContainerStats,
  listDockerContainerFiles,
  readDockerContainerFile,
  writeDockerContainerFile,
} from "../api/client";
import ImagePreview from "../components/files/preview/ImagePreview";
import MarkdownPreview from "../components/files/preview/MarkdownPreview";
import PdfPreview from "../components/files/preview/PdfPreview";
import { useI18n } from "../i18n";
import { toast } from "../toast";
import type {
  DockerContainerFileEntry,
  DockerContainerProcesses,
  DockerContainerStats,
  DockurrVmInfo,
  SessionInfo,
} from "../types";
import { decodeBase64Utf8, encodeBase64Utf8 } from "../util/codec";
import { cn } from "../util/cn";
import { getDockurrSocket, type DockurrSocketStatus } from "../ws/dockurrSocket";

type Props = {
  session: SessionInfo;
};

type DetailTab = "load" | "inspect" | "logs" | "files";
type FileMode = "none" | "text" | "markdown" | "image" | "pdf";

type VersionOption = {
  value: string;
  label: string;
};

type TrendPoint = {
  tsMs: number;
  value: number | null;
};

type LoadStyle = {
  strokeColor: string;
  fillColor: string;
  barClass: string;
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
const RAM_MIN_GB = 1;
const RAM_MAX_GB = 64;
const DISK_MIN_GB = 16;
const DISK_MAX_GB = 512;

const HISTORY_LIMIT = 90;
const STATS_REFRESH_MS = 2200;
const PROCESS_REFRESH_MS = 3000;
const LOG_REFRESH_MS = 1500;
const LOG_TAIL = 600;
const LOG_MAX_LINES = 5000;
const PROCESS_LIMIT = 120;

const actionButtonClass =
  "inline-flex h-8 items-center gap-1 rounded-xl px-2.5 text-xs font-semibold transition-colors";

const IMAGE_EXTENSIONS = new Set(["png", "jpg", "jpeg", "gif", "webp", "bmp", "svg", "ico", "tif", "tiff"]);
const MARKDOWN_EXTENSIONS = new Set(["md", "markdown"]);
const PDF_EXTENSIONS = new Set(["pdf"]);
const IMAGE_MIME_TYPES: Record<string, string> = {
  png: "image/png",
  jpg: "image/jpeg",
  jpeg: "image/jpeg",
  gif: "image/gif",
  webp: "image/webp",
  bmp: "image/bmp",
  svg: "image/svg+xml",
  ico: "image/x-icon",
  tif: "image/tiff",
  tiff: "image/tiff",
};

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

function clampInt(value: number, min: number, max: number) {
  if (!Number.isFinite(value)) return min;
  return Math.min(max, Math.max(min, Math.round(value)));
}

function fileExtension(name: string) {
  const idx = name.lastIndexOf(".");
  if (idx <= 0 || idx >= name.length - 1) return "";
  return name.slice(idx + 1).toLowerCase();
}

function normalizeContainerPath(value: string) {
  const trimmed = value.trim();
  if (!trimmed) return "/";
  let normalized = trimmed.replaceAll("\\", "/");
  if (!normalized.startsWith("/")) {
    normalized = `/${normalized}`;
  }
  normalized = normalized.replace(/\/+/g, "/");
  if (normalized.length > 1 && normalized.endsWith("/")) {
    normalized = normalized.slice(0, -1);
  }
  return normalized || "/";
}

function parentContainerPath(path: string) {
  const normalized = normalizeContainerPath(path);
  if (normalized === "/") return "/";
  const idx = normalized.lastIndexOf("/");
  if (idx <= 0) return "/";
  return normalized.slice(0, idx) || "/";
}

function basename(path: string) {
  const normalized = normalizeContainerPath(path);
  if (normalized === "/") return "";
  const idx = normalized.lastIndexOf("/");
  return idx >= 0 ? normalized.slice(idx + 1) : normalized;
}

function joinPath(dirPath: string, fileName: string) {
  const dir = normalizeContainerPath(dirPath);
  if (dir === "/") return `/${fileName}`;
  return `${dir}/${fileName}`;
}

function base64ToBytes(base64: string) {
  const binary = atob(base64);
  const bytes = new Uint8Array(binary.length);
  for (let i = 0; i < binary.length; i += 1) {
    bytes[i] = binary.charCodeAt(i);
  }
  return bytes;
}

function bytesToBase64(bytes: Uint8Array) {
  let binary = "";
  for (let i = 0; i < bytes.length; i += 1) {
    binary += String.fromCharCode(bytes[i]);
  }
  return btoa(binary);
}

async function browserFileToBase64(file: File) {
  const bytes = new Uint8Array(await file.arrayBuffer());
  return bytesToBase64(bytes);
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

function clampPercent(value: number) {
  if (!Number.isFinite(value)) return 0;
  if (value < 0) return 0;
  if (value > 100) return 100;
  return value;
}

function appendHistory(previous: TrendPoint[], tsMs: number, value: number | null) {
  const next = [...previous, { tsMs, value }];
  if (next.length <= HISTORY_LIMIT) return next;
  return next.slice(next.length - HISTORY_LIMIT);
}

function normalizeLogText(value: string) {
  return value.replace(/\r\n/g, "\n");
}

function clampLogLines(value: string, maxLines: number) {
  if (!value || maxLines <= 0) return value;
  const normalized = normalizeLogText(value);
  const lines = normalized.split("\n");
  if (lines.length <= maxLines) {
    return normalized;
  }
  return lines.slice(lines.length - maxLines).join("\n");
}

function mergeLogText(previous: string, nextSnapshot: string) {
  const prev = normalizeLogText(previous);
  const next = normalizeLogText(nextSnapshot);
  if (!prev) return next;
  if (!next) return prev;
  if (prev === next) return prev;
  if (prev.endsWith(next)) return prev;

  const overlapLimit = Math.min(prev.length, next.length);
  for (let overlap = overlapLimit; overlap > 0; overlap -= 1) {
    if (prev.slice(prev.length - overlap) === next.slice(0, overlap)) {
      return prev + next.slice(overlap);
    }
  }
  return next;
}

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

function containerMimeType(fileName: string) {
  const ext = fileExtension(fileName);
  if (PDF_EXTENSIONS.has(ext)) return "application/pdf";
  return IMAGE_MIME_TYPES[ext] ?? "application/octet-stream";
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
      minTs = samples[0]?.time.getTime() ?? minTs;
      maxTs = samples[samples.length - 1]?.time.getTime() ?? maxTs;
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
    }
  }, [chartWidth, fillColor, points, strokeColor]);

  return (
    <div ref={containerRef} className="w-full">
      <svg ref={svgRef} className="h-[152px] w-full" />
    </div>
  );
}

export default function DockurrPage({ session }: Props) {
  const { t } = useI18n();
  const socket = useMemo(() => getDockurrSocket(), []);
  const uploadInputRef = useRef<HTMLInputElement | null>(null);
  const startupViewportRef = useRef<HTMLDivElement | null>(null);
  const logsViewportRef = useRef<HTMLPreElement | null>(null);

  const [vms, setVms] = useState<DockurrVmInfo[]>([]);
  const [selectedName, setSelectedName] = useState("");
  const [listError, setListError] = useState("");
  const [status, setStatus] = useState<DockurrSocketStatus>({ kind: "disconnected" });

  const [os, setOs] = useState<"windows" | "macos">("windows");
  const [version, setVersion] = useState(WINDOWS_VERSIONS[0]?.value ?? "11");
  const [ramGb, setRamGb] = useState(4);
  const [diskGb, setDiskGb] = useState(64);
  const [name, setName] = useState("");
  const [persist, setPersist] = useState(false);
  const [createLoading, setCreateLoading] = useState(false);
  const [createDialogOpen, setCreateDialogOpen] = useState(false);

  const [actionLoading, setActionLoading] = useState<"" | "stop" | "restart">("");
  const [detailTab, setDetailTab] = useState<DetailTab>("load");

  const [statsLoading, setStatsLoading] = useState(false);
  const [stats, setStats] = useState<DockerContainerStats | null>(null);
  const [cpuHistory, setCpuHistory] = useState<TrendPoint[]>([]);
  const [memoryHistory, setMemoryHistory] = useState<TrendPoint[]>([]);
  const [processesLoading, setProcessesLoading] = useState(false);
  const [processes, setProcesses] = useState<DockerContainerProcesses>({
    name: "",
    columns: [],
    rows: [],
  });

  const [inspectLoading, setInspectLoading] = useState(false);
  const [inspectText, setInspectText] = useState("");
  const [logsLoading, setLogsLoading] = useState(false);
  const [logsText, setLogsText] = useState("");
  const [logsAutoScroll, setLogsAutoScroll] = useState(true);

  const [filePath, setFilePath] = useState("/");
  const [fileEntries, setFileEntries] = useState<DockerContainerFileEntry[]>([]);
  const [filesLoading, setFilesLoading] = useState(false);
  const [filesError, setFilesError] = useState("");
  const [fileLoading, setFileLoading] = useState(false);
  const [fileSaving, setFileSaving] = useState(false);
  const [fileUploading, setFileUploading] = useState(false);
  const [fileDownloading, setFileDownloading] = useState(false);
  const [selectedFilePath, setSelectedFilePath] = useState("");
  const [selectedFileMode, setSelectedFileMode] = useState<FileMode>("none");
  const [selectedFileText, setSelectedFileText] = useState("");
  const [selectedPdfBytes, setSelectedPdfBytes] = useState<Uint8Array | null>(null);
  const [selectedImageUrl, setSelectedImageUrl] = useState("");
  const [markdownPreview, setMarkdownPreview] = useState(true);

  const [runtimeAutoScroll, setRuntimeAutoScroll] = useState(true);
  const [runtimeLogs, setRuntimeLogs] = useState<RuntimeLogEntry[]>([]);

  const currentCreateRequestIdRef = useRef("");
  const pendingCreateNameRef = useRef("");

  const versionOptions = useMemo(() => (os === "windows" ? WINDOWS_VERSIONS : MACOS_VERSIONS), [os]);
  const selectedVm = useMemo(() => vms.find((vm) => vm.name === selectedName) ?? null, [selectedName, vms]);

  const replaceImageUrl = useCallback((next: string) => {
    setSelectedImageUrl((current) => {
      if (current) {
        URL.revokeObjectURL(current);
      }
      return next;
    });
  }, []);

  const resetSelectedFile = useCallback(() => {
    setSelectedFilePath("");
    setSelectedFileMode("none");
    setSelectedFileText("");
    setSelectedPdfBytes(null);
    replaceImageUrl("");
    setMarkdownPreview(true);
  }, [replaceImageUrl]);

  useEffect(() => {
    return () => {
      if (selectedImageUrl) {
        URL.revokeObjectURL(selectedImageUrl);
      }
    };
  }, [selectedImageUrl]);

  useEffect(() => {
    if (versionOptions.some((option) => option.value === version)) {
      return;
    }
    setVersion(versionOptions[0]?.value ?? "");
  }, [version, versionOptions]);

  useEffect(() => {
    if (!runtimeAutoScroll) return;
    const viewport = startupViewportRef.current;
    if (!viewport) return;
    viewport.scrollTop = viewport.scrollHeight;
  }, [runtimeAutoScroll, runtimeLogs]);

  useEffect(() => {
    if (!logsAutoScroll || detailTab !== "logs") {
      return;
    }
    const viewport = logsViewportRef.current;
    if (!viewport) {
      return;
    }
    viewport.scrollTop = viewport.scrollHeight;
  }, [detailTab, logsAutoScroll, logsText]);

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

  const refreshVms = useCallback(() => {
    void sendAction("list", {});
  }, [socket]);

  useEffect(() => {
    setVms([]);
    setSelectedName("");
    setListError("");
    setRuntimeLogs([]);
    setCreateLoading(false);
    setCreateDialogOpen(false);
    setActionLoading("");
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
        if (action === "stop" || action === "restart") {
          setActionLoading("");
        }
        appendRuntimeLog("error", action || "dockurr", error, asString(payload.ts));
        toast.error(error);
        return;
      }

      if (action === "create") {
        setCreateLoading(false);
        setCreateDialogOpen(false);
        const vm = parseVm(payload.vm);
        const createdName = vm?.name || pendingCreateNameRef.current;
        toast.success(t("toast.dockurr_created", { name: createdName || t("dockurr.unnamed") }));
        setName("");
        pendingCreateNameRef.current = "";
        if (createdName) {
          setSelectedName(createdName);
          setDetailTab("load");
        }
        refreshVms();
        return;
      }

      if (action === "stop") {
        setActionLoading("");
        toast.success(t("toast.dockurr_stopped", { name: asString(payload.name) }));
        refreshVms();
        return;
      }

      if (action === "restart") {
        setActionLoading("");
        toast.success(t("toast.dockurr_restarted", { name: asString(payload.name) }));
        refreshVms();
      }
    });

    socket.start(session);
    return () => {
      unsubscribeMessages();
      unsubscribeStatus();
      socket.stop();
    };
  }, [refreshVms, session, socket, t]);

  const loadStats = useCallback(async (name: string, showLoading = false) => {
    if (showLoading) {
      setStatsLoading(true);
    }
    const res = await getDockerContainerStats(session.token, name);
    if (showLoading) {
      setStatsLoading(false);
    }
    if (!res.ok || !res.stats) {
      return;
    }
    const tsMs = Date.now();
    setStats(res.stats);
    setCpuHistory((prev) => appendHistory(prev, tsMs, res.stats.cpu_percent));
    setMemoryHistory((prev) => appendHistory(prev, tsMs, res.stats.memory_percent));
  }, [session.token]);

  const loadProcesses = useCallback(async (name: string, showLoading = false) => {
    if (showLoading) {
      setProcessesLoading(true);
    }
    const res = await getDockerContainerProcesses(session.token, name, PROCESS_LIMIT);
    if (showLoading) {
      setProcessesLoading(false);
    }
    if (!res.ok) {
      if (showLoading) {
        toast.error(res.error ?? t("toast.request_failed"));
      }
      return;
    }
    setProcesses({
      name: res.name ?? name,
      columns: Array.isArray(res.columns) ? res.columns : [],
      rows: Array.isArray(res.rows) ? res.rows : [],
    });
  }, [session.token, t]);

  const loadInspect = useCallback(async (name: string) => {
    setInspectLoading(true);
    const res = await getDockerContainerInspect(session.token, name);
    setInspectLoading(false);
    if (!res.ok) {
      setInspectText(res.error ?? t("toast.request_failed"));
      return;
    }
    setInspectText(res.inspect ?? "");
  }, [session.token, t]);

  const loadLogs = useCallback(async (name: string, stream = false, showLoading = false) => {
    if (showLoading) {
      setLogsLoading(true);
    }
    const res = await getDockerContainerLogs(session.token, name, LOG_TAIL);
    if (showLoading) {
      setLogsLoading(false);
    }
    if (!res.ok) {
      if (!stream) {
        setLogsText(res.error ?? t("toast.request_failed"));
      }
      return;
    }
    const snapshot = typeof res.logs === "string" ? res.logs : "";
    setLogsText((previous) => {
      const merged = stream ? mergeLogText(previous, snapshot) : normalizeLogText(snapshot);
      const next = clampLogLines(merged, LOG_MAX_LINES);
      return next === previous ? previous : next;
    });
  }, [session.token, t]);

  const loadFiles = useCallback(async (targetPath: string, explicitName?: string) => {
    const vmName = explicitName ?? selectedVm?.name;
    if (!vmName) {
      return;
    }
    const normalized = normalizeContainerPath(targetPath);
    setFilesLoading(true);
    const res = await listDockerContainerFiles(session.token, vmName, normalized);
    setFilesLoading(false);
    if (!res.ok) {
      setFilesError(res.error ?? t("toast.request_failed"));
      return;
    }
    setFilesError("");
    setFilePath(normalizeContainerPath(res.current_path || normalized));
    setFileEntries(Array.isArray(res.entries) ? res.entries : []);
  }, [selectedVm?.name, session.token, t]);

  const openFile = useCallback(async (entry: DockerContainerFileEntry) => {
    if (!selectedVm) {
      return;
    }
    if (entry.is_directory) {
      await loadFiles(entry.path, selectedVm.name);
      return;
    }

    setFileLoading(true);
    const res = await readDockerContainerFile(session.token, selectedVm.name, entry.path);
    setFileLoading(false);
    if (!res.ok) {
      toast.error(res.error ?? t("toast.request_failed"));
      return;
    }

    const base64 = typeof res.content_base64 === "string" ? res.content_base64 : "";
    const path = typeof res.path === "string" ? res.path : entry.path;
    const fileName = basename(path) || entry.name;
    const ext = fileExtension(fileName);

    setSelectedFilePath(path);
    setSelectedPdfBytes(null);
    replaceImageUrl("");
    setSelectedFileText("");
    setMarkdownPreview(true);

    if (IMAGE_EXTENSIONS.has(ext)) {
      setSelectedFileMode("image");
      const bytes = base64ToBytes(base64);
      const url = URL.createObjectURL(new Blob([bytes], { type: containerMimeType(fileName) }));
      replaceImageUrl(url);
      return;
    }

    if (PDF_EXTENSIONS.has(ext)) {
      setSelectedFileMode("pdf");
      setSelectedPdfBytes(base64ToBytes(base64));
      return;
    }

    const decoded = decodeBase64Utf8(base64);
    if (MARKDOWN_EXTENSIONS.has(ext)) {
      setSelectedFileMode("markdown");
      setSelectedFileText(decoded);
      return;
    }

    setSelectedFileMode("text");
    setSelectedFileText(decoded);
  }, [loadFiles, replaceImageUrl, selectedVm, session.token, t]);

  const saveSelectedFile = useCallback(async () => {
    if (!selectedVm || !selectedFilePath) {
      toast.error(t("docker.file_select"));
      return;
    }
    if (selectedFileMode !== "text" && selectedFileMode !== "markdown") {
      return;
    }

    setFileSaving(true);
    const res = await writeDockerContainerFile(
      session.token,
      selectedVm.name,
      selectedFilePath,
      encodeBase64Utf8(selectedFileText)
    );
    setFileSaving(false);
    if (!res.ok) {
      toast.error(res.error ?? t("toast.request_failed"));
      return;
    }
    toast.success(t("docker.saved", { path: selectedFilePath }));
    await loadFiles(filePath, selectedVm.name);
  }, [filePath, loadFiles, selectedFileMode, selectedFilePath, selectedFileText, selectedVm, session.token, t]);

  const downloadSelectedFile = useCallback(async () => {
    if (!selectedVm || !selectedFilePath) {
      toast.error(t("docker.file_select"));
      return;
    }

    setFileDownloading(true);
    const res = await readDockerContainerFile(session.token, selectedVm.name, selectedFilePath);
    setFileDownloading(false);
    if (!res.ok) {
      toast.error(res.error ?? t("toast.request_failed"));
      return;
    }

    const fileName = basename(selectedFilePath) || "download.bin";
    const bytes = base64ToBytes(res.content_base64 ?? "");
    const url = URL.createObjectURL(new Blob([bytes], { type: containerMimeType(fileName) }));
    const anchor = document.createElement("a");
    anchor.href = url;
    anchor.download = fileName;
    anchor.click();
    URL.revokeObjectURL(url);
  }, [selectedFilePath, selectedVm, session.token, t]);

  const uploadContainerFile = useCallback(async (event: ChangeEvent<HTMLInputElement>) => {
    if (!selectedVm) {
      event.target.value = "";
      return;
    }
    const file = event.target.files?.[0];
    if (!file) {
      return;
    }

    setFileUploading(true);
    const base64 = await browserFileToBase64(file);
    const targetPath = joinPath(filePath, file.name);
    const res = await writeDockerContainerFile(session.token, selectedVm.name, targetPath, base64);
    setFileUploading(false);
    event.target.value = "";
    if (!res.ok) {
      toast.error(res.error ?? t("toast.request_failed"));
      return;
    }
    toast.success(t("docker.uploaded", { name: file.name }));
    await loadFiles(filePath, selectedVm.name);
  }, [filePath, loadFiles, selectedVm, session.token, t]);

  const refreshDetail = useCallback(() => {
    if (!selectedVm) {
      return;
    }
    if (detailTab === "load") {
      void loadStats(selectedVm.name, true);
      void loadProcesses(selectedVm.name, true);
      return;
    }
    if (detailTab === "inspect") {
      void loadInspect(selectedVm.name);
      return;
    }
    if (detailTab === "logs") {
      void loadLogs(selectedVm.name, false, true);
      return;
    }
    void loadFiles(filePath, selectedVm.name);
  }, [detailTab, filePath, loadFiles, loadInspect, loadLogs, loadProcesses, loadStats, selectedVm]);

  useEffect(() => {
    setStats(null);
    setCpuHistory([]);
    setMemoryHistory([]);
    setProcesses({ name: "", columns: [], rows: [] });
    if (detailTab !== "load" || !selectedVm) {
      return;
    }
    void loadStats(selectedVm.name, true);
    void loadProcesses(selectedVm.name, true);
    const statsTimer = window.setInterval(() => {
      void loadStats(selectedVm.name, false);
    }, STATS_REFRESH_MS);
    const processTimer = window.setInterval(() => {
      void loadProcesses(selectedVm.name, false);
    }, PROCESS_REFRESH_MS);
    return () => {
      window.clearInterval(statsTimer);
      window.clearInterval(processTimer);
    };
  }, [detailTab, loadProcesses, loadStats, selectedVm?.name]);

  useEffect(() => {
    setInspectText("");
    setLogsText("");
    setLogsLoading(false);
    setFilesError("");
    setFileEntries([]);
    setFilePath("/");
    resetSelectedFile();
    if (!selectedVm) {
      return;
    }
    if (detailTab === "inspect") {
      void loadInspect(selectedVm.name);
      return;
    }
    if (detailTab === "files") {
      void loadFiles("/", selectedVm.name);
    }
  }, [detailTab, loadFiles, loadInspect, resetSelectedFile, selectedVm?.name]);

  useEffect(() => {
    if (detailTab !== "logs" || !selectedVm) {
      return;
    }
    void loadLogs(selectedVm.name, false, true);
    const timer = window.setInterval(() => {
      void loadLogs(selectedVm.name, true, false);
    }, LOG_REFRESH_MS);
    return () => {
      window.clearInterval(timer);
    };
  }, [detailTab, loadLogs, selectedVm?.name]);

  const submitCreate = (event: FormEvent<HTMLFormElement>) => {
    event.preventDefault();
    if (createLoading) {
      return;
    }

    setCreateLoading(true);
    pendingCreateNameRef.current = name.trim();
    appendRuntimeLog("info", "create", t("dockurr.startup_submitted"), new Date().toISOString());
    const ram = `${clampInt(ramGb, RAM_MIN_GB, RAM_MAX_GB)}G`;
    const disk = `${clampInt(diskGb, DISK_MIN_GB, DISK_MAX_GB)}G`;
    const requestId = sendAction("create", {
      os,
      version,
      ram,
      disk,
      name: name.trim(),
      persist,
    });
    if (!requestId) {
      setCreateLoading(false);
      return;
    }
    currentCreateRequestIdRef.current = requestId;
  };

  const runStop = (vm: DockurrVmInfo) => {
    if (actionLoading) return;
    setActionLoading("stop");
    const requestId = sendAction("stop", { name: vm.name });
    if (!requestId) {
      setActionLoading("");
    }
  };

  const runRestart = (vm: DockurrVmInfo) => {
    if (actionLoading) return;
    setActionLoading("restart");
    const requestId = sendAction("restart", { name: vm.name });
    if (!requestId) {
      setActionLoading("");
    }
  };

  const openVm = (vm: DockurrVmInfo) => {
    if (!vm.novnc_port) {
      toast.error(t("dockurr.open_unavailable"));
      return;
    }
    const protocol = window.location.protocol === "https:" ? "https:" : "http:";
    const host = window.location.hostname || "localhost";
    window.open(`${protocol}//${host}:${vm.novnc_port}/`, "_blank", "noopener,noreferrer");
  };

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

  const cpuStyle = styleForLoad(stats?.cpu_percent);
  const memoryStyle = styleForLoad(stats?.memory_percent);

  return (
    <>
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

              <div className="flex items-center gap-2">
                <button
                  className="inline-flex h-10 items-center gap-2 rounded-2xl bg-slate-100 px-4 text-sm font-semibold text-slate-700 hover:bg-slate-200 dark:bg-neutral-800 dark:text-neutral-100 dark:hover:bg-neutral-700"
                  onClick={refreshVms}
                >
                  <FiRefreshCw className={cn(status.kind === "connecting" && "animate-spin")} />
                  {t("common.refresh")}
                </button>
                <button
                  className="inline-flex h-10 items-center gap-2 rounded-2xl bg-slate-900 px-4 text-sm font-semibold text-white shadow-sm hover:bg-slate-800 dark:bg-neutral-50 dark:text-neutral-900 dark:hover:bg-white"
                  onClick={() => setCreateDialogOpen(true)}
                >
                  <FiPlus />
                  {t("dockurr.create")}
                </button>
              </div>
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
                          actionLoading === "stop"
                            ? "cursor-wait bg-slate-200 text-slate-500 dark:bg-neutral-800 dark:text-neutral-500"
                            : "bg-rose-100 text-rose-800 hover:bg-rose-200 dark:bg-rose-900/40 dark:text-rose-300 dark:hover:bg-rose-900/60"
                        )}
                        onClick={(event) => {
                          event.stopPropagation();
                          runStop(vm);
                        }}
                        disabled={actionLoading.length > 0}
                      >
                        <FiSquare /> {t("dockurr.stop")}
                      </button>
                      <button
                        className={cn(
                          actionButtonClass,
                          actionLoading === "restart"
                            ? "cursor-wait bg-slate-200 text-slate-500 dark:bg-neutral-800 dark:text-neutral-500"
                            : "bg-amber-100 text-amber-900 hover:bg-amber-200 dark:bg-amber-900/40 dark:text-amber-300 dark:hover:bg-amber-900/60"
                        )}
                        onClick={(event) => {
                          event.stopPropagation();
                          runRestart(vm);
                        }}
                        disabled={actionLoading.length > 0}
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
                          setSelectedName(vm.name);
                          setDetailTab("logs");
                        }}
                      >
                        <FiActivity /> {t("dockurr.logs")}
                      </button>
                      <button
                        className={cn(
                          actionButtonClass,
                          "bg-slate-200 text-slate-800 hover:bg-slate-300 dark:bg-neutral-800 dark:text-neutral-100 dark:hover:bg-neutral-700"
                        )}
                        onClick={(event) => {
                          event.stopPropagation();
                          setSelectedName(vm.name);
                          setDetailTab("inspect");
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
            <div className="flex items-center justify-between gap-3">
              <h3 className="inline-flex items-center gap-2 text-sm font-semibold tracking-tight text-slate-900 dark:text-neutral-50">
                <FiActivity className="text-[14px]" />
                {t("dockurr.startup_logs")}
              </h3>
              <label className="inline-flex items-center gap-2 rounded-xl border border-slate-200 bg-white/80 px-2.5 py-1 text-[11px] font-semibold text-slate-600 shadow-sm dark:border-neutral-800 dark:bg-neutral-950/35 dark:text-neutral-300">
                <span>{t("dockurr.auto_scroll")}</span>
                <span
                  className={cn(
                    "rounded-full px-1.5 py-0.5 text-[10px]",
                    runtimeAutoScroll
                      ? "bg-emerald-100 text-emerald-700 dark:bg-emerald-900/35 dark:text-emerald-300"
                      : "bg-slate-200 text-slate-600 dark:bg-neutral-800 dark:text-neutral-300"
                  )}
                >
                  {runtimeAutoScroll ? t("common.on") : t("common.off")}
                </span>
                <span className="relative inline-flex h-5 w-9 shrink-0">
                  <input
                    type="checkbox"
                    checked={runtimeAutoScroll}
                    onChange={(event) => setRuntimeAutoScroll(event.target.checked)}
                    className="peer sr-only"
                  />
                  <span className="absolute inset-0 rounded-full bg-slate-300 transition-colors peer-checked:bg-slate-900 dark:bg-neutral-700 dark:peer-checked:bg-neutral-100" />
                  <span className="absolute left-0.5 top-0.5 h-4 w-4 rounded-full bg-white shadow-sm transition-transform peer-checked:translate-x-4 dark:bg-neutral-900 dark:peer-checked:bg-neutral-900" />
                </span>
              </label>
            </div>
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

        <section className="flex min-h-[320px] min-w-0 flex-col overflow-hidden rounded-3xl bg-white/75 p-4 shadow-soft ring-1 ring-slate-200/70 backdrop-blur xl:h-full dark:bg-neutral-900/60 dark:ring-neutral-800/80">
          <div className="flex flex-wrap items-start justify-between gap-2">
            <div>
              <h3 className="inline-flex items-center gap-2 text-sm font-semibold text-slate-900 dark:text-neutral-50">
                <FiActivity />
                {t("dockurr.detail")}
              </h3>
              <div className="mt-1 text-xs text-slate-500 dark:text-neutral-400">
                {selectedVm?.name || t("dockurr.select_hint")}
              </div>
            </div>
            {selectedVm ? (
              <div className="flex flex-wrap items-center gap-1.5">
                <button
                  className={cn(
                    "inline-flex h-8 items-center gap-1 rounded-xl px-2.5 text-xs font-semibold transition-colors",
                    actionLoading === "stop"
                      ? "cursor-wait bg-slate-200 text-slate-500 dark:bg-neutral-800 dark:text-neutral-500"
                      : "bg-rose-100 text-rose-800 hover:bg-rose-200 dark:bg-rose-900/40 dark:text-rose-300 dark:hover:bg-rose-900/60"
                  )}
                  onClick={() => runStop(selectedVm)}
                  disabled={actionLoading.length > 0}
                >
                  <FiSquare /> {t("dockurr.stop")}
                </button>
                <button
                  className={cn(
                    "inline-flex h-8 items-center gap-1 rounded-xl px-2.5 text-xs font-semibold transition-colors",
                    actionLoading === "restart"
                      ? "cursor-wait bg-slate-200 text-slate-500 dark:bg-neutral-800 dark:text-neutral-500"
                      : "bg-amber-100 text-amber-900 hover:bg-amber-200 dark:bg-amber-900/40 dark:text-amber-300 dark:hover:bg-amber-900/60"
                  )}
                  onClick={() => runRestart(selectedVm)}
                  disabled={actionLoading.length > 0}
                >
                  <FiRotateCw /> {t("dockurr.restart")}
                </button>
                <button
                  className="inline-flex h-8 items-center gap-1 rounded-xl bg-emerald-100 px-2.5 text-xs font-semibold text-emerald-800 transition-colors hover:bg-emerald-200 dark:bg-emerald-900/40 dark:text-emerald-300 dark:hover:bg-emerald-900/60"
                  onClick={() => openVm(selectedVm)}
                >
                  <FiEye /> {t("dockurr.open")}
                </button>
                <button
                  className="grid h-8 w-8 place-items-center rounded-xl bg-slate-100 text-slate-700 transition-colors hover:bg-slate-200 dark:bg-neutral-800 dark:text-neutral-100 dark:hover:bg-neutral-700"
                  onClick={refreshDetail}
                  title={t("common.refresh")}
                >
                  <FiRefreshCw />
                </button>
              </div>
            ) : null}
          </div>

          {selectedVm ? (
            <>
              <div className="mt-3 inline-flex rounded-2xl bg-slate-100 p-1 dark:bg-neutral-800">
                {([
                  ["load", t("docker.tab_load")],
                  ["inspect", t("docker.tab_inspect")],
                  ["logs", t("docker.tab_logs")],
                  ["files", t("docker.tab_files")],
                ] as Array<[DetailTab, string]>).map(([tab, label]) => (
                  <button
                    key={tab}
                    className={cn(
                      "rounded-xl px-3 py-1.5 text-xs font-semibold transition-colors",
                      detailTab === tab
                        ? "bg-white text-slate-900 shadow-sm ring-1 ring-slate-200 dark:bg-neutral-900 dark:text-neutral-50 dark:ring-neutral-700"
                        : "text-slate-600 hover:bg-slate-200 dark:text-neutral-300 dark:hover:bg-neutral-700"
                    )}
                    onClick={() => setDetailTab(tab)}
                  >
                    {label}
                  </button>
                ))}
              </div>

              {detailTab === "load" ? (
                <div className="mt-3 min-h-0 min-w-0 flex flex-1 flex-col gap-3 overflow-y-auto pr-1 [scrollbar-gutter:stable]">
                  <div className="shrink-0 grid gap-3 sm:grid-cols-2 xl:grid-cols-4">
                    <div className="rounded-2xl bg-slate-100/80 p-3 dark:bg-neutral-900">
                      <div className="text-xs text-slate-500 dark:text-neutral-400">{t("docker.cpu_load")}</div>
                      <div className="mt-1 text-xl font-semibold text-slate-900 dark:text-neutral-50">
                        {formatPercent(stats?.cpu_percent ?? null)}
                      </div>
                    </div>
                    <div className="rounded-2xl bg-slate-100/80 p-3 dark:bg-neutral-900">
                      <div className="text-xs text-slate-500 dark:text-neutral-400">{t("docker.memory_load")}</div>
                      <div className="mt-1 text-xl font-semibold text-slate-900 dark:text-neutral-50">
                        {formatPercent(stats?.memory_percent ?? null)}
                      </div>
                    </div>
                    <div className="rounded-2xl bg-slate-100/80 p-3 dark:bg-neutral-900">
                      <div className="text-xs text-slate-500 dark:text-neutral-400">{t("docker.net_io")}</div>
                      <div className="mt-1 text-xs font-semibold text-slate-900 dark:text-neutral-50">
                        {stats
                          ? `${formatBytes(stats.net_input_bytes)} / ${formatBytes(stats.net_output_bytes)}`
                          : "--"}
                      </div>
                    </div>
                    <div className="rounded-2xl bg-slate-100/80 p-3 dark:bg-neutral-900">
                      <div className="text-xs text-slate-500 dark:text-neutral-400">{t("docker.block_io")}</div>
                      <div className="mt-1 text-xs font-semibold text-slate-900 dark:text-neutral-50">
                        {stats
                          ? `${formatBytes(stats.block_input_bytes)} / ${formatBytes(stats.block_output_bytes)}`
                          : "--"}
                      </div>
                    </div>
                  </div>

                  <div className="shrink-0 grid gap-3 xl:grid-cols-2">
                    <div className="rounded-2xl bg-slate-100/60 p-3 dark:bg-neutral-900">
                      <div className="mb-1 text-xs text-slate-500 dark:text-neutral-400">{t("docker.cpu_trend")}</div>
                      <TrendChart points={cpuHistory} strokeColor={cpuStyle.strokeColor} fillColor={cpuStyle.fillColor} />
                    </div>
                    <div className="rounded-2xl bg-slate-100/60 p-3 dark:bg-neutral-900">
                      <div className="mb-1 text-xs text-slate-500 dark:text-neutral-400">{t("docker.memory_trend")}</div>
                      <TrendChart
                        points={memoryHistory}
                        strokeColor={memoryStyle.strokeColor}
                        fillColor={memoryStyle.fillColor}
                      />
                    </div>
                  </div>

                  <div className="shrink-0 text-xs text-slate-500 dark:text-neutral-400">
                    {statsLoading ? t("docker.loading") : `${t("docker.pids")}: ${stats?.pids ?? 0}`}
                  </div>

                  <div className="min-h-0 min-w-0 flex flex-1 flex-col rounded-2xl bg-slate-100/60 p-3 dark:bg-neutral-900">
                    <div className="mb-2 shrink-0 text-xs text-slate-500 dark:text-neutral-400">{t("docker.processes")}</div>
                    <div className="min-h-0 min-w-0 flex-1 overflow-x-auto overflow-y-auto rounded-xl border border-slate-200 bg-white/85 dark:border-neutral-800 dark:bg-neutral-950/45">
                      {processesLoading ? (
                        <div className="grid h-28 place-items-center text-xs text-slate-500 dark:text-neutral-400">
                          {t("docker.loading")}
                        </div>
                      ) : processes.columns.length === 0 || processes.rows.length === 0 ? (
                        <div className="grid h-28 place-items-center text-xs text-slate-500 dark:text-neutral-400">
                          {t("docker.processes_empty")}
                        </div>
                      ) : (
                        <table className="w-max min-w-full border-collapse text-left font-mono text-[11px] text-slate-800 dark:text-neutral-100">
                          <thead className="sticky top-0 z-10 bg-slate-100/95 text-[10px] text-slate-600 dark:bg-neutral-900/95 dark:text-neutral-300">
                            <tr>
                              {processes.columns.map((column) => (
                                <th
                                  key={column}
                                  className="whitespace-nowrap border-b border-slate-200 px-2 py-1.5 font-semibold dark:border-neutral-800"
                                >
                                  {column}
                                </th>
                              ))}
                            </tr>
                          </thead>
                          <tbody>
                            {processes.rows.map((row, rowIndex) => (
                              <tr
                                key={`process-row-${rowIndex}`}
                                className="even:bg-slate-50/70 dark:even:bg-neutral-900/40"
                              >
                                {processes.columns.map((_, colIndex) => (
                                  <td
                                    key={`process-cell-${rowIndex}-${colIndex}`}
                                    className="max-w-[340px] truncate border-b border-slate-100 px-2 py-1.5 dark:border-neutral-900"
                                  >
                                    {row[colIndex] ?? ""}
                                  </td>
                                ))}
                              </tr>
                            ))}
                          </tbody>
                        </table>
                      )}
                    </div>
                  </div>
                </div>
              ) : null}

              {detailTab === "inspect" ? (
                <pre className="mt-3 min-h-0 min-w-0 flex-1 overflow-auto whitespace-pre-wrap break-all rounded-2xl bg-neutral-950 p-3 font-mono text-[12px] leading-relaxed text-neutral-50">
                  {inspectLoading ? t("docker.loading") : inspectText || t("docker.inspect_empty")}
                </pre>
              ) : null}

              {detailTab === "logs" ? (
                <div className="mt-3 min-h-0 min-w-0 flex flex-1 flex-col">
                  <div className="relative min-h-0 min-w-0 flex-1">
                    <button
                      className={cn(
                        "absolute right-2 top-2 z-10 inline-flex h-8 items-center rounded-xl px-2.5 text-xs font-semibold transition-colors",
                        logsAutoScroll
                          ? "bg-slate-900 text-white dark:bg-neutral-50 dark:text-neutral-900"
                          : "bg-slate-100 text-slate-700 hover:bg-slate-200 dark:bg-neutral-800 dark:text-neutral-100 dark:hover:bg-neutral-700"
                      )}
                      onClick={() => setLogsAutoScroll((prev) => !prev)}
                    >
                      {t("docker.auto_scroll")}: {logsAutoScroll ? t("common.on") : t("common.off")}
                    </button>
                  </div>
                  <pre
                    ref={logsViewportRef}
                    className="h-full min-h-0 min-w-0 overflow-auto whitespace-pre-wrap break-all rounded-2xl bg-neutral-950 px-3 pb-3 pt-12 font-mono text-[12px] leading-relaxed text-neutral-50"
                  >
                    {logsLoading ? t("docker.loading") : logsText || t("docker.logs_empty")}
                  </pre>
                </div>
              ) : null}

              {detailTab === "files" ? (
                <div className="mt-3 min-h-0 min-w-0 flex flex-1 flex-col gap-3">
                  <div className="flex items-center gap-2">
                    <button
                      className="grid h-9 w-9 place-items-center rounded-xl bg-slate-100 text-slate-700 hover:bg-slate-200 disabled:opacity-50 dark:bg-neutral-800 dark:text-neutral-100 dark:hover:bg-neutral-700"
                      onClick={() => void loadFiles(parentContainerPath(filePath), selectedVm.name)}
                      disabled={filePath === "/"}
                      title={t("common.up")}
                    >
                      <FiChevronLeft />
                    </button>
                    <input
                      value={filePath}
                      onChange={(event) => setFilePath(event.target.value)}
                      onKeyDown={(event) => {
                        if (event.key === "Enter") {
                          void loadFiles(filePath, selectedVm.name);
                        }
                      }}
                      className="h-9 w-full rounded-xl border border-slate-200 bg-white px-3 text-xs text-slate-900 shadow-sm outline-none focus:border-slate-300 dark:border-neutral-800 dark:bg-neutral-950/40 dark:text-neutral-50 dark:focus:border-neutral-700"
                    />
                    <button
                      className="inline-flex h-9 items-center justify-center rounded-xl bg-slate-900 px-3 text-xs font-semibold text-white hover:bg-slate-800 dark:bg-neutral-50 dark:text-neutral-900 dark:hover:bg-white"
                      onClick={() => void loadFiles(filePath, selectedVm.name)}
                    >
                      {t("common.go")}
                    </button>
                    <button
                      className={cn(
                        "grid h-9 w-9 place-items-center rounded-xl bg-slate-100 text-slate-700 hover:bg-slate-200 dark:bg-neutral-800 dark:text-neutral-100 dark:hover:bg-neutral-700",
                        filesLoading && "animate-pulse"
                      )}
                      onClick={() => void loadFiles(filePath, selectedVm.name)}
                      title={t("common.refresh")}
                    >
                      <FiRefreshCw />
                    </button>
                    <button
                      className={cn(
                        "inline-flex h-9 items-center gap-1 rounded-xl px-2.5 text-xs font-semibold",
                        fileUploading
                          ? "cursor-wait bg-slate-200 text-slate-500 dark:bg-neutral-800 dark:text-neutral-500"
                          : "bg-sky-100 text-sky-800 hover:bg-sky-200 dark:bg-sky-900/40 dark:text-sky-300 dark:hover:bg-sky-900/60"
                      )}
                      onClick={() => uploadInputRef.current?.click()}
                      disabled={fileUploading}
                    >
                      <FiUpload /> {t("common.upload")}
                    </button>
                    <input
                      ref={uploadInputRef}
                      type="file"
                      className="hidden"
                      onChange={(event) => void uploadContainerFile(event)}
                    />
                  </div>

                  {filesError ? (
                    <div className="rounded-xl border border-rose-200 bg-rose-50 px-3 py-2 text-xs text-rose-700 dark:border-rose-900/40 dark:bg-rose-950/30 dark:text-rose-300">
                      {filesError}
                    </div>
                  ) : null}

                  <div className="grid min-h-0 min-w-0 flex-1 gap-3 xl:grid-cols-[0.9fr_1.1fr]">
                    <div className="min-h-0 min-w-0 space-y-1 overflow-auto rounded-2xl border border-slate-200 bg-white/85 p-2 dark:border-neutral-800 dark:bg-neutral-950/35">
                      {fileEntries.map((entry) => (
                        <button
                          key={entry.path}
                          className="flex w-full items-center justify-between gap-2 rounded-xl px-2 py-1.5 text-left text-xs text-slate-700 transition-colors hover:bg-slate-100 dark:text-neutral-200 dark:hover:bg-neutral-800/70"
                          onClick={() => void openFile(entry)}
                        >
                          <span className="flex min-w-0 items-center gap-1.5">
                            {entry.is_directory ? <FiFolder /> : <FiFileText />}
                            <span className="truncate">{entry.name}</span>
                          </span>
                          <span className="shrink-0 font-mono text-[10px] text-slate-500 dark:text-neutral-400">
                            {entry.is_directory ? "dir" : formatBytes(entry.size)}
                          </span>
                        </button>
                      ))}
                      {fileEntries.length === 0 ? (
                        <div className="grid h-full min-h-[140px] place-items-center text-xs text-slate-500 dark:text-neutral-400">
                          {filesLoading ? t("docker.loading") : t("docker.files_empty")}
                        </div>
                      ) : null}
                    </div>

                    <div className="flex min-h-0 min-w-0 flex-col rounded-2xl border border-slate-200 bg-white/85 p-2 dark:border-neutral-800 dark:bg-neutral-950/35">
                      <div className="flex items-center justify-between gap-2 pb-2">
                        <div
                          className="truncate font-mono text-[11px] text-slate-500 dark:text-neutral-400"
                          title={selectedFilePath}
                        >
                          {selectedFilePath || t("docker.file_select")}
                        </div>
                        <div className="flex items-center gap-1.5">
                          {selectedFileMode === "markdown" ? (
                            <button
                              className="inline-flex h-8 items-center rounded-xl bg-slate-100 px-2.5 text-xs font-semibold text-slate-700 hover:bg-slate-200 dark:bg-neutral-800 dark:text-neutral-100 dark:hover:bg-neutral-700"
                              onClick={() => setMarkdownPreview((prev) => !prev)}
                            >
                              {markdownPreview ? t("docker.edit") : t("docker.preview")}
                            </button>
                          ) : null}
                          <button
                            className="inline-flex h-8 items-center gap-1 rounded-xl bg-slate-100 px-2.5 text-xs font-semibold text-slate-700 hover:bg-slate-200 disabled:opacity-50 dark:bg-neutral-800 dark:text-neutral-100 dark:hover:bg-neutral-700"
                            onClick={() => void downloadSelectedFile()}
                            disabled={!selectedFilePath || fileDownloading}
                          >
                            <FiDownload /> {t("common.download")}
                          </button>
                          {(selectedFileMode === "text" || selectedFileMode === "markdown") ? (
                            <button
                              className="inline-flex h-8 items-center gap-1 rounded-xl bg-slate-900 px-3 text-xs font-semibold text-white hover:bg-slate-800 disabled:opacity-50 dark:bg-neutral-50 dark:text-neutral-900 dark:hover:bg-white"
                              onClick={() => void saveSelectedFile()}
                              disabled={!selectedFilePath || fileSaving}
                            >
                              <FiSave /> {t("common.save")}
                            </button>
                          ) : null}
                        </div>
                      </div>

                      {selectedFileMode === "none" ? (
                        <div className="grid min-h-0 flex-1 place-items-center rounded-xl border border-dashed border-slate-200 text-xs text-slate-500 dark:border-neutral-800 dark:text-neutral-400">
                          {fileLoading ? t("docker.loading") : t("docker.file_select")}
                        </div>
                      ) : selectedFileMode === "image" ? (
                        <ImagePreview url={selectedImageUrl} alt={basename(selectedFilePath)} loadingText={t("docker.loading")} />
                      ) : selectedFileMode === "pdf" ? (
                        <PdfPreview
                          bytes={selectedPdfBytes}
                          loadingText={t("docker.loading")}
                          onParseFailed={() => toast.error(t("files.preview_parse_failed"))}
                        />
                      ) : selectedFileMode === "markdown" && markdownPreview ? (
                        <MarkdownPreview content={selectedFileText} />
                      ) : (
                        <textarea
                          value={selectedFileText}
                          onChange={(event) => setSelectedFileText(event.target.value)}
                          className="min-h-0 flex-1 resize-none rounded-xl border border-slate-200 bg-white p-2 font-mono text-[12px] text-slate-900 outline-none focus:border-slate-300 dark:border-neutral-800 dark:bg-neutral-950 dark:text-neutral-100 dark:focus:border-neutral-700"
                          placeholder={fileLoading ? t("docker.loading") : t("docker.file_select")}
                        />
                      )}
                    </div>
                  </div>
                </div>
              ) : null}
            </>
          ) : (
            <div className="mt-4 rounded-2xl border border-dashed border-slate-200 p-6 text-center text-sm text-slate-500 dark:border-neutral-800 dark:text-neutral-400">
              {t("dockurr.select_hint")}
            </div>
          )}
        </section>
      </div>

      {createDialogOpen ? (
        <div
          className="fixed inset-0 z-[120] flex items-center justify-center bg-slate-900/45 p-4 backdrop-blur-[2px]"
          onClick={() => {
            if (!createLoading) {
              setCreateDialogOpen(false);
            }
          }}
        >
          <section
            className="flex max-h-[92dvh] w-full max-w-3xl min-w-0 flex-col rounded-3xl bg-white/95 p-4 shadow-2xl ring-1 ring-slate-200/80 dark:bg-neutral-900/95 dark:ring-neutral-700"
            onClick={(event) => event.stopPropagation()}
          >
            <div className="flex items-center justify-between gap-3">
              <h3 className="inline-flex items-center gap-2 text-sm font-semibold tracking-tight text-slate-900 dark:text-neutral-50">
                <FiPlus className="text-[14px]" />
                {t("dockurr.create")}
              </h3>
              <button
                type="button"
                className={cn(
                  "inline-flex h-9 items-center gap-1.5 rounded-xl px-3 text-xs font-semibold transition-colors",
                  createLoading
                    ? "cursor-not-allowed bg-slate-200 text-slate-500 dark:bg-neutral-800 dark:text-neutral-500"
                    : "bg-slate-100 text-slate-700 hover:bg-slate-200 dark:bg-neutral-800 dark:text-neutral-100 dark:hover:bg-neutral-700"
                )}
                disabled={createLoading}
                onClick={() => setCreateDialogOpen(false)}
              >
                <FiX />
                {t("common.close")}
              </button>
            </div>

            <form className="mt-4 flex min-h-0 flex-1 flex-col" onSubmit={submitCreate}>
              <div className="min-h-0 flex-1 space-y-4 overflow-auto pr-1">
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
                    <div className="mt-1 flex items-center gap-2">
                      <input
                        type="range"
                        min={RAM_MIN_GB}
                        max={RAM_MAX_GB}
                        step={1}
                        value={ramGb}
                        onChange={(event) =>
                          setRamGb(clampInt(event.currentTarget.valueAsNumber, RAM_MIN_GB, RAM_MAX_GB))
                        }
                        className="h-2 w-full cursor-pointer accent-slate-900 dark:accent-neutral-200"
                      />
                      <div className="relative w-24 shrink-0">
                        <input
                          type="number"
                          min={RAM_MIN_GB}
                          max={RAM_MAX_GB}
                          step={1}
                          value={ramGb}
                          onChange={(event) =>
                            setRamGb(clampInt(event.currentTarget.valueAsNumber, RAM_MIN_GB, RAM_MAX_GB))
                          }
                          className="w-full rounded-2xl border border-slate-200 bg-white py-2 pl-3 pr-9 text-sm text-slate-900 shadow-sm outline-none focus:border-slate-300 dark:border-neutral-800 dark:bg-neutral-950/40 dark:text-neutral-50 dark:focus:border-neutral-700"
                        />
                        <span className="pointer-events-none absolute right-3 top-1/2 -translate-y-1/2 text-[11px] text-slate-500 dark:text-neutral-400">
                          GB
                        </span>
                      </div>
                    </div>
                    <div className="mt-1 flex items-center justify-between text-[11px] text-slate-500 dark:text-neutral-400">
                      <span>{RAM_MIN_GB} GB</span>
                      <span>{RAM_MAX_GB} GB</span>
                    </div>
                  </label>

                  <label className="text-xs font-semibold text-slate-600 dark:text-neutral-300">
                    {t("dockurr.disk")}
                    <div className="mt-1 flex items-center gap-2">
                      <input
                        type="range"
                        min={DISK_MIN_GB}
                        max={DISK_MAX_GB}
                        step={1}
                        value={diskGb}
                        onChange={(event) =>
                          setDiskGb(clampInt(event.currentTarget.valueAsNumber, DISK_MIN_GB, DISK_MAX_GB))
                        }
                        className="h-2 w-full cursor-pointer accent-slate-900 dark:accent-neutral-200"
                      />
                      <div className="relative w-24 shrink-0">
                        <input
                          type="number"
                          min={DISK_MIN_GB}
                          max={DISK_MAX_GB}
                          step={1}
                          value={diskGb}
                          onChange={(event) =>
                            setDiskGb(clampInt(event.currentTarget.valueAsNumber, DISK_MIN_GB, DISK_MAX_GB))
                          }
                          className="w-full rounded-2xl border border-slate-200 bg-white py-2 pl-3 pr-9 text-sm text-slate-900 shadow-sm outline-none focus:border-slate-300 dark:border-neutral-800 dark:bg-neutral-950/40 dark:text-neutral-50 dark:focus:border-neutral-700"
                        />
                        <span className="pointer-events-none absolute right-3 top-1/2 -translate-y-1/2 text-[11px] text-slate-500 dark:text-neutral-400">
                          GB
                        </span>
                      </div>
                    </div>
                    <div className="mt-1 flex items-center justify-between text-[11px] text-slate-500 dark:text-neutral-400">
                      <span>{DISK_MIN_GB} GB</span>
                      <span>{DISK_MAX_GB} GB</span>
                    </div>
                  </label>
                </div>

                <div className="grid grid-cols-1 gap-3 sm:grid-cols-2">
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
              </div>

              <div className="mt-4 flex flex-col gap-3 border-t border-slate-200/80 pt-4 sm:flex-row sm:items-center sm:justify-between dark:border-neutral-800/80">
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
                  <FiPlus /> {createLoading ? t("dockurr.creating") : t("dockurr.launch")}
                </button>
              </div>
            </form>
          </section>
        </div>
      ) : null}
    </>
  );
}
