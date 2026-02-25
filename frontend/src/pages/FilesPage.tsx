import { useEffect, useMemo, useRef, useState } from "react";
import { toast } from "../toast";
import {
  FiArchive,
  FiChevronLeft,
  FiCode,
  FiDatabase,
  FiDownload,
  FiFilm,
  FiFileText,
  FiFolder,
  FiGitBranch,
  FiGrid,
  FiImage,
  FiList,
  FiMusic,
  FiPackage,
  FiRefreshCw,
  FiSave,
  FiUpload,
  FiX,
} from "react-icons/fi";
import * as mammoth from "mammoth";
import * as XLSX from "xlsx";
import JSZip from "jszip";

import { listFiles, readFile, writeFile } from "../api/client";
import AudioPreview from "../components/files/preview/AudioPreview";
import DocxPreview from "../components/files/preview/DocxPreview";
import EditorPreview from "../components/files/preview/EditorPreview";
import HtmlPreview from "../components/files/preview/HtmlPreview";
import ImagePreview from "../components/files/preview/ImagePreview";
import MdEditorPanel from "../components/files/preview/MdEditorPanel";
import PdfPreview from "../components/files/preview/PdfPreview";
import PresentationPreview from "../components/files/preview/PresentationPreview";
import PptxPreview from "../components/files/preview/PptxPreview";
import SpreadsheetPreview from "../components/files/preview/SpreadsheetPreview";
import type { SpreadsheetPreviewData } from "../components/files/preview/types";
import VideoPreview from "../components/files/preview/VideoPreview";
import WordPreview from "../components/files/preview/WordPreview";
import XmindPreview from "../components/files/preview/XmindPreview";
import { useI18n } from "../i18n";
import { useTheme } from "../theme";
import { decodeBase64Utf8, encodeBase64Utf8 } from "../util/codec";
import { cn } from "../util/cn";
import type { FileEntry } from "../types";

type Props = {
  token: string;
  query: string;
};

type FilesViewMode = "list" | "grid";
type FileOpenMode =
  | "editor"
  | "image"
  | "video"
  | "audio"
  | "pdf"
  | "word"
  | "spreadsheet"
  | "presentation"
  | "xmind"
  | "unsupported";

const MARKDOWN_PREVIEW_THEMES = [
  "default",
  "github",
  "vuepress",
  "mk-cute",
  "smart-blue",
  "cyanosis",
  "arknights",
] as const;

type MarkdownPreviewTheme = (typeof MARKDOWN_PREVIEW_THEMES)[number];

const MARKDOWN_CODE_THEMES = [
  "atom",
  "a11y",
  "github",
  "gradient",
  "kimbie",
  "paraiso",
  "qtcreator",
  "stackoverflow",
] as const;

type MarkdownCodeTheme = (typeof MARKDOWN_CODE_THEMES)[number];

const FILES_VIEW_MODE_KEY = "ferryman.files.view_mode";
const FILES_PATH_KEY = "ferryman.files.path";
const FILES_MARKDOWN_PREVIEW_THEME_KEY = "ferryman.files.markdown.preview_theme";
const FILES_MARKDOWN_CODE_THEME_KEY = "ferryman.files.markdown.code_theme";

const IMAGE_EXTENSIONS = new Set(["png", "jpg", "jpeg", "gif", "webp", "bmp", "svg", "ico", "tif", "tiff"]);
const VIDEO_EXTENSIONS = new Set(["mp4", "mkv", "mov", "avi", "webm", "wmv", "m4v"]);
const AUDIO_EXTENSIONS = new Set(["mp3", "wav", "flac", "aac", "ogg", "m4a"]);
const PDF_EXTENSIONS = new Set(["pdf"]);
const MARKDOWN_EXTENSIONS = new Set(["md", "markdown"]);
const WORD_EXTENSIONS = new Set(["doc", "docx", "dox"]);
const PRESENTATION_EXTENSIONS = new Set(["ppt", "pptx"]);
const SPREADSHEET_EXTENSIONS = new Set(["xls", "xlsx", "csv"]);
const XMIND_EXTENSIONS = new Set(["xmind"]);
const ARCHIVE_EXTENSIONS = new Set([
  "zip",
  "rar",
  "7z",
  "tar",
  "gz",
  "bz2",
  "xz",
  "zst",
  "tgz",
]);
const CODE_EXTENSIONS = new Set([
  "c",
  "cc",
  "cpp",
  "cxx",
  "h",
  "hpp",
  "m",
  "mm",
  "swift",
  "go",
  "rs",
  "py",
  "java",
  "kt",
  "kts",
  "js",
  "jsx",
  "ts",
  "tsx",
  "css",
  "scss",
  "less",
  "html",
  "htm",
  "xml",
  "json",
  "yaml",
  "yml",
  "toml",
  "ini",
  "conf",
  "sh",
  "bash",
  "zsh",
  "fish",
  "ps1",
  "bat",
  "cmd",
  "sql",
  "md",
]);
const DATA_EXTENSIONS = new Set(["csv", "tsv", "parquet", "db", "sqlite", "sqlite3"]);
const PACKAGE_EXTENSIONS = new Set(["apk", "ipa", "dmg", "pkg", "msi", "deb", "rpm", "whl"]);
const SPECIAL_CODE_FILES = new Set([
  "makefile",
  "dockerfile",
  "cmakelists.txt",
  ".gitignore",
  ".gitattributes",
  ".env",
  ".env.local",
  ".env.production",
  ".env.development",
]);
const EDITABLE_TEXT_EXTENSIONS = new Set([
  ...CODE_EXTENSIONS,
  "csv",
  "txt",
  "text",
  "log",
  "cfg",
  "properties",
  "editorconfig",
]);

const MEDIA_MIME_TYPES: Record<string, string> = {
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
  mp4: "video/mp4",
  mkv: "video/x-matroska",
  mov: "video/quicktime",
  avi: "video/x-msvideo",
  webm: "video/webm",
  wmv: "video/x-ms-wmv",
  m4v: "video/x-m4v",
  mp3: "audio/mpeg",
  wav: "audio/wav",
  flac: "audio/flac",
  aac: "audio/aac",
  ogg: "audio/ogg",
  m4a: "audio/mp4",
};

const MONACO_LANGUAGE_BY_EXTENSION: Record<string, string> = {
  c: "cpp",
  cc: "cpp",
  cpp: "cpp",
  cxx: "cpp",
  h: "cpp",
  hpp: "cpp",
  m: "cpp",
  mm: "cpp",
  swift: "swift",
  go: "go",
  rs: "rust",
  py: "python",
  java: "java",
  js: "javascript",
  jsx: "javascript",
  ts: "typescript",
  tsx: "typescript",
  css: "css",
  scss: "scss",
  less: "less",
  html: "html",
  htm: "html",
  xml: "xml",
  json: "json",
  yaml: "yaml",
  yml: "yaml",
  md: "markdown",
  sql: "sql",
  sh: "shell",
  bash: "shell",
  zsh: "shell",
  fish: "shell",
  ps1: "powershell",
  bat: "bat",
  cmd: "bat",
};

function normalizePath(value: string) {
  if (!value) return "/";
  if (value.length > 1 && value.endsWith("/")) {
    return value.slice(0, -1);
  }
  return value;
}

function formatFileSize(bytes: number) {
  if (!Number.isFinite(bytes) || bytes < 0) return "--";
  if (bytes < 1024) return `${Math.round(bytes)} B`;

  const units = ["KB", "MB", "GB", "TB", "PB"];
  let value = bytes / 1024;
  let unitIndex = 0;

  while (value >= 1024 && unitIndex < units.length - 1) {
    value /= 1024;
    unitIndex += 1;
  }

  const maximumFractionDigits = value >= 100 ? 0 : value >= 10 ? 1 : 2;
  return `${value.toLocaleString(undefined, { maximumFractionDigits })} ${units[unitIndex]}`;
}

function formatModifiedAt(timestampSec: number) {
  if (!Number.isFinite(timestampSec) || timestampSec <= 0) return "--";
  const date = new Date(timestampSec * 1000);
  if (Number.isNaN(date.getTime())) return "--";
  return date.toLocaleString(undefined, {
    year: "numeric",
    month: "2-digit",
    day: "2-digit",
    hour: "2-digit",
    minute: "2-digit",
  });
}

function fileExtension(name: string) {
  const lastDot = name.lastIndexOf(".");
  if (lastDot <= 0 || lastDot >= name.length - 1) return "";
  return name.slice(lastDot + 1).toLowerCase();
}

function fileIconForName(name: string) {
  const lower = name.toLowerCase();
  const ext = fileExtension(name);

  if (SPECIAL_CODE_FILES.has(lower) || CODE_EXTENSIONS.has(ext)) {
    return <FiCode />;
  }
  if (IMAGE_EXTENSIONS.has(ext)) {
    return <FiImage />;
  }
  if (VIDEO_EXTENSIONS.has(ext)) {
    return <FiFilm />;
  }
  if (AUDIO_EXTENSIONS.has(ext)) {
    return <FiMusic />;
  }
  if (XMIND_EXTENSIONS.has(ext)) {
    return <FiGitBranch />;
  }
  if (ARCHIVE_EXTENSIONS.has(ext)) {
    return <FiArchive />;
  }
  if (DATA_EXTENSIONS.has(ext)) {
    return <FiDatabase />;
  }
  if (PACKAGE_EXTENSIONS.has(ext)) {
    return <FiPackage />;
  }
  return <FiFileText />;
}

function fileOpenModeForName(name: string): FileOpenMode {
  const lower = name.toLowerCase();
  const ext = fileExtension(name);
  if (PDF_EXTENSIONS.has(ext)) return "pdf";
  if (WORD_EXTENSIONS.has(ext)) return "word";
  if (PRESENTATION_EXTENSIONS.has(ext)) return "presentation";
  if (SPREADSHEET_EXTENSIONS.has(ext)) return "spreadsheet";
  if (IMAGE_EXTENSIONS.has(ext)) return "image";
  if (VIDEO_EXTENSIONS.has(ext)) return "video";
  if (AUDIO_EXTENSIONS.has(ext)) return "audio";
  if (XMIND_EXTENSIONS.has(ext)) return "xmind";
  if (SPECIAL_CODE_FILES.has(lower) || EDITABLE_TEXT_EXTENSIONS.has(ext)) return "editor";
  return "unsupported";
}

function mediaMimeType(name: string) {
  const ext = fileExtension(name);
  return MEDIA_MIME_TYPES[ext] ?? "application/octet-stream";
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

function base64ToObjectUrl(base64: string, mimeType: string) {
  const bytes = base64ToBytes(base64);
  return URL.createObjectURL(new Blob([bytes], { type: mimeType }));
}

function toArrayBuffer(bytes: Uint8Array): ArrayBuffer {
  const copy = new Uint8Array(bytes.byteLength);
  copy.set(bytes);
  return copy.buffer;
}

function escapeHtml(value: string) {
  return value
    .replaceAll("&", "&amp;")
    .replaceAll("<", "&lt;")
    .replaceAll(">", "&gt;")
    .replaceAll('"', "&quot;")
    .replaceAll("'", "&#39;");
}

function decodeXmlEntities(value: string) {
  return value
    .replaceAll("&lt;", "<")
    .replaceAll("&gt;", ">")
    .replaceAll("&amp;", "&")
    .replaceAll("&quot;", '"')
    .replaceAll("&apos;", "'");
}

function extractPrintableText(bytes: Uint8Array) {
  const utf8 = new TextDecoder("utf-8", { fatal: false }).decode(bytes);
  const cleaned = utf8.replace(/[^\x09\x0A\x0D\x20-\x7E\u00A0-\uFFFF]/g, "");
  return cleaned.trim();
}

async function previewWordDocument(bytes: Uint8Array) {
  const html = await mammoth.convertToHtml({
    arrayBuffer: toArrayBuffer(bytes),
  });
  return html.value;
}

async function previewSpreadsheet(bytes: Uint8Array): Promise<SpreadsheetPreviewData> {
  const workbook = XLSX.read(bytes, { type: "array" });
  const firstSheetName = workbook.SheetNames[0];
  if (!firstSheetName) {
    return { sheetName: "", rows: [] };
  }
  const sheet = workbook.Sheets[firstSheetName];
  const rows = (XLSX.utils.sheet_to_json(sheet, { header: 1, defval: "" }) as Array<Array<unknown>>)
    .slice(0, 500)
    .map((row) => row.map((cell) => String(cell ?? "")));
  return { sheetName: firstSheetName, rows };
}

async function previewPresentation(bytes: Uint8Array): Promise<string[]> {
  const zip = await JSZip.loadAsync(bytes);
  const slidePaths = Object.keys(zip.files)
    .filter((path) => /^ppt\/slides\/slide\d+\.xml$/.test(path))
    .sort((a, b) => {
      const na = Number(a.match(/slide(\d+)\.xml$/)?.[1] ?? "0");
      const nb = Number(b.match(/slide(\d+)\.xml$/)?.[1] ?? "0");
      return na - nb;
    });

  const slides: string[] = [];
  for (const path of slidePaths) {
    const xml = await zip.files[path]?.async("text");
    if (!xml) continue;
    const matches = Array.from(xml.matchAll(/<a:t>([\s\S]*?)<\/a:t>/g));
    const texts = matches.map((m) => decodeXmlEntities(m[1] ?? "").trim()).filter(Boolean);
    slides.push(texts.join("\n"));
  }
  return slides;
}

function monacoLanguageForFile(name: string) {
  const lower = name.toLowerCase();
  if (lower === "dockerfile") return "shell";
  if (lower === "cmakelists.txt" || lower === "makefile") return "plaintext";
  if (lower.startsWith(".env")) return "shell";
  if (lower === ".gitignore" || lower === ".gitattributes") return "plaintext";
  return MONACO_LANGUAGE_BY_EXTENSION[fileExtension(name)] ?? "plaintext";
}

export default function FilesPage({ token, query }: Props) {
  const { t } = useI18n();
  const { theme } = useTheme();
  const uploadInputRef = useRef<HTMLInputElement | null>(null);
  const [path, setPath] = useState(() => {
    if (typeof window === "undefined") return "/";
    const raw = window.localStorage.getItem(FILES_PATH_KEY) ?? "/";
    return normalizePath(raw.trim() || "/");
  });
  const [rootPath, setRootPath] = useState("/");
  const [entries, setEntries] = useState<FileEntry[]>([]);
  const [selectedFile, setSelectedFile] = useState<string>("");
  const [fileContent, setFileContent] = useState<string>("");
  const [openMode, setOpenMode] = useState<FileOpenMode>("editor");
  const [previewUrl, setPreviewUrl] = useState<string>("");
  const [pdfBytes, setPdfBytes] = useState<Uint8Array | null>(null);
  const [docxBytes, setDocxBytes] = useState<Uint8Array | null>(null);
  const [pptxBytes, setPptxBytes] = useState<Uint8Array | null>(null);
  const [xmindBytes, setXmindBytes] = useState<Uint8Array | null>(null);
  const [docPreviewHtml, setDocPreviewHtml] = useState("");
  const [spreadsheetPreview, setSpreadsheetPreview] = useState<SpreadsheetPreviewData>({
    sheetName: "",
    rows: [],
  });
  const [presentationSlides, setPresentationSlides] = useState<string[]>([]);
  const [previewError, setPreviewError] = useState("");
  const [editorOpen, setEditorOpen] = useState(false);
  const [editorPreviewOpen, setEditorPreviewOpen] = useState(false);
  const [markdownPreviewTheme, setMarkdownPreviewTheme] = useState<MarkdownPreviewTheme>(() => {
    if (typeof window === "undefined") return "default";
    const raw = window.localStorage.getItem(FILES_MARKDOWN_PREVIEW_THEME_KEY);
    if (raw && MARKDOWN_PREVIEW_THEMES.includes(raw as MarkdownPreviewTheme)) {
      return raw as MarkdownPreviewTheme;
    }
    return "default";
  });
  const [markdownCodeTheme, setMarkdownCodeTheme] = useState<MarkdownCodeTheme>(() => {
    if (typeof window === "undefined") return "atom";
    const raw = window.localStorage.getItem(FILES_MARKDOWN_CODE_THEME_KEY);
    if (raw && MARKDOWN_CODE_THEMES.includes(raw as MarkdownCodeTheme)) {
      return raw as MarkdownCodeTheme;
    }
    return "atom";
  });
  const [loading, setLoading] = useState(false);
  const [uploading, setUploading] = useState(false);
  const [downloading, setDownloading] = useState(false);
  const [viewMode, setViewMode] = useState<FilesViewMode>(() => {
    if (typeof window === "undefined") return "list";
    const raw = window.localStorage.getItem(FILES_VIEW_MODE_KEY);
    return raw === "grid" ? "grid" : "list";
  });

  useEffect(() => {
    if (typeof window === "undefined") return;
    window.localStorage.setItem(FILES_VIEW_MODE_KEY, viewMode);
  }, [viewMode]);

  useEffect(() => {
    if (typeof window === "undefined") return;
    window.localStorage.setItem(FILES_PATH_KEY, normalizePath(path.trim() || "/"));
  }, [path]);

  useEffect(() => {
    if (typeof window === "undefined") return;
    window.localStorage.setItem(FILES_MARKDOWN_PREVIEW_THEME_KEY, markdownPreviewTheme);
  }, [markdownPreviewTheme]);

  useEffect(() => {
    if (typeof window === "undefined") return;
    window.localStorage.setItem(FILES_MARKDOWN_CODE_THEME_KEY, markdownCodeTheme);
  }, [markdownCodeTheme]);

  const replacePreviewUrl = (next: string) => {
    setPreviewUrl((current) => {
      if (current) {
        URL.revokeObjectURL(current);
      }
      return next;
    });
  };

  const resetPreviewPayload = () => {
    setPdfBytes(null);
    setDocxBytes(null);
    setPptxBytes(null);
    setXmindBytes(null);
    setDocPreviewHtml("");
    setSpreadsheetPreview({ sheetName: "", rows: [] });
    setPresentationSlides([]);
    setPreviewError("");
  };

  const closeViewer = () => {
    setEditorOpen(false);
    setEditorPreviewOpen(false);
    setSelectedFile("");
    setFileContent("");
    setOpenMode("editor");
    resetPreviewPayload();
    replacePreviewUrl("");
  };

  const loadList = async (targetPath = path) => {
    const normalizedTargetPath = normalizePath(targetPath.trim() || "/");
    setLoading(true);
    const res = await listFiles(token, normalizedTargetPath);
    setLoading(false);
    if (!res.ok) {
      toast.error(res.error ?? t("toast.request_failed"));
      return;
    }
    const rows = (res.entries ?? []) as unknown as FileEntry[];
    const serverCurrentPath = typeof res.current_path === "string" ? res.current_path : normalizedTargetPath;
    const serverRootPath = typeof res.root_path === "string" ? res.root_path : rootPath;
    setEntries(rows);
    setPath(normalizePath(serverCurrentPath));
    setRootPath(normalizePath(serverRootPath));
  };

  useEffect(() => {
    void loadList(path);
  }, []);

  useEffect(() => {
    if (!editorOpen || typeof window === "undefined") return;
    const onKeyDown = (event: KeyboardEvent) => {
      if (event.key === "Escape") {
        closeViewer();
      }
    };
    window.addEventListener("keydown", onKeyDown);
    return () => window.removeEventListener("keydown", onKeyDown);
  }, [editorOpen]);

  useEffect(() => {
    return () => {
      if (previewUrl) {
        URL.revokeObjectURL(previewUrl);
      }
    };
  }, [previewUrl]);

  const normalizedPath = normalizePath(path);
  const normalizedRootPath = normalizePath(rootPath);
  const atRoot = normalizedPath === normalizedRootPath;

  const breadcrumbs = (() => {
    if (!normalizedRootPath) {
      return [{ label: "/", path: "/" }];
    }
    const items: Array<{ label: string; path: string }> = [
      { label: normalizedRootPath, path: normalizedRootPath },
    ];
    if (normalizedPath === normalizedRootPath) {
      return items;
    }

    const rel = normalizedRootPath === "/"
      ? normalizedPath.slice(1)
      : normalizedPath.startsWith(`${normalizedRootPath}/`)
        ? normalizedPath.slice(normalizedRootPath.length + 1)
        : "";
    if (!rel) {
      return items;
    }

    let current = normalizedRootPath;
    for (const part of rel.split("/")) {
      if (!part) continue;
      current = `${current}/${part}`;
      items.push({ label: part, path: current });
    }
    return items;
  })();

  const openFile = async (entry: FileEntry) => {
    const mode = fileOpenModeForName(entry.name);
    const ext = fileExtension(entry.name);
    if (mode === "unsupported") {
      toast.error(t("files.preview_unsupported"));
      return;
    }

    const res = await readFile(token, entry.path);
    if (!res.ok) {
      toast.error(res.error ?? t("toast.request_failed"));
      return;
    }

    const base64 = res.content_base64;
    const bytes = base64ToBytes(base64);

    setSelectedFile(entry.path);
    setOpenMode(mode);
    setFileContent("");
    setEditorPreviewOpen(false);
    resetPreviewPayload();
    replacePreviewUrl("");

    if (mode === "editor") {
      setFileContent(decodeBase64Utf8(base64));
    } else if (mode === "image" || mode === "video" || mode === "audio") {
      replacePreviewUrl(base64ToObjectUrl(base64, mediaMimeType(entry.name)));
    } else if (mode === "pdf") {
      setPdfBytes(bytes);
    } else if (mode === "word") {
      if (ext === "docx" || ext === "dox") {
        setDocxBytes(bytes);
      } else {
        try {
          const html = await previewWordDocument(bytes);
          setDocPreviewHtml(html || `<pre>${escapeHtml(extractPrintableText(bytes) || t("files.preview_empty"))}</pre>`);
        } catch {
          const fallbackText = extractPrintableText(bytes);
          if (fallbackText) {
            setDocPreviewHtml(`<pre>${escapeHtml(fallbackText)}</pre>`);
          } else {
            setPreviewError(t("files.preview_parse_failed"));
          }
        }
      }
    } else if (mode === "spreadsheet") {
      try {
        const sheet = await previewSpreadsheet(bytes);
        setSpreadsheetPreview(sheet);
      } catch {
        setPreviewError(t("files.preview_parse_failed"));
      }
    } else if (mode === "presentation") {
      if (ext === "pptx") {
        setPptxBytes(bytes);
      } else {
        try {
          const slides = await previewPresentation(bytes);
          if (slides.length === 0) {
            setPreviewError(t("files.preview_empty"));
          } else {
            setPresentationSlides(slides);
          }
        } catch {
          const fallbackText = extractPrintableText(bytes);
          if (fallbackText) {
            setPresentationSlides([fallbackText]);
          } else {
            setPreviewError(t("files.preview_parse_failed"));
          }
        }
      }
    } else if (mode === "xmind") {
      setXmindBytes(bytes);
    }
    setEditorOpen(true);
  };

  const saveFile = async () => {
    if (!selectedFile) {
      toast.error(t("files.select_file"));
      return;
    }
    const res = await writeFile(token, selectedFile, encodeBase64Utf8(fileContent));
    if (!res.ok) {
      toast.error(res.error ?? t("toast.request_failed"));
      return;
    }
    toast.success(t("files.saved_path", { path: selectedFile }));
    await loadList(path);
  };

  const uploadFileToCurrentPath = async (file: File) => {
    const dir = normalizePath(path);
    const targetPath = dir === "/" ? `/${file.name}` : `${dir}/${file.name}`;
    setUploading(true);
    const base64 = await browserFileToBase64(file);
    const res = await writeFile(token, targetPath, base64);
    setUploading(false);
    if (!res.ok) {
      toast.error(res.error ?? t("toast.request_failed"));
      return;
    }
    toast.success(t("files.saved_path", { path: targetPath }));
    await loadList(path);
  };

  const downloadSelectedFile = async () => {
    if (!selectedFile) {
      toast.error(t("files.select_file"));
      return;
    }
    setDownloading(true);
    const res = await readFile(token, selectedFile);
    setDownloading(false);
    if (!res.ok) {
      toast.error(res.error ?? t("toast.request_failed"));
      return;
    }
    const bytes = base64ToBytes(res.content_base64 ?? "");
    const fileName = selectedFile.split(/[\\/]/).pop() || "download.bin";
    const mimeType = mediaMimeType(fileName);
    const url = URL.createObjectURL(new Blob([bytes], { type: mimeType }));
    const anchor = document.createElement("a");
    anchor.href = url;
    anchor.download = fileName;
    anchor.click();
    URL.revokeObjectURL(url);
  };

  const parentPath = (() => {
    if (atRoot) return normalizedRootPath;
    const trimmed = normalizedPath;
    const idx = trimmed.lastIndexOf("/");
    if (idx <= 0) return normalizedRootPath;
    const candidate = trimmed.slice(0, idx) || "/";
    if (
      normalizedRootPath !== "/" &&
      candidate !== normalizedRootPath &&
      !candidate.startsWith(`${normalizedRootPath}/`)
    ) {
      return normalizedRootPath;
    }
    return candidate;
  })();

  const visibleEntries = (() => {
    const q = query.trim().toLowerCase();
    if (!q) return entries;
    return entries.filter((entry) => {
      return entry.name.toLowerCase().includes(q) || entry.path.toLowerCase().includes(q);
    });
  })();

  const selectedFileName = useMemo(() => {
    if (!selectedFile) return "";
    const parts = selectedFile.split(/[\\/]/);
    return parts[parts.length - 1] ?? selectedFile;
  }, [selectedFile]);
  const selectedFileExt = useMemo(
    () => (selectedFileName ? fileExtension(selectedFileName) : ""),
    [selectedFileName]
  );
  const isDocxPreview = openMode === "word" && (selectedFileExt === "docx" || selectedFileExt === "dox");
  const isPptxPreview = openMode === "presentation" && selectedFileExt === "pptx";
  const editorLanguage = useMemo(
    () => (selectedFileName ? monacoLanguageForFile(selectedFileName) : "plaintext"),
    [selectedFileName]
  );
  const isMarkdownFile = MARKDOWN_EXTENSIONS.has(selectedFileExt);
  const isHtmlFile = selectedFileExt === "html" || selectedFileExt === "htm";
  const canToggleEditorPreview = openMode === "editor" && (isMarkdownFile || isHtmlFile);
  const showEditorPreview = canToggleEditorPreview && editorPreviewOpen;
  const canSave = openMode === "editor" && Boolean(selectedFile);
  const isPreviewMode = openMode !== "editor" || showEditorPreview;

  return (
    <>
      <div className="grid h-full min-h-[460px] grid-cols-1 gap-4">
        <section className="flex h-full min-h-[460px] flex-col rounded-3xl bg-white/70 p-4 shadow-soft ring-1 ring-slate-200/70 backdrop-blur dark:bg-neutral-900/55 dark:ring-neutral-800/70">
          <div className="flex items-start justify-between gap-3">
            <div>
              <h2 className="inline-flex items-center gap-2 text-base font-semibold tracking-tight text-slate-900 dark:text-neutral-50">
                <FiFolder className="text-[15px]" />
                {t("files.title")}
              </h2>
              <div className="mt-1 flex flex-wrap items-center gap-1 font-mono text-xs text-slate-500 dark:text-neutral-400">
                {breadcrumbs.map((item, idx) => (
                  <div key={item.path} className="flex items-center gap-1">
                    {idx > 0 ? <span>/</span> : null}
                    <button
                      className="rounded px-1 py-0.5 transition-colors hover:bg-slate-200/70 hover:text-slate-700 dark:hover:bg-neutral-800 dark:hover:text-neutral-100"
                      onClick={() => void loadList(item.path)}
                      title={item.path}
                    >
                      {item.label}
                    </button>
                  </div>
                ))}
              </div>
            </div>
            <div className="flex items-center gap-2">
              <button
                className="grid h-10 w-10 place-items-center rounded-2xl bg-slate-100 text-slate-700 hover:bg-slate-200 disabled:opacity-50 dark:bg-neutral-800 dark:text-neutral-100 dark:hover:bg-neutral-700"
                onClick={() => void loadList(parentPath)}
                disabled={atRoot}
                title={t("common.up")}
              >
                <FiChevronLeft />
              </button>
              <button
                className={cn(
                  "grid h-10 w-10 place-items-center rounded-2xl bg-slate-100 text-slate-700 hover:bg-slate-200 disabled:opacity-50 dark:bg-neutral-800 dark:text-neutral-100 dark:hover:bg-neutral-700",
                  loading && "animate-pulse"
                )}
                onClick={() => void loadList(path)}
                disabled={loading}
                title={t("common.refresh")}
              >
                <FiRefreshCw />
              </button>
              <button
                className={cn(
                  "inline-flex h-10 items-center gap-2 rounded-2xl px-3 text-xs font-semibold transition-colors",
                  uploading
                    ? "cursor-wait bg-slate-200 text-slate-500 dark:bg-neutral-800 dark:text-neutral-500"
                    : "bg-sky-100 text-sky-800 hover:bg-sky-200 dark:bg-sky-900/40 dark:text-sky-300 dark:hover:bg-sky-900/60"
                )}
                onClick={() => uploadInputRef.current?.click()}
                disabled={uploading}
                title={t("common.upload")}
              >
                <FiUpload />
                {t("common.upload")}
              </button>
              <input
                ref={uploadInputRef}
                type="file"
                className="hidden"
                onChange={(event) => {
                  const file = event.target.files?.[0];
                  if (file) {
                    void uploadFileToCurrentPath(file);
                  }
                  event.target.value = "";
                }}
              />
              <div className="inline-flex rounded-2xl bg-slate-100 p-1 dark:bg-neutral-800">
                <button
                  className={cn(
                    "grid h-8 w-8 place-items-center rounded-xl text-slate-700 transition-colors hover:bg-slate-200 dark:text-neutral-100 dark:hover:bg-neutral-700",
                    viewMode === "list" &&
                      "bg-white shadow-sm ring-1 ring-slate-200/80 dark:bg-neutral-900 dark:ring-neutral-700/80"
                  )}
                  onClick={() => setViewMode("list")}
                  title={t("files.view_list")}
                  aria-label={t("files.view_list")}
                >
                  <FiList />
                </button>
                <button
                  className={cn(
                    "grid h-8 w-8 place-items-center rounded-xl text-slate-700 transition-colors hover:bg-slate-200 dark:text-neutral-100 dark:hover:bg-neutral-700",
                    viewMode === "grid" &&
                      "bg-white shadow-sm ring-1 ring-slate-200/80 dark:bg-neutral-900 dark:ring-neutral-700/80"
                  )}
                  onClick={() => setViewMode("grid")}
                  title={t("files.view_grid")}
                  aria-label={t("files.view_grid")}
                >
                  <FiGrid />
                </button>
              </div>
            </div>
          </div>

          <div className="mt-4 flex items-center gap-2">
            <input
              value={path}
              onChange={(e) => setPath(e.target.value)}
              onKeyDown={(e) => {
                if (e.key === "Enter") void loadList(path);
              }}
              className="w-full rounded-2xl border border-slate-200 bg-white px-3 py-2 text-sm text-slate-900 shadow-sm outline-none placeholder:text-slate-400 focus:border-slate-300 dark:border-neutral-800 dark:bg-neutral-950/40 dark:text-neutral-50 dark:placeholder:text-neutral-500 dark:focus:border-neutral-700"
              placeholder={normalizedRootPath}
            />
            <button
              className="inline-flex h-10 shrink-0 items-center justify-center rounded-2xl bg-slate-900 px-4 text-sm font-semibold text-white shadow-sm hover:bg-slate-800 dark:bg-neutral-50 dark:text-neutral-900 dark:hover:bg-white"
              onClick={() => void loadList(path)}
            >
              {t("common.go")}
            </button>
          </div>

          <div
            className={cn(
              "mt-4 min-h-0 flex-1 overflow-auto p-1",
              viewMode === "list"
                ? "space-y-1"
                : "grid content-start auto-rows-[104px] grid-cols-2 gap-2 sm:grid-cols-3 xl:grid-cols-2"
            )}
          >
            {visibleEntries.map((entry) => {
              const sizeText = entry.is_directory ? "dir" : formatFileSize(entry.size);
              const permsText = entry.permissions || "---------";
              const modifiedText = formatModifiedAt(entry.modified_at);
              const fileIcon = entry.is_directory ? <FiFolder /> : fileIconForName(entry.name);

              return (
                <button
                  key={entry.path}
                  className={cn(
                    "rounded-2xl border border-transparent bg-white/70 text-left shadow-sm ring-1 ring-slate-200/60 transition-colors hover:bg-slate-50 dark:bg-neutral-950/30 dark:text-neutral-50 dark:ring-neutral-800/70 dark:hover:bg-neutral-900/60",
                    viewMode === "list"
                      ? "flex w-full items-center gap-3 px-3 py-2 text-sm text-slate-800"
                      : "flex h-full w-full flex-col gap-2 px-3 py-3 text-slate-800"
                  )}
                  onClick={() => {
                    if (entry.is_directory) {
                      void loadList(entry.path);
                    } else {
                      void openFile(entry);
                    }
                  }}
                >
                  {viewMode === "list" ? (
                    <>
                      <span className="text-base text-slate-600 dark:text-neutral-300">
                        {fileIcon}
                      </span>
                      <div className="min-w-0 flex-1">
                        <div className="truncate font-semibold" title={entry.name}>
                          {entry.name}
                        </div>
                        <div className="mt-0.5 truncate font-mono text-[11px] text-slate-500 dark:text-neutral-400">
                          {permsText} · {modifiedText}
                        </div>
                      </div>
                      <span className="shrink-0 font-mono text-[11px] text-slate-500 dark:text-neutral-400">
                        {sizeText}
                      </span>
                    </>
                  ) : (
                    <>
                      <div className="flex w-full items-center justify-between gap-2">
                        <span className="text-lg text-slate-600 dark:text-neutral-300">
                          {fileIcon}
                        </span>
                        <span className="rounded-full bg-white/70 px-2 py-0.5 font-mono text-[11px] text-slate-500 dark:bg-neutral-900/60 dark:text-neutral-400">
                          {sizeText}
                        </span>
                      </div>
                      <span className="block w-full truncate text-sm font-semibold" title={entry.name}>
                        {entry.name}
                      </span>
                      <span className="block w-full truncate font-mono text-[11px] text-slate-500 dark:text-neutral-400">
                        {permsText} · {modifiedText}
                      </span>
                    </>
                  )}
                </button>
              );
            })}
            {visibleEntries.length === 0 ? (
              <div
                className={cn(
                  "rounded-2xl border border-dashed border-slate-200 p-5 text-center text-sm text-slate-500 dark:border-neutral-800 dark:text-neutral-400",
                  viewMode === "grid" && "col-span-full"
                )}
              >
                {query.trim() ? t("files.no_match") : t("files.empty")}
              </div>
            ) : null}
          </div>
        </section>
      </div>

      {editorOpen ? (
        <div className="fixed inset-0 z-50 p-4 sm:p-6">
          <div
            className="absolute inset-0 bg-slate-900/45 backdrop-blur-sm"
            onClick={closeViewer}
          />
          <section className="relative mx-auto flex h-full max-h-[900px] w-full max-w-6xl flex-col rounded-3xl bg-white/95 p-4 shadow-soft ring-1 ring-slate-200/80 dark:bg-neutral-900/95 dark:ring-neutral-800/80">
            <div className="flex items-start justify-between gap-3">
              <div className="min-w-0">
                <h2 className="text-base font-semibold tracking-tight text-slate-900 dark:text-neutral-50">
                  {isPreviewMode ? t("files.preview") : t("files.editor")}
                </h2>
                <div
                  className="mt-1 truncate font-mono text-xs text-slate-500 dark:text-neutral-400"
                  title={selectedFile}
                >
                  {selectedFile || t("files.select_file")}
                </div>
              </div>
              <div className="flex shrink-0 items-center gap-2">
                {openMode === "editor" ? (
                  <>
                    {isMarkdownFile ? (
                      <>
                        <select
                          value={markdownPreviewTheme}
                          onChange={(event) => {
                            const nextTheme = event.target.value as MarkdownPreviewTheme;
                            if (MARKDOWN_PREVIEW_THEMES.includes(nextTheme)) {
                              setMarkdownPreviewTheme(nextTheme);
                            }
                          }}
                          className="h-10 rounded-2xl border border-slate-200 bg-white px-3 text-sm text-slate-700 outline-none focus:border-slate-300 dark:border-neutral-700 dark:bg-neutral-900 dark:text-neutral-100 dark:focus:border-neutral-600"
                          title="Preview Theme"
                          aria-label="Preview Theme"
                        >
                          {MARKDOWN_PREVIEW_THEMES.map((themeName) => (
                            <option key={themeName} value={themeName}>
                              {themeName}
                            </option>
                          ))}
                        </select>
                        <select
                          value={markdownCodeTheme}
                          onChange={(event) => {
                            const nextTheme = event.target.value as MarkdownCodeTheme;
                            if (MARKDOWN_CODE_THEMES.includes(nextTheme)) {
                              setMarkdownCodeTheme(nextTheme);
                            }
                          }}
                          className="h-10 rounded-2xl border border-slate-200 bg-white px-3 text-sm text-slate-700 outline-none focus:border-slate-300 dark:border-neutral-700 dark:bg-neutral-900 dark:text-neutral-100 dark:focus:border-neutral-600"
                          title="Code Theme"
                          aria-label="Code Theme"
                        >
                          {MARKDOWN_CODE_THEMES.map((themeName) => (
                            <option key={themeName} value={themeName}>
                              {themeName}
                            </option>
                          ))}
                        </select>
                      </>
                    ) : null}
                    {canToggleEditorPreview ? (
                      <button
                        onClick={() => setEditorPreviewOpen((current) => !current)}
                        className="inline-flex h-10 items-center rounded-2xl bg-slate-100 px-4 text-sm font-semibold text-slate-700 hover:bg-slate-200 dark:bg-neutral-800 dark:text-neutral-100 dark:hover:bg-neutral-700"
                      >
                        {editorPreviewOpen ? t("files.editor") : t("files.preview")}
                      </button>
                    ) : null}
                    <button
                      onClick={() => void saveFile()}
                      disabled={!canSave}
                      className="inline-flex h-10 items-center gap-2 rounded-2xl bg-slate-900 px-4 text-sm font-semibold text-white shadow-sm hover:bg-slate-800 disabled:opacity-50 dark:bg-neutral-50 dark:text-neutral-900 dark:hover:bg-white"
                    >
                      <FiSave /> {t("common.save")}
                    </button>
                  </>
                ) : null}
                <button
                  onClick={() => void downloadSelectedFile()}
                  disabled={!selectedFile || downloading}
                  className="inline-flex h-10 items-center gap-2 rounded-2xl bg-slate-100 px-3 text-sm font-semibold text-slate-700 hover:bg-slate-200 disabled:opacity-50 dark:bg-neutral-800 dark:text-neutral-100 dark:hover:bg-neutral-700"
                >
                  <FiDownload /> {t("common.download")}
                </button>
                <button
                  onClick={closeViewer}
                  className="inline-flex h-10 w-10 items-center justify-center rounded-2xl bg-slate-100 text-slate-700 hover:bg-slate-200 dark:bg-neutral-800 dark:text-neutral-100 dark:hover:bg-neutral-700"
                  title={t("common.close")}
                  aria-label={t("common.close")}
                >
                  <FiX />
                </button>
              </div>
            </div>

            {openMode === "editor" ? (
              isMarkdownFile ? (
                <MdEditorPanel
                  value={fileContent}
                  onChange={setFileContent}
                  theme={theme}
                  preview={editorPreviewOpen}
                  previewTheme={markdownPreviewTheme}
                  codeTheme={markdownCodeTheme}
                />
              ) : showEditorPreview && isHtmlFile ? (
                <HtmlPreview content={fileContent} emptyText={t("files.preview_empty")} />
              ) : (
                <EditorPreview
                  language={editorLanguage}
                  value={fileContent}
                  onChange={setFileContent}
                  theme={theme}
                />
              )
            ) : previewError ? (
              <div className="mt-4 grid min-h-0 flex-1 place-items-center rounded-2xl border border-slate-200 bg-white p-6 text-sm text-slate-500 dark:border-neutral-800 dark:bg-neutral-950/50 dark:text-neutral-300">
                {previewError}
              </div>
            ) : openMode === "pdf" ? (
              <PdfPreview
                bytes={pdfBytes}
                loadingText={t("files.preview_loading")}
                onParseFailed={() => setPreviewError(t("files.preview_parse_failed"))}
              />
            ) : openMode === "word" ? (
              isDocxPreview ? (
                <DocxPreview
                  bytes={docxBytes}
                  loadingText={t("files.preview_loading")}
                  onParseFailed={() => setPreviewError(t("files.preview_parse_failed"))}
                />
              ) : (
                <WordPreview html={docPreviewHtml} loadingText={t("files.preview_loading")} />
              )
            ) : openMode === "spreadsheet" ? (
              <SpreadsheetPreview
                data={spreadsheetPreview}
                sheetLabel={t("files.preview_sheet")}
                emptyText={t("files.preview_empty")}
              />
            ) : openMode === "presentation" ? (
              isPptxPreview ? (
                <PptxPreview
                  bytes={pptxBytes}
                  loadingText={t("files.preview_loading")}
                  emptyText={t("files.preview_empty")}
                  slideLabel={(index) => t("files.preview_slide", { index })}
                  onParseFailed={() => setPreviewError(t("files.preview_parse_failed"))}
                />
              ) : (
                <PresentationPreview
                  slides={presentationSlides}
                  slideLabel={(index) => t("files.preview_slide", { index })}
                  emptyText={t("files.preview_empty")}
                />
              )
            ) : openMode === "xmind" ? (
              <XmindPreview
                bytes={xmindBytes}
                loadingText={t("files.preview_loading")}
                emptyText={t("files.preview_empty")}
                onParseFailed={() => setPreviewError(t("files.preview_parse_failed"))}
              />
            ) : openMode === "image" ? (
              <ImagePreview
                url={previewUrl}
                alt={selectedFileName || "image"}
                loadingText={t("files.preview_loading")}
              />
            ) : openMode === "video" ? (
              <VideoPreview url={previewUrl} loadingText={t("files.preview_loading")} />
            ) : (
              <AudioPreview url={previewUrl} loadingText={t("files.preview_loading")} />
            )}
          </section>
        </div>
      ) : null}
    </>
  );
}
