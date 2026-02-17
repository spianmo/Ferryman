import { emitUnauthorized, wsUrl } from "../api/client";
import type { SessionInfo } from "../types";

export type TerminalSocketStatus = {
  kind: "disconnected" | "connecting" | "connected" | "closed" | "failed" | "error";
  message?: string;
};

type Payload = Record<string, unknown>;
type MessageListener = (payload: Payload) => void;
type StatusListener = (status: TerminalSocketStatus) => void;

class TerminalSocketSingleton {
  private ws: WebSocket | null = null;
  private session: SessionInfo | null = null;
  private sessionKey = "";
  private status: TerminalSocketStatus = { kind: "disconnected" };
  private messageListeners = new Set<MessageListener>();
  private statusListeners = new Set<StatusListener>();
  private manualClosing = false;

  private makeSessionKey(session: SessionInfo) {
    return `${window.location.origin}:${session.token}`;
  }

  private emitStatus(next: TerminalSocketStatus) {
    this.status = next;
    this.statusListeners.forEach((listener) => listener(next));
  }

  private emitMessage(payload: Payload) {
    this.messageListeners.forEach((listener) => listener(payload));
  }

  private ensureSession(session: SessionInfo) {
    const nextKey = this.makeSessionKey(session);
    if (nextKey === this.sessionKey) return;
    this.disconnect();
    this.session = session;
    this.sessionKey = nextKey;
  }

  connect(session: SessionInfo) {
    this.ensureSession(session);

    if (
      this.ws &&
      (this.ws.readyState === WebSocket.OPEN || this.ws.readyState === WebSocket.CONNECTING)
    ) {
      return;
    }
    if (!this.session) return;

    const ws = new WebSocket(wsUrl(this.session, "/ws/terminal"));
    this.ws = ws;
    this.manualClosing = false;
    this.emitStatus({ kind: "connecting" });

    ws.onopen = () => {
      if (this.ws !== ws) return;
      this.emitStatus({ kind: "connected" });
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
    };
  }

  disconnect() {
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

const terminalSocketSingleton = new TerminalSocketSingleton();

export function getTerminalSocket() {
  return terminalSocketSingleton;
}
