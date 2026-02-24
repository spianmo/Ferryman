import { useCallback, useEffect, useMemo, useRef, useState, type ChangeEvent } from "react";
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
  FiBox,
  FiChevronLeft,
  FiDownload,
  FiFileText,
  FiFolder,
  FiPlay,
  FiRefreshCw,
  FiRotateCw,
  FiSave,
  FiSquare,
  FiUpload,
} from "react-icons/fi";

import {
  getDockerContainerInspect,
  getDockerContainerLogs,
  getDockerContainerProcesses,
  getDockerContainerStats,
  listDockerContainerFiles,
  listDockerContainers,
  readDockerContainerFile,
  restartDockerContainer,
  startDockerContainer,
  stopDockerContainer,
  writeDockerContainerFile,
} from "../api/client";
import ImagePreview from "../components/files/preview/ImagePreview";
import MarkdownPreview from "../components/files/preview/MarkdownPreview";
import PdfPreview from "../components/files/preview/PdfPreview";
import { useI18n } from "../i18n";
import { toast } from "../toast";
import type {
  DockerContainerFileEntry,
  DockerContainerInfo,
  DockerContainerProcesses,
  DockerContainerStats,
  SessionInfo,
} from "../types";
import { decodeBase64Utf8, encodeBase64Utf8 } from "../util/codec";
import { cn } from "../util/cn";

type Props = {
  session: SessionInfo;
};

type DetailTab = "load" | "inspect" | "logs" | "files";
type FileMode = "none" | "text" | "markdown" | "image" | "pdf";
type TrendPoint = {
  tsMs: number;
  value: number | null;
};

type LoadStyle = {
  strokeColor: string;
  fillColor: string;
  barClass: string;
};

const HISTORY_LIMIT = 90;
const LIST_REFRESH_MS = 4200;
const STATS_REFRESH_MS = 2200;
const PROCESS_REFRESH_MS = 3000;
const LOG_REFRESH_MS = 1500;
const LOG_TAIL = 600;
const LOG_MAX_LINES = 5000;
const PROCESS_LIMIT = 120;

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

function stateDotClass(state: string) {
  const normalized = state.trim().toLowerCase();
  if (normalized === "running") return "bg-emerald-500";
  if (normalized === "paused") return "bg-amber-500";
  if (normalized === "restarting") return "bg-sky-500";
  if (normalized === "created") return "bg-slate-500";
  return "bg-rose-500";
}

function isRunning(state: string) {
  return state.trim().toLowerCase() === "running";
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

export default function DockerPage({ session }: Props) {
  const { t } = useI18n();
  const uploadInputRef = useRef<HTMLInputElement | null>(null);
  const logsViewportRef = useRef<HTMLPreElement | null>(null);

  const [includeAll, setIncludeAll] = useState(true);
  const [containers, setContainers] = useState<DockerContainerInfo[]>([]);
  const [selectedName, setSelectedName] = useState("");
  const [listLoading, setListLoading] = useState(false);
  const [listError, setListError] = useState("");

  const [detailTab, setDetailTab] = useState<DetailTab>("load");
  const [actionLoading, setActionLoading] = useState<"" | "start" | "stop" | "restart">("");

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

  const selectedContainer = useMemo(
    () => containers.find((container) => container.name === selectedName) ?? null,
    [containers, selectedName]
  );

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

  const refreshContainers = useCallback(async (showLoading = true) => {
    if (showLoading) {
      setListLoading(true);
    }
    const res = await listDockerContainers(session.token, includeAll);
    if (showLoading) {
      setListLoading(false);
    }
    if (!res.ok) {
      setListError(res.error ?? t("toast.request_failed"));
      return;
    }
    const next = Array.isArray(res.containers) ? res.containers : [];
    setContainers(next);
    setListError("");
    setSelectedName((prev) => {
      if (prev && next.some((item) => item.name === prev)) {
        return prev;
      }
      return next[0]?.name ?? "";
    });
  }, [includeAll, session.token, t]);

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

  const loadFiles = useCallback(async (targetPath: string, explicitContainerName?: string) => {
    const containerName = explicitContainerName ?? selectedContainer?.name;
    if (!containerName) {
      return;
    }
    const normalized = normalizeContainerPath(targetPath);
    setFilesLoading(true);
    const res = await listDockerContainerFiles(session.token, containerName, normalized);
    setFilesLoading(false);
    if (!res.ok) {
      setFilesError(res.error ?? t("toast.request_failed"));
      return;
    }
    setFilesError("");
    setFilePath(normalizeContainerPath(res.current_path || normalized));
    setFileEntries(Array.isArray(res.entries) ? res.entries : []);
  }, [selectedContainer?.name, session.token, t]);

  const refreshDetail = useCallback(() => {
    if (!selectedContainer) {
      return;
    }
    if (detailTab === "load") {
      if (isRunning(selectedContainer.state)) {
        void loadStats(selectedContainer.name, true);
        void loadProcesses(selectedContainer.name, true);
      }
      return;
    }
    if (detailTab === "inspect") {
      void loadInspect(selectedContainer.name);
      return;
    }
    if (detailTab === "logs") {
      void loadLogs(selectedContainer.name, false, true);
      return;
    }
    void loadFiles(filePath, selectedContainer.name);
  }, [detailTab, filePath, loadFiles, loadInspect, loadLogs, loadProcesses, loadStats, selectedContainer]);

  const runAction = useCallback(async (action: "start" | "stop" | "restart") => {
    if (!selectedContainer) {
      return;
    }
    setActionLoading(action);
    const res =
      action === "start"
        ? await startDockerContainer(session.token, selectedContainer.name)
        : action === "stop"
          ? await stopDockerContainer(session.token, selectedContainer.name)
          : await restartDockerContainer(session.token, selectedContainer.name);
    setActionLoading("");
    if (!res.ok) {
      toast.error(res.error ?? t("toast.request_failed"));
      return;
    }
    if (action === "start") {
      toast.success(t("toast.docker_started", { name: selectedContainer.name }));
    } else if (action === "stop") {
      toast.success(t("toast.docker_stopped", { name: selectedContainer.name }));
    } else {
      toast.success(t("toast.docker_restarted", { name: selectedContainer.name }));
    }
    await refreshContainers(false);
    refreshDetail();
  }, [refreshContainers, refreshDetail, selectedContainer, session.token, t]);

  const openFile = useCallback(async (entry: DockerContainerFileEntry) => {
    if (!selectedContainer) {
      return;
    }
    if (entry.is_directory) {
      await loadFiles(entry.path, selectedContainer.name);
      return;
    }

    setFileLoading(true);
    const res = await readDockerContainerFile(session.token, selectedContainer.name, entry.path);
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
  }, [loadFiles, replaceImageUrl, selectedContainer, session.token, t]);

  const saveSelectedFile = useCallback(async () => {
    if (!selectedContainer || !selectedFilePath) {
      toast.error(t("docker.file_select"));
      return;
    }
    if (selectedFileMode !== "text" && selectedFileMode !== "markdown") {
      return;
    }
    setFileSaving(true);
    const res = await writeDockerContainerFile(
      session.token,
      selectedContainer.name,
      selectedFilePath,
      encodeBase64Utf8(selectedFileText)
    );
    setFileSaving(false);
    if (!res.ok) {
      toast.error(res.error ?? t("toast.request_failed"));
      return;
    }
    toast.success(t("docker.saved", { path: selectedFilePath }));
    await loadFiles(filePath, selectedContainer.name);
  }, [filePath, loadFiles, selectedContainer, selectedFileMode, selectedFilePath, selectedFileText, session.token, t]);

  const downloadSelectedFile = useCallback(async () => {
    if (!selectedContainer || !selectedFilePath) {
      toast.error(t("docker.file_select"));
      return;
    }
    setFileDownloading(true);
    const res = await readDockerContainerFile(session.token, selectedContainer.name, selectedFilePath);
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
  }, [selectedContainer, selectedFilePath, session.token, t]);

  const uploadContainerFile = useCallback(async (event: ChangeEvent<HTMLInputElement>) => {
    if (!selectedContainer) {
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
    const res = await writeDockerContainerFile(session.token, selectedContainer.name, targetPath, base64);
    setFileUploading(false);
    event.target.value = "";
    if (!res.ok) {
      toast.error(res.error ?? t("toast.request_failed"));
      return;
    }
    toast.success(t("docker.uploaded", { name: file.name }));
    await loadFiles(filePath, selectedContainer.name);
  }, [filePath, loadFiles, selectedContainer, session.token, t]);

  useEffect(() => {
    void refreshContainers(true);
    const timer = window.setInterval(() => {
      void refreshContainers(false);
    }, LIST_REFRESH_MS);
    return () => {
      window.clearInterval(timer);
    };
  }, [refreshContainers]);

  useEffect(() => {
    setStats(null);
    setCpuHistory([]);
    setMemoryHistory([]);
    setProcesses({ name: "", columns: [], rows: [] });
    if (detailTab !== "load" || !selectedContainer || !isRunning(selectedContainer.state)) {
      return;
    }
    void loadStats(selectedContainer.name, true);
    void loadProcesses(selectedContainer.name, true);
    const statsTimer = window.setInterval(() => {
      void loadStats(selectedContainer.name, false);
    }, STATS_REFRESH_MS);
    const processTimer = window.setInterval(() => {
      void loadProcesses(selectedContainer.name, false);
    }, PROCESS_REFRESH_MS);
    return () => {
      window.clearInterval(statsTimer);
      window.clearInterval(processTimer);
    };
  }, [detailTab, loadProcesses, loadStats, selectedContainer?.name, selectedContainer?.state]);

  useEffect(() => {
    setInspectText("");
    setLogsText("");
    setLogsLoading(false);
    setFilesError("");
    setProcesses({ name: "", columns: [], rows: [] });
    setFileEntries([]);
    setFilePath("/");
    resetSelectedFile();
    if (!selectedContainer) {
      return;
    }
    if (detailTab === "inspect") {
      void loadInspect(selectedContainer.name);
      return;
    }
    if (detailTab === "files") {
      void loadFiles("/", selectedContainer.name);
    }
  }, [detailTab, loadFiles, loadInspect, resetSelectedFile, selectedContainer?.name]);

  useEffect(() => {
    if (detailTab !== "logs" || !selectedContainer) {
      return;
    }
    void loadLogs(selectedContainer.name, false, true);
    const timer = window.setInterval(() => {
      void loadLogs(selectedContainer.name, true, false);
    }, LOG_REFRESH_MS);
    return () => {
      window.clearInterval(timer);
    };
  }, [detailTab, loadLogs, selectedContainer?.name]);

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

  const cpuStyle = styleForLoad(stats?.cpu_percent);
  const memoryStyle = styleForLoad(stats?.memory_percent);

  return (
    <div className="grid min-h-0 grid-cols-1 gap-4 xl:h-full xl:min-h-[520px] xl:grid-cols-[1.05fr_1.5fr]">
      <section className="flex min-h-[320px] flex-col rounded-3xl bg-white/75 p-4 shadow-soft ring-1 ring-slate-200/70 backdrop-blur xl:h-full dark:bg-neutral-900/60 dark:ring-neutral-800/80">
        <div className="flex items-center justify-between gap-2">
          <div className="inline-flex items-center gap-2 text-sm font-semibold text-slate-900 dark:text-neutral-50">
            <FiBox />
            <span>{t("docker.title")}</span>
          </div>
          <div className="flex items-center gap-2">
            <button
              className={cn(
                "inline-flex h-8 items-center rounded-xl px-3 text-xs font-semibold transition-colors",
                includeAll
                  ? "bg-slate-900 text-white dark:bg-neutral-50 dark:text-neutral-900"
                  : "bg-slate-100 text-slate-700 hover:bg-slate-200 dark:bg-neutral-800 dark:text-neutral-100 dark:hover:bg-neutral-700"
              )}
              onClick={() => setIncludeAll((prev) => !prev)}
            >
              {includeAll ? t("docker.filter_all") : t("docker.filter_running")}
            </button>
            <button
              className={cn(
                "grid h-8 w-8 place-items-center rounded-xl bg-slate-100 text-slate-700 transition-colors hover:bg-slate-200 dark:bg-neutral-800 dark:text-neutral-100 dark:hover:bg-neutral-700",
                listLoading && "animate-pulse"
              )}
              onClick={() => void refreshContainers(true)}
              title={t("common.refresh")}
            >
              <FiRefreshCw />
            </button>
          </div>
        </div>

        {listError ? (
          <div className="mt-3 rounded-xl border border-rose-200 bg-rose-50 px-3 py-2 text-xs text-rose-700 dark:border-rose-900/40 dark:bg-rose-950/30 dark:text-rose-300">
            {listError}
          </div>
        ) : null}

        <div className="mt-3 min-h-0 flex-1 space-y-2 overflow-y-auto overflow-x-hidden py-1.5 [scrollbar-gutter:stable]">
          {containers.map((container) => {
            const selected = selectedName === container.name;
            return (
              <button
                key={container.id || container.name}
                className={cn(
                  "block w-full rounded-2xl border border-transparent bg-white/70 px-3 py-2.5 text-left ring-1 ring-inset ring-slate-200/60 transition-colors dark:bg-neutral-950/35 dark:ring-neutral-800/70",
                  selected
                    ? "ring-2 ring-slate-900/70 dark:ring-neutral-50/70"
                    : "hover:bg-slate-50 dark:hover:bg-neutral-900/50"
                )}
                onClick={() => setSelectedName(container.name)}
              >
                <div className="flex items-center justify-between gap-2">
                  <div className="truncate text-sm font-semibold text-slate-900 dark:text-neutral-50">
                    {container.name}
                  </div>
                  <span className="inline-flex items-center gap-1 rounded-full bg-slate-100 px-2 py-0.5 text-[11px] font-semibold text-slate-700 dark:bg-neutral-800 dark:text-neutral-200">
                    <span className={cn("h-2 w-2 rounded-full", stateDotClass(container.state))} />
                    {container.state || "--"}
                  </span>
                </div>
                <div className="mt-1 truncate text-xs text-slate-500 dark:text-neutral-400">
                  {container.image || "--"}
                </div>
                <div className="mt-2 truncate text-[11px] text-slate-600 dark:text-neutral-300">
                  {t("docker.ports")}: <span className="font-mono">{container.ports || "-"}</span>
                </div>
                <div className="mt-1 truncate text-[11px] text-slate-500 dark:text-neutral-400">
                  {container.status || container.running_for || "--"}
                </div>
              </button>
            );
          })}
          {containers.length === 0 ? (
            <div className="rounded-2xl border border-dashed border-slate-200 p-5 text-center text-sm text-slate-500 dark:border-neutral-800 dark:text-neutral-400">
              {t("docker.empty")}
            </div>
          ) : null}
        </div>
      </section>

      <section className="flex min-h-[320px] min-w-0 flex-col overflow-hidden rounded-3xl bg-white/75 p-4 shadow-soft ring-1 ring-slate-200/70 backdrop-blur xl:h-full dark:bg-neutral-900/60 dark:ring-neutral-800/80">
        <div className="flex flex-wrap items-start justify-between gap-2">
          <div>
            <h3 className="inline-flex items-center gap-2 text-sm font-semibold text-slate-900 dark:text-neutral-50">
              <FiActivity />
              {t("docker.detail")}
            </h3>
            <div className="mt-1 text-xs text-slate-500 dark:text-neutral-400">
              {selectedContainer?.name || t("docker.select_hint")}
            </div>
          </div>
          {selectedContainer ? (
            <div className="flex flex-wrap items-center gap-1.5">
              <button
                className={cn(
                  "inline-flex h-8 items-center gap-1 rounded-xl px-2.5 text-xs font-semibold transition-colors",
                  actionLoading === "start"
                    ? "cursor-wait bg-slate-200 text-slate-500 dark:bg-neutral-800 dark:text-neutral-500"
                    : "bg-emerald-100 text-emerald-800 hover:bg-emerald-200 dark:bg-emerald-900/40 dark:text-emerald-300 dark:hover:bg-emerald-900/60"
                )}
                onClick={() => void runAction("start")}
                disabled={actionLoading.length > 0}
              >
                <FiPlay /> {t("docker.start")}
              </button>
              <button
                className={cn(
                  "inline-flex h-8 items-center gap-1 rounded-xl px-2.5 text-xs font-semibold transition-colors",
                  actionLoading === "stop"
                    ? "cursor-wait bg-slate-200 text-slate-500 dark:bg-neutral-800 dark:text-neutral-500"
                    : "bg-rose-100 text-rose-800 hover:bg-rose-200 dark:bg-rose-900/40 dark:text-rose-300 dark:hover:bg-rose-900/60"
                )}
                onClick={() => void runAction("stop")}
                disabled={actionLoading.length > 0}
              >
                <FiSquare /> {t("docker.stop")}
              </button>
              <button
                className={cn(
                  "inline-flex h-8 items-center gap-1 rounded-xl px-2.5 text-xs font-semibold transition-colors",
                  actionLoading === "restart"
                    ? "cursor-wait bg-slate-200 text-slate-500 dark:bg-neutral-800 dark:text-neutral-500"
                    : "bg-amber-100 text-amber-900 hover:bg-amber-200 dark:bg-amber-900/40 dark:text-amber-300 dark:hover:bg-amber-900/60"
                )}
                onClick={() => void runAction("restart")}
                disabled={actionLoading.length > 0}
              >
                <FiRotateCw /> {t("docker.restart")}
              </button>
              <button
                className="grid h-8 w-8 place-items-center rounded-xl bg-slate-100 text-slate-700 transition-colors hover:bg-slate-200 dark:bg-neutral-800 dark:text-neutral-100 dark:hover:bg-neutral-700"
                onClick={() => refreshDetail()}
                title={t("common.refresh")}
              >
                <FiRefreshCw />
              </button>
            </div>
          ) : null}
        </div>

        {selectedContainer ? (
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
              <div className="mt-3 min-h-0 min-w-0 flex-1 space-y-3 overflow-y-auto pr-1 [scrollbar-gutter:stable]">
                {isRunning(selectedContainer.state) ? (
                  <>
                    <div className="grid gap-3 sm:grid-cols-2 xl:grid-cols-4">
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

                    <div className="grid gap-3 xl:grid-cols-2">
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
                    <div className="text-xs text-slate-500 dark:text-neutral-400">
                      {statsLoading ? t("docker.loading") : `${t("docker.pids")}: ${stats?.pids ?? 0}`}
                    </div>

                    <div className="min-w-0 rounded-2xl bg-slate-100/60 p-3 dark:bg-neutral-900">
                      <div className="mb-2 text-xs text-slate-500 dark:text-neutral-400">{t("docker.processes")}</div>
                      <div className="h-[260px] min-w-0 overflow-x-auto overflow-y-auto rounded-xl border border-slate-200 bg-white/85 dark:border-neutral-800 dark:bg-neutral-950/45">
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
                                  <th key={column} className="whitespace-nowrap border-b border-slate-200 px-2 py-1.5 font-semibold dark:border-neutral-800">
                                    {column}
                                  </th>
                                ))}
                              </tr>
                            </thead>
                            <tbody>
                              {processes.rows.map((row, rowIndex) => (
                                <tr key={`process-row-${rowIndex}`} className="even:bg-slate-50/70 dark:even:bg-neutral-900/40">
                                  {processes.columns.map((_, colIndex) => (
                                    <td key={`process-cell-${rowIndex}-${colIndex}`} className="max-w-[340px] truncate border-b border-slate-100 px-2 py-1.5 dark:border-neutral-900">
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
                  </>
                ) : (
                  <div className="rounded-2xl border border-dashed border-slate-200 p-5 text-center text-sm text-slate-500 dark:border-neutral-800 dark:text-neutral-400">
                    {t("docker.not_running")}
                  </div>
                )}
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
                    onClick={() => void loadFiles(parentContainerPath(filePath), selectedContainer.name)}
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
                        void loadFiles(filePath, selectedContainer.name);
                      }
                    }}
                    className="h-9 w-full rounded-xl border border-slate-200 bg-white px-3 text-xs text-slate-900 shadow-sm outline-none focus:border-slate-300 dark:border-neutral-800 dark:bg-neutral-950/40 dark:text-neutral-50 dark:focus:border-neutral-700"
                  />
                  <button
                    className="inline-flex h-9 items-center justify-center rounded-xl bg-slate-900 px-3 text-xs font-semibold text-white hover:bg-slate-800 dark:bg-neutral-50 dark:text-neutral-900 dark:hover:bg-white"
                    onClick={() => void loadFiles(filePath, selectedContainer.name)}
                  >
                    {t("common.go")}
                  </button>
                  <button
                    className={cn(
                      "grid h-9 w-9 place-items-center rounded-xl bg-slate-100 text-slate-700 hover:bg-slate-200 dark:bg-neutral-800 dark:text-neutral-100 dark:hover:bg-neutral-700",
                      filesLoading && "animate-pulse"
                    )}
                    onClick={() => void loadFiles(filePath, selectedContainer.name)}
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
            {t("docker.select_hint")}
          </div>
        )}
      </section>
    </div>
  );
}
