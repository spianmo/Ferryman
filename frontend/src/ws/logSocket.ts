import { emitUnauthorized, wsUrl } from "../api/client";
import type { SessionInfo } from "../types";

export type LogSocketStatus = {
  kind: "disconnected" | "connecting" | "connected" | "closed" | "failed" | "error";
  message?: string;
};

type Payload = Record<string, unknown>;
type MessageListener = (payload: Payload) => void;
type StatusListener = (status: LogSocketStatus) => void;

class LogSocketSingleton {
  private ws: WebSocket | null = null;
  private session: SessionInfo | null = null;
  private sessionKey = "";
  private status: LogSocketStatus = { kind: "disconnected" };
  private messageListeners = new Set<MessageListener>();
  private statusListeners = new Set<StatusListener>();
  private reconnectTimer: number | null = null;
  private manualClosing = false;
  private shouldRun = false;
  private tailLines = 400;
  private reconnectMs = 1500;

  private makeSessionKey(session: SessionInfo) {
    return `${window.location.origin}:${session.token}`;
  }

  private clearReconnectTimer() {
    if (this.reconnectTimer !== null) {
      window.clearTimeout(this.reconnectTimer);
      this.reconnectTimer = null;
    }
  }

  private emitStatus(next: LogSocketStatus) {
    this.status = next;
    this.statusListeners.forEach((listener) => listener(next));
  }

  private emitMessage(payload: Payload) {
    this.messageListeners.forEach((listener) => listener(payload));
  }

  private ensureSession(session: SessionInfo) {
    const nextKey = this.makeSessionKey(session);
    if (nextKey === this.sessionKey) return;
    this.stop();
    this.session = session;
    this.sessionKey = nextKey;
  }

  private connect() {
    if (!this.shouldRun || !this.session) return;
    if (
      this.ws &&
      (this.ws.readyState === WebSocket.OPEN || this.ws.readyState === WebSocket.CONNECTING)
    ) {
      return;
    }

    const ws = new WebSocket(wsUrl(this.session, "/ws/logs"));
    this.ws = ws;
    this.manualClosing = false;
    this.emitStatus({ kind: "connecting" });

    ws.onopen = () => {
      if (this.ws !== ws) return;
      this.emitStatus({ kind: "connected" });
      this.send({ action: "tail", lines: this.tailLines });
    };

    ws.onmessage = (event) => {
      if (this.ws !== ws) return;

      let payload: Payload;
      try {
        payload = JSON.parse(event.data as string) as Payload;
      } catch {
        return;
      }

      if (payload.ok === false) {
        if (String(payload.code ?? "") === "unauthorized") {
          const reason = payload.error;
          emitUnauthorized({ reason: typeof reason === "string" ? reason : undefined });
        }
        this.emitStatus({ kind: "error", message: String(payload.error ?? "WS error") });
        return;
      }

      this.emitMessage(payload);
    };

    ws.onerror = () => {
      if (this.ws !== ws) return;
      this.emitStatus({ kind: "failed" });
    };

    ws.onclose = () => {
      if (this.ws !== ws) return;
      this.ws = null;
      if (this.manualClosing) {
        this.manualClosing = false;
        this.emitStatus({ kind: "disconnected" });
        return;
      }

      this.emitStatus({ kind: "closed" });
      if (!this.shouldRun) return;

      this.clearReconnectTimer();
      this.reconnectTimer = window.setTimeout(() => {
        this.connect();
      }, this.reconnectMs);
    };
  }

  start(session: SessionInfo) {
    this.ensureSession(session);
    this.shouldRun = true;
    this.clearReconnectTimer();
    this.connect();
  }

  stop() {
    this.shouldRun = false;
    this.clearReconnectTimer();
    this.manualClosing = true;
    const ws = this.ws;
    this.ws = null;
    if (
      ws &&
      (ws.readyState === WebSocket.OPEN || ws.readyState === WebSocket.CONNECTING)
    ) {
      ws.close();
    }
    this.emitStatus({ kind: "disconnected" });
  }

  requestTail(lines = 400) {
    this.tailLines = lines;
    if (this.send({ action: "tail", lines })) {
      return true;
    }
    if (this.shouldRun) {
      this.connect();
    }
    return false;
  }

  send(payload: Payload) {
    if (!this.ws || this.ws.readyState !== WebSocket.OPEN) return false;
    this.ws.send(JSON.stringify(payload));
    return true;
  }

  subscribeMessages(listener: MessageListener) {
    this.messageListeners.add(listener);
    return () => this.messageListeners.delete(listener);
  }

  subscribeStatus(listener: StatusListener) {
    this.statusListeners.add(listener);
    listener(this.status);
    return () => this.statusListeners.delete(listener);
  }
}

const logSocketSingleton = new LogSocketSingleton();

export function getLogSocket() {
  return logSocketSingleton;
}
