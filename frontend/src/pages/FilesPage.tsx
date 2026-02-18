import { useEffect, useState } from "react";
import { toast } from "../toast";
import {
  FiChevronLeft,
  FiFileText,
  FiFolder,
  FiGrid,
  FiList,
  FiRefreshCw,
  FiSave,
  FiX,
} from "react-icons/fi";

import { listFiles, readFile, writeFile } from "../api/client";
import { useI18n } from "../i18n";
import { decodeBase64Utf8, encodeBase64Utf8 } from "../util/codec";
import { cn } from "../util/cn";
import type { FileEntry } from "../types";

type Props = {
  token: string;
  query: string;
};

type FilesViewMode = "list" | "grid";

const FILES_VIEW_MODE_KEY = "ferryman.files.view_mode";

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

export default function FilesPage({ token, query }: Props) {
  const { t } = useI18n();
  const [path, setPath] = useState("/");
  const [rootPath, setRootPath] = useState("/");
  const [entries, setEntries] = useState<FileEntry[]>([]);
  const [selectedFile, setSelectedFile] = useState<string>("");
  const [fileContent, setFileContent] = useState<string>("");
  const [editorOpen, setEditorOpen] = useState(false);
  const [loading, setLoading] = useState(false);
  const [viewMode, setViewMode] = useState<FilesViewMode>(() => {
    if (typeof window === "undefined") return "list";
    const raw = window.localStorage.getItem(FILES_VIEW_MODE_KEY);
    return raw === "grid" ? "grid" : "list";
  });

  useEffect(() => {
    if (typeof window === "undefined") return;
    window.localStorage.setItem(FILES_VIEW_MODE_KEY, viewMode);
  }, [viewMode]);

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
    void loadList("/");
  }, []);

  useEffect(() => {
    if (!editorOpen || typeof window === "undefined") return;
    const onKeyDown = (event: KeyboardEvent) => {
      if (event.key === "Escape") {
        setEditorOpen(false);
      }
    };
    window.addEventListener("keydown", onKeyDown);
    return () => window.removeEventListener("keydown", onKeyDown);
  }, [editorOpen]);

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

  const openFile = async (filePath: string) => {
    const res = await readFile(token, filePath);
    if (!res.ok) {
      toast.error(res.error ?? t("toast.request_failed"));
      return;
    }
    setSelectedFile(filePath);
    setFileContent(decodeBase64Utf8(res.content_base64));
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

  return (
    <>
      <div className="grid grid-cols-1 gap-4">
      <section className="rounded-3xl bg-white/70 p-4 shadow-soft ring-1 ring-slate-200/70 backdrop-blur dark:bg-neutral-900/55 dark:ring-neutral-800/70">
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
            "mt-4 max-h-[560px] overflow-auto p-1",
            viewMode === "list" ? "space-y-1" : "grid grid-cols-2 gap-2 sm:grid-cols-3 xl:grid-cols-2"
          )}
        >
          {visibleEntries.map((entry) => (
            <button
              key={entry.path}
              className={cn(
                "rounded-2xl border border-transparent bg-white/70 text-left shadow-sm ring-1 ring-slate-200/60 transition-colors hover:bg-slate-50 dark:bg-neutral-950/30 dark:text-neutral-50 dark:ring-neutral-800/70 dark:hover:bg-neutral-900/60",
                viewMode === "list"
                  ? "flex w-full items-center gap-3 px-3 py-2 text-sm text-slate-800"
                  : "flex min-h-[104px] w-full flex-col gap-2 px-3 py-3 text-slate-800"
              )}
              onClick={() => {
                if (entry.is_directory) {
                  void loadList(entry.path);
                } else {
                  void openFile(entry.path);
                }
              }}
            >
              {viewMode === "list" ? (
                <>
                  <span className="text-base text-slate-600 dark:text-neutral-300">
                    {entry.is_directory ? <FiFolder /> : <FiFileText />}
                  </span>
                  <span className="min-w-0 flex-1 truncate font-semibold" title={entry.name}>
                    {entry.name}
                  </span>
                  <span className="shrink-0 font-mono text-[11px] text-slate-500 dark:text-neutral-400">
                    {entry.is_directory ? "dir" : formatFileSize(entry.size)}
                  </span>
                </>
              ) : (
                <>
                  <div className="flex w-full items-center justify-between gap-2">
                    <span className="text-lg text-slate-600 dark:text-neutral-300">
                      {entry.is_directory ? <FiFolder /> : <FiFileText />}
                    </span>
                    <span className="rounded-full bg-white/70 px-2 py-0.5 font-mono text-[11px] text-slate-500 dark:bg-neutral-900/60 dark:text-neutral-400">
                      {entry.is_directory ? "dir" : formatFileSize(entry.size)}
                    </span>
                  </div>
                  <span className="block w-full truncate text-sm font-semibold" title={entry.name}>
                    {entry.name}
                  </span>
                </>
              )}
            </button>
          ))}
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
            onClick={() => setEditorOpen(false)}
          />
          <section className="relative mx-auto flex h-full max-h-[900px] w-full max-w-6xl flex-col rounded-3xl bg-white/95 p-4 shadow-soft ring-1 ring-slate-200/80 dark:bg-neutral-900/95 dark:ring-neutral-800/80">
            <div className="flex items-start justify-between gap-3">
              <div className="min-w-0">
                <h2 className="text-base font-semibold tracking-tight text-slate-900 dark:text-neutral-50">
                  {t("files.editor")}
                </h2>
                <div
                  className="mt-1 truncate font-mono text-xs text-slate-500 dark:text-neutral-400"
                  title={selectedFile}
                >
                  {selectedFile || t("files.select_file")}
                </div>
              </div>
              <div className="flex shrink-0 items-center gap-2">
                <button
                  onClick={() => setEditorOpen(false)}
                  className="inline-flex h-10 w-10 items-center justify-center rounded-2xl bg-slate-100 text-slate-700 hover:bg-slate-200 dark:bg-neutral-800 dark:text-neutral-100 dark:hover:bg-neutral-700"
                  title={t("common.close")}
                  aria-label={t("common.close")}
                >
                  <FiX />
                </button>
                <button
                  onClick={() => void saveFile()}
                  disabled={!selectedFile}
                  className="inline-flex h-10 items-center gap-2 rounded-2xl bg-slate-900 px-4 text-sm font-semibold text-white shadow-sm hover:bg-slate-800 disabled:opacity-50 dark:bg-neutral-50 dark:text-neutral-900 dark:hover:bg-white"
                >
                  <FiSave /> {t("common.save")}
                </button>
              </div>
            </div>

            <textarea
              className="mt-4 h-full min-h-0 w-full flex-1 rounded-2xl border border-slate-200 bg-white px-3 py-3 font-mono text-[13px] leading-relaxed text-slate-900 shadow-sm outline-none placeholder:text-slate-400 focus:border-slate-300 dark:border-neutral-800 dark:bg-neutral-950/50 dark:text-neutral-50 dark:placeholder:text-neutral-500 dark:focus:border-neutral-700"
              value={fileContent}
              onChange={(event) => setFileContent(event.target.value)}
              placeholder={selectedFile ? "" : t("files.select_file")}
            />
          </section>
        </div>
      ) : null}
    </>
  );
}
