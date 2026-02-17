import type { SessionInfo } from "../types";

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
  return request<{ entries: Array<Record<string, unknown>> }>(
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

export async function getScreenCapabilities(token: string) {
  return request<{ capabilities: Record<string, unknown>; screen_authorized: boolean }>(
    "/api/screen/capabilities",
    { method: "GET" },
    token
  );
}

export function wsUrl(session: SessionInfo, path: "/ws/terminal" | "/ws/webrtc"): string {
  const protocol = window.location.protocol === "https:" ? "wss" : "ws";
  const host = window.location.hostname;
  return `${protocol}://${host}:${session.wsPort}${path}?token=${encodeURIComponent(session.token)}`;
}
