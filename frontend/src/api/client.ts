import type {
  DockerContainerFileEntry,
  DockerContainerInfo,
  DockerContainerProcesses,
  DockerContainerStats,
  DockurrVmInfo,
  ScreenSource,
  SessionInfo,
} from "../types";

export type ApiResponse<T = unknown> = {
  ok: boolean;
  error?: string;
  code?: string;
} & T;

export const UNAUTHORIZED_EVENT = "ferryman:unauthorized";

export type UnauthorizedEventDetail = {
  reason?: string;
  status?: number;
  path?: string;
};

export function emitUnauthorized(detail: UnauthorizedEventDetail) {
  if (typeof window === "undefined") return;
  window.dispatchEvent(new CustomEvent<UnauthorizedEventDetail>(UNAUTHORIZED_EVENT, { detail }));
}

function apiBase(): string {
  return "";
}

async function request<T>(
  path: string,
  init: RequestInit,
  token?: string
): Promise<ApiResponse<T>> {
  const headers = new Headers(init.headers ?? {});
  if (!headers.has("Content-Type") && init.body !== undefined) {
    headers.set("Content-Type", "application/json");
  }
  if (token) {
    headers.set("X-Session-Token", token);
  }

  const response = await fetch(`${apiBase()}${path}`, {
    ...init,
    headers,
  });

  const text = await response.text();
  try {
    const parsed = JSON.parse(text) as ApiResponse<T>;
    if (token && (response.status === 401 || parsed.code === "unauthorized")) {
      emitUnauthorized({
        reason: parsed.error,
        status: response.status,
        path,
      });
    }
    return parsed;
  } catch {
    if (token && response.status === 401) {
      emitUnauthorized({
        reason: undefined,
        status: response.status,
        path,
      });
    }
    return {
      ok: false,
      error: text || `HTTP ${response.status}`,
      code: "invalid_json",
    } as ApiResponse<T>;
  }
}

export async function login(accessKey: string): Promise<ApiResponse<{ session_token: string; ws_port: number; http_port: number; host: string }>> {
  return request("/api/auth/login", {
    method: "POST",
    body: JSON.stringify({ access_key: accessKey }),
  });
}

export async function getSessionMe(token: string) {
  return request<{ command_authorized: boolean; screen_authorized: boolean }>(
    "/api/session/me",
    { method: "GET" },
    token
  );
}

export async function listFiles(token: string, path: string) {
  const encoded = encodeURIComponent(path);
  return request<{
    entries: Array<Record<string, unknown>>;
    current_path?: string;
    root_path?: string;
  }>(
    `/api/files/list?path=${encoded}`,
    { method: "GET" },
    token
  );
}

export async function readFile(token: string, path: string) {
  const encoded = encodeURIComponent(path);
  return request<{ path: string; content_base64: string }>(
    `/api/files/read?path=${encoded}`,
    { method: "GET" },
    token
  );
}

export async function writeFile(token: string, path: string, base64Content: string) {
  const encoded = encodeURIComponent(path);
  return request<{ path: string; bytes: number }>(
    `/api/files/write?path=${encoded}&base64=1`,
    {
      method: "POST",
      headers: {
        "Content-Type": "text/plain",
      },
      body: base64Content,
    },
    token
  );
}

export async function startTask(token: string, command: string) {
  return request<{ task_id: string }>(
    "/api/tasks/start",
    {
      method: "POST",
      body: JSON.stringify({ command }),
    },
    token
  );
}

export async function listTasks(token: string) {
  return request<{ tasks: Array<Record<string, unknown>> }>(
    "/api/tasks/list",
    { method: "GET" },
    token
  );
}

export async function getTask(token: string, taskId: string) {
  const encoded = encodeURIComponent(taskId);
  return request<{ task_id: string; status: string; output_base64: string; exit_code: number; command: string }>(
    `/api/tasks/get?task_id=${encoded}`,
    { method: "GET" },
    token
  );
}

export async function getLogs(token: string, lines = 200) {
  return request<{ items: Array<Record<string, unknown>> }>(
    `/api/logs/tail?lines=${lines}`,
    { method: "GET" },
    token
  );
}

export type CreateDockurrVmPayload = {
  os: "windows" | "macos";
  version: string;
  ram: string;
  disk: string;
  name?: string;
  persist: boolean;
};

export async function listDockurrVms(token: string) {
  return request<{ vms: Array<DockurrVmInfo> }>(
    "/api/dockurr/list",
    { method: "GET" },
    token
  );
}

export async function createDockurrVm(token: string, payload: CreateDockurrVmPayload) {
  return request<{ vm: DockurrVmInfo }>(
    "/api/dockurr/create",
    {
      method: "POST",
      body: JSON.stringify(payload),
    },
    token
  );
}

export async function stopDockurrVm(token: string, name: string) {
  return request<{ name: string }>(
    "/api/dockurr/stop",
    {
      method: "POST",
      body: JSON.stringify({ name }),
    },
    token
  );
}

export async function restartDockurrVm(token: string, name: string) {
  return request<{ name: string }>(
    "/api/dockurr/restart",
    {
      method: "POST",
      body: JSON.stringify({ name }),
    },
    token
  );
}

export async function getDockurrVmLogs(token: string, name: string, tail = 50) {
  const encoded = encodeURIComponent(name);
  return request<{ name: string; logs: string }>(
    `/api/dockurr/logs?name=${encoded}&tail=${tail}`,
    { method: "GET" },
    token
  );
}

export async function getDockurrVmInspect(token: string, name: string) {
  const encoded = encodeURIComponent(name);
  return request<{ name: string; inspect: string }>(
    `/api/dockurr/inspect?name=${encoded}`,
    { method: "GET" },
    token
  );
}

export async function listDockerContainers(token: string, all = true) {
  return request<{ containers: Array<DockerContainerInfo>; all: boolean }>(
    `/api/docker/list?all=${all ? 1 : 0}`,
    { method: "GET" },
    token
  );
}

export async function startDockerContainer(token: string, name: string) {
  return request<{ name: string }>(
    "/api/docker/start",
    {
      method: "POST",
      body: JSON.stringify({ name }),
    },
    token
  );
}

export async function stopDockerContainer(token: string, name: string) {
  return request<{ name: string }>(
    "/api/docker/stop",
    {
      method: "POST",
      body: JSON.stringify({ name }),
    },
    token
  );
}

export async function restartDockerContainer(token: string, name: string) {
  return request<{ name: string }>(
    "/api/docker/restart",
    {
      method: "POST",
      body: JSON.stringify({ name }),
    },
    token
  );
}

export async function getDockerContainerLogs(token: string, name: string, tail = 160) {
  const encoded = encodeURIComponent(name);
  return request<{ name: string; logs: string }>(
    `/api/docker/logs?name=${encoded}&tail=${tail}`,
    { method: "GET" },
    token
  );
}

export async function getDockerContainerInspect(token: string, name: string) {
  const encoded = encodeURIComponent(name);
  return request<{ name: string; inspect: string }>(
    `/api/docker/inspect?name=${encoded}`,
    { method: "GET" },
    token
  );
}

export async function getDockerContainerStats(token: string, name: string) {
  const encoded = encodeURIComponent(name);
  return request<{ name: string; stats: DockerContainerStats }>(
    `/api/docker/stats?name=${encoded}`,
    { method: "GET" },
    token
  );
}

export async function getDockerContainerProcesses(token: string, name: string, limit = 120) {
  const encoded = encodeURIComponent(name);
  return request<DockerContainerProcesses>(
    `/api/docker/processes?name=${encoded}&limit=${Math.max(1, limit)}`,
    { method: "GET" },
    token
  );
}

export async function listDockerContainerFiles(token: string, name: string, path: string) {
  const encodedName = encodeURIComponent(name);
  const encodedPath = encodeURIComponent(path);
  return request<{ name: string; entries: Array<DockerContainerFileEntry>; current_path: string }>(
    `/api/docker/files/list?name=${encodedName}&path=${encodedPath}`,
    { method: "GET" },
    token
  );
}

export async function readDockerContainerFile(token: string, name: string, path: string) {
  const encodedName = encodeURIComponent(name);
  const encodedPath = encodeURIComponent(path);
  return request<{ name: string; path: string; content_base64: string }>(
    `/api/docker/files/read?name=${encodedName}&path=${encodedPath}`,
    { method: "GET" },
    token
  );
}

export async function writeDockerContainerFile(
  token: string,
  name: string,
  path: string,
  base64Content: string
) {
  const encodedName = encodeURIComponent(name);
  const encodedPath = encodeURIComponent(path);
  return request<{ name: string; path: string; bytes: number }>(
    `/api/docker/files/write?name=${encodedName}&path=${encodedPath}&base64=1`,
    {
      method: "POST",
      headers: {
        "Content-Type": "text/plain",
      },
      body: base64Content,
    },
    token
  );
}

export async function getScreenCapabilities(token: string) {
  return request<{ capabilities: Record<string, unknown>; screen_authorized: boolean }>(
    "/api/screen/capabilities",
    { method: "GET" },
    token
  );
}

export async function getScreenSources(token: string) {
  return request<{
    sources: Array<ScreenSource>;
    default_source_id: string;
    active_source_id: string;
  }>(
    "/api/screen/sources",
    { method: "GET" },
    token
  );
}

export type ScreenUploadBeginPayload = {
  transfer_id: string;
  name: string;
  size: number;
  drop_x?: number;
  drop_y?: number;
  view_width?: number;
  view_height?: number;
};

export async function beginScreenUploadTransfer(
  token: string,
  payload: ScreenUploadBeginPayload,
  signal?: AbortSignal
) {
  return request<{ transfer_id: string; target_dir: string; accepted: boolean }>(
    "/api/screen/upload/begin",
    {
      method: "POST",
      body: JSON.stringify(payload),
      signal,
    },
    token
  );
}

export async function appendScreenUploadTransferChunk(
  token: string,
  transferId: string,
  dataBase64: string,
  signal?: AbortSignal
) {
  return request<{ transfer_id: string; received_bytes: number; expected_bytes: number }>(
    "/api/screen/upload/chunk",
    {
      method: "POST",
      body: JSON.stringify({
        transfer_id: transferId,
        data_base64: dataBase64,
      }),
      signal,
    },
    token
  );
}

export async function commitScreenUploadTransfer(
  token: string,
  transferId: string,
  signal?: AbortSignal
) {
  return request<{ transfer_id: string; name: string; path: string; target_dir: string; bytes: number }>(
    "/api/screen/upload/commit",
    {
      method: "POST",
      body: JSON.stringify({ transfer_id: transferId }),
      signal,
    },
    token
  );
}

export async function cancelScreenUploadTransfer(token: string, transferId: string) {
  return request<{ transfer_id: string; cancelled: boolean }>(
    "/api/screen/upload/cancel",
    {
      method: "POST",
      body: JSON.stringify({ transfer_id: transferId }),
    },
    token
  );
}

export function wsUrl(
  session: SessionInfo,
  path: "/ws/terminal" | "/ws/webrtc" | "/ws/logs" | "/ws/dockurr" | "/ws/monitor"
): string {
  const url = new URL(path, window.location.origin);
  url.protocol = window.location.protocol === "https:" ? "wss:" : "ws:";
  url.searchParams.set("token", session.token);
  return url.toString();
}
