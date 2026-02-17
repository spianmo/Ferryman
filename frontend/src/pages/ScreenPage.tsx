import { useEffect, useRef, useState } from "react";
import type {
  KeyboardEvent as ReactKeyboardEvent,
  MouseEvent as ReactMouseEvent,
  WheelEvent as ReactWheelEvent,
} from "react";
import { FiMaximize, FiMinimize, FiMonitor, FiPause, FiPlay } from "react-icons/fi";

import { emitUnauthorized, wsUrl } from "../api/client";
import { useI18n } from "../i18n";
import type { SessionInfo } from "../types";
import { cn } from "../util/cn";

type Props = {
  session: SessionInfo;
};

export default function ScreenPage({ session }: Props) {
  const { t } = useI18n();
  const wsRef = useRef<WebSocket | null>(null);
  const nativeSurfaceRef = useRef<HTMLDivElement | null>(null);
  const lastMouseMoveAtRef = useRef(0);
  const nativeWantedRef = useRef(false);
  const [dotState, setDotState] = useState<"idle" | "connecting" | "active" | "error">("idle");

  const [status, setStatus] = useState(t("terminal.disconnected"));
  const [nativeStreaming, setNativeStreaming] = useState(false);
  const [nativeFrameUrl, setNativeFrameUrl] = useState("");
  const [nativeInputFocused, setNativeInputFocused] = useState(false);
  const [nativeFullscreen, setNativeFullscreen] = useState(false);

  useEffect(() => {
    return () => {
      wsRef.current?.close();
      wsRef.current = null;
    };
  }, []);

  useEffect(() => {
    const onFullscreenChange = () => {
      setNativeFullscreen(document.fullscreenElement === nativeSurfaceRef.current);
    };
    document.addEventListener("fullscreenchange", onFullscreenChange);
    return () => document.removeEventListener("fullscreenchange", onFullscreenChange);
  }, []);

  const send = (payload: Record<string, unknown>) => {
    const ws = wsRef.current;
    if (!ws || ws.readyState !== WebSocket.OPEN) return;
    ws.send(JSON.stringify(payload));
  };

  const connectWs = () => {
    const current = wsRef.current;
    if (
      current &&
      (current.readyState === WebSocket.OPEN || current.readyState === WebSocket.CONNECTING)
    ) {
      return;
    }

    const ws = new WebSocket(wsUrl(session, "/ws/webrtc"));
    wsRef.current = ws;
    setStatus(t("terminal.connecting"));
    setDotState("connecting");

    ws.onopen = () => {
      setStatus(t("terminal.connected"));
      if (!nativeWantedRef.current) {
        setDotState("idle");
      }
      if (nativeWantedRef.current) {
        send({ action: "native_subscribe" });
      }
    };

    ws.onerror = () => {
      setStatus(t("terminal.failed"));
      setDotState("error");
    };

    ws.onclose = () => {
      if (wsRef.current === ws) {
        wsRef.current = null;
      }
      nativeWantedRef.current = false;
      setNativeStreaming(false);
      setNativeFrameUrl("");
      setStatus(t("terminal.closed"));
      setDotState("idle");
    };

    ws.onmessage = (event) => {
      let payload: Record<string, unknown>;
      try {
        payload = JSON.parse(event.data as string) as Record<string, unknown>;
      } catch {
        return;
      }

      if (payload.ok === false) {
        if (String(payload.code ?? "") === "unauthorized") {
          const reason = payload.error;
          emitUnauthorized({ reason: typeof reason === "string" ? reason : undefined });
        }
        setStatus(String(payload.error ?? t("toast.request_failed")));
        setDotState("error");
        return;
      }

      const eventType = String(payload.event ?? "");
      if (eventType === "native_subscribed") {
        nativeWantedRef.current = true;
        setNativeStreaming(true);
        setStatus(t("screen.native_start"));
        setDotState("active");
        return;
      }
      if (eventType === "native_unsubscribed") {
        nativeWantedRef.current = false;
        setNativeStreaming(false);
        setNativeFrameUrl("");
        setStatus(t("terminal.connected"));
        setDotState("idle");
        return;
      }
      if (eventType === "native_frame") {
        const frame = String(payload.jpeg_base64 ?? "");
        if (frame.length > 0) {
          setNativeFrameUrl(`data:image/jpeg;base64,${frame}`);
        }
      }
    };
  };

  const toggleNativeStreaming = () => {
    const nextWanted = !nativeWantedRef.current;
    nativeWantedRef.current = nextWanted;
    setDotState(nextWanted ? "connecting" : "idle");

    const ws = wsRef.current;
    if (!ws || ws.readyState === WebSocket.CLOSED || ws.readyState === WebSocket.CLOSING) {
      if (!nextWanted) {
        setNativeStreaming(false);
        setNativeFrameUrl("");
        setDotState("idle");
        return;
      }
      connectWs();
      return;
    }

    if (ws.readyState === WebSocket.OPEN) {
      send({ action: nextWanted ? "native_subscribe" : "native_unsubscribe" });
      if (!nextWanted) {
        setNativeStreaming(false);
        setNativeFrameUrl("");
        setDotState("idle");
      }
      return;
    }

    if (!nextWanted) {
      setNativeStreaming(false);
      setNativeFrameUrl("");
      setDotState("idle");
    }
  };

  const toggleNativeFullscreen = async () => {
    const surface = nativeSurfaceRef.current;
    if (!surface) return;
    try {
      if (document.fullscreenElement) {
        await document.exitFullscreen();
      } else {
        await surface.requestFullscreen();
      }
    } catch {
      // Ignore fullscreen errors (e.g. unsupported platform/browser).
    }
  };

  const sendInput = (type: string, payload: Record<string, unknown>) => {
    send({ action: "input_event", type, payload });
  };

  const onNativeMouseMove = (event: ReactMouseEvent<HTMLImageElement>) => {
    if (!nativeStreaming) return;

    const now = Date.now();
    if (now - lastMouseMoveAtRef.current < 20) {
      return;
    }
    lastMouseMoveAtRef.current = now;

    const rect = event.currentTarget.getBoundingClientRect();
    sendInput("mouse_move", {
      x: event.clientX - rect.left,
      y: event.clientY - rect.top,
      width: rect.width,
      height: rect.height,
    });
  };

  const onNativeClick = (event: ReactMouseEvent<HTMLImageElement>) => {
    if (!nativeStreaming) return;
    nativeSurfaceRef.current?.focus();
    sendInput("mouse_click", {
      button: event.button,
    });
  };

  const onNativeWheel = (event: ReactWheelEvent<HTMLImageElement>) => {
    if (!nativeStreaming) return;
    event.preventDefault();
    sendInput("mouse_wheel", {
      delta_y: Math.round(event.deltaY),
    });
  };

  const sendKeyEvent = (type: "key_down" | "key_up", event: ReactKeyboardEvent<HTMLDivElement>) => {
    if (!nativeStreaming) return;
    sendInput(type, {
      key: event.key,
      code: event.code,
      location: event.location,
      repeat: event.repeat,
      shiftKey: event.shiftKey,
      ctrlKey: event.ctrlKey,
      altKey: event.altKey,
      metaKey: event.metaKey,
    });
    event.preventDefault();
  };

  const onNativeKeyDown = (event: ReactKeyboardEvent<HTMLDivElement>) => {
    sendKeyEvent("key_down", event);
  };

  const onNativeKeyUp = (event: ReactKeyboardEvent<HTMLDivElement>) => {
    sendKeyEvent("key_up", event);
  };

  return (
    <section className="flex h-full min-h-[460px] flex-col rounded-3xl bg-white/70 p-4 shadow-soft ring-1 ring-slate-200/70 backdrop-blur dark:bg-slate-900/55 dark:ring-slate-800/70">
      <div className="flex flex-wrap items-start justify-between gap-3">
        <div>
          <div className="inline-flex items-center gap-2">
            <h2 className="inline-flex items-center gap-2 text-base font-semibold tracking-tight text-slate-900 dark:text-slate-50">
              <FiMonitor />
              {t("screen.title")}
            </h2>
            <span className="inline-flex h-2.5 w-2.5 shrink-0 items-center justify-center">
              <span
                className={cn(
                  "block h-2.5 w-2.5 rounded-full border border-white/90 transition-all duration-300 ease-out dark:border-slate-900/90",
                  dotState === "active" &&
                    "scale-100 bg-emerald-500 shadow-[0_0_0_3px_rgba(16,185,129,0.20)]",
                  dotState === "connecting" && "animate-pulse scale-95 bg-amber-400",
                  dotState === "error" && "scale-100 bg-rose-500 shadow-[0_0_0_3px_rgba(244,63,94,0.16)]",
                  dotState === "idle" && "scale-90 bg-slate-400 dark:bg-slate-500"
                )}
                title={status}
                aria-label={`${t("terminal.status")}: ${status}`}
              />
            </span>
          </div>
          <div className="mt-1 text-xs font-semibold text-slate-500 dark:text-slate-400">
            {t("screen.native_stream")}
          </div>
        </div>

        <div className="flex w-full flex-col gap-2 sm:flex-row sm:items-center lg:w-auto lg:flex-row lg:flex-wrap">
          <button
            className="inline-flex h-10 items-center justify-center gap-2 rounded-2xl bg-slate-900 px-4 text-sm font-semibold text-white shadow-sm hover:bg-slate-800 dark:bg-slate-50 dark:text-slate-900 dark:hover:bg-white"
            onClick={toggleNativeStreaming}
          >
            {nativeStreaming ? <FiPause /> : <FiPlay />}{" "}
            {nativeStreaming ? t("screen.native_stop") : t("screen.native_start")}
          </button>
          <button
            className="inline-flex h-10 items-center justify-center gap-2 rounded-2xl bg-slate-100 px-4 text-sm font-semibold text-slate-700 hover:bg-slate-200 dark:bg-slate-800 dark:text-slate-100 dark:hover:bg-slate-700"
            onClick={() => void toggleNativeFullscreen()}
          >
            {nativeFullscreen ? <FiMinimize /> : <FiMaximize />}{" "}
            {nativeFullscreen ? t("screen.exit_fullscreen") : t("screen.fullscreen")}
          </button>
        </div>
      </div>

      <div className="mt-4 flex min-h-0 flex-1 flex-col">
        <div
          ref={nativeSurfaceRef}
          className={cn(
            "native-surface min-h-0 flex-1 rounded-2xl bg-slate-950 shadow-sm ring-1 ring-slate-200/70 outline-none dark:ring-slate-800/70",
            nativeInputFocused && "ring-2 ring-slate-900/70 dark:ring-slate-50/70"
          )}
          tabIndex={0}
          onMouseDown={() => nativeSurfaceRef.current?.focus()}
          onFocus={() => setNativeInputFocused(true)}
          onBlur={() => setNativeInputFocused(false)}
          onKeyDown={onNativeKeyDown}
          onKeyUp={onNativeKeyUp}
        >
          {nativeFrameUrl ? (
            <img
              src={nativeFrameUrl}
              className="block h-full w-full select-none rounded-2xl object-contain"
              alt="native screen frame"
              onMouseMove={onNativeMouseMove}
              onClick={onNativeClick}
              onWheel={onNativeWheel}
              onContextMenu={(event) => event.preventDefault()}
              draggable={false}
            />
          ) : (
            <div className="grid h-full w-full place-items-center rounded-2xl bg-slate-950 text-sm text-slate-300">
              {nativeStreaming ? t("screen.native_wait") : t("screen.native_start_hint")}
            </div>
          )}
        </div>
      </div>
    </section>
  );
}
