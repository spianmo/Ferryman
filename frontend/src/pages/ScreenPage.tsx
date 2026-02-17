import { useEffect, useRef, useState } from "react";
import type {
  KeyboardEvent as ReactKeyboardEvent,
  MouseEvent as ReactMouseEvent,
  WheelEvent as ReactWheelEvent,
} from "react";
import { toast } from "../toast";
import { FiCast, FiMonitor, FiSend, FiPlay, FiPause, FiMaximize, FiMinimize } from "react-icons/fi";

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
  const pcRef = useRef<RTCPeerConnection | null>(null);
  const localStreamRef = useRef<MediaStream | null>(null);
  const remoteVideoRef = useRef<HTMLVideoElement | null>(null);
  const localVideoRef = useRef<HTMLVideoElement | null>(null);
  const nativeSurfaceRef = useRef<HTMLDivElement | null>(null);
  const lastMouseMoveAtRef = useRef(0);

  const [roomId, setRoomId] = useState("default-room");
  const [status, setStatus] = useState(t("terminal.disconnected"));
  const [peerId, setPeerId] = useState("");
  const [peers, setPeers] = useState<string[]>([]);
  const [nativeStreaming, setNativeStreaming] = useState(false);
  const [nativeFrameUrl, setNativeFrameUrl] = useState("");
  const [nativeInputFocused, setNativeInputFocused] = useState(false);
  const [nativeFullscreen, setNativeFullscreen] = useState(false);

  useEffect(() => {
    return () => {
      wsRef.current?.close();
      pcRef.current?.close();
      localStreamRef.current?.getTracks().forEach((track) => track.stop());
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
    if (!ws || ws.readyState !== WebSocket.OPEN) {
      return;
    }
    ws.send(JSON.stringify(payload));
  };

  const setupPc = () => {
    if (pcRef.current) {
      return pcRef.current;
    }

    const pc = new RTCPeerConnection({
      iceServers: [{ urls: "stun:stun.l.google.com:19302" }],
    });

    pc.onicecandidate = (event) => {
      if (!event.candidate) return;
      send({
        action: "signal",
        signal_type: "candidate",
        payload: JSON.stringify(event.candidate),
      });
    };

    pc.ontrack = (event) => {
      if (!remoteVideoRef.current) return;
      remoteVideoRef.current.srcObject = event.streams[0];
    };

    const stream = localStreamRef.current;
    if (stream) {
      stream.getTracks().forEach((track) => pc.addTrack(track, stream));
    }

    pcRef.current = pc;
    return pc;
  };

  const joinRoom = () => {
    if (wsRef.current && wsRef.current.readyState === WebSocket.OPEN) {
      send({ action: "join", room_id: roomId });
      return;
    }

    const ws = new WebSocket(wsUrl(session, "/ws/webrtc"));
    wsRef.current = ws;
    setStatus(t("terminal.connecting"));

    ws.onopen = () => {
      setStatus(t("terminal.connected"));
      send({ action: "join", room_id: roomId });
    };

    ws.onerror = () => {
      setStatus(t("terminal.failed"));
    };

    ws.onclose = () => {
      setStatus(t("terminal.closed"));
      setNativeStreaming(false);
    };

    ws.onmessage = async (event) => {
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
        return;
      }

      const eventType = String(payload.event ?? "");
      if (eventType === "joined") {
        setPeerId(String(payload.peer_id ?? ""));
        const existingPeers = (payload.peers ?? []) as string[];
        setPeers(existingPeers);
        setStatus(`${t("screen.join")} ${roomId}`);
      }

      if (eventType === "peer_join") {
        const id = String(payload.peer_id ?? "");
        setPeers((prev) => (prev.includes(id) ? prev : [...prev, id]));
      }

      if (eventType === "native_subscribed") {
        setNativeStreaming(true);
        setStatus(t("screen.native_start"));
      }

      if (eventType === "native_unsubscribed") {
        setNativeStreaming(false);
      }

      if (eventType === "native_frame") {
        const frame = String(payload.jpeg_base64 ?? "");
        if (frame.length > 0) {
          setNativeFrameUrl(`data:image/jpeg;base64,${frame}`);
        }
      }

      if (eventType === "signal") {
        const signalType = String(payload.signal_type ?? "");
        const fromPeerId = String(payload.from_peer_id ?? "");
        const raw = String(payload.payload ?? "{}");
        const signalData = JSON.parse(raw) as RTCSessionDescriptionInit | RTCIceCandidateInit;

        const pc = setupPc();

        if (signalType === "offer") {
          await pc.setRemoteDescription(signalData as RTCSessionDescriptionInit);
          const answer = await pc.createAnswer();
          await pc.setLocalDescription(answer);
          send({
            action: "signal",
            target_peer_id: fromPeerId,
            signal_type: "answer",
            payload: JSON.stringify(answer),
          });
        } else if (signalType === "answer") {
          await pc.setRemoteDescription(signalData as RTCSessionDescriptionInit);
        } else if (signalType === "candidate") {
          await pc.addIceCandidate(signalData as RTCIceCandidateInit);
        }
      }
    };
  };

  const shareScreen = async () => {
    let stream: MediaStream;
    try {
      stream = await navigator.mediaDevices.getDisplayMedia({ video: true, audio: false });
    } catch (err) {
      toast.error(String((err as Error)?.message ?? t("toast.request_failed")));
      return;
    }
    localStreamRef.current = stream;
    if (localVideoRef.current) {
      localVideoRef.current.srcObject = stream;
    }

    const pc = setupPc();
    stream.getTracks().forEach((track) => pc.addTrack(track, stream));

    if (peers.length > 0) {
      const targetPeer = peers[0];
      const offer = await pc.createOffer();
      await pc.setLocalDescription(offer);
      send({
        action: "signal",
        target_peer_id: targetPeer,
        signal_type: "offer",
        payload: JSON.stringify(offer),
      });
      setStatus(`${t("screen.share")} -> ${targetPeer}`);
    } else {
      setStatus(t("screen.share"));
    }
  };

  const startNative = () => {
    send({ action: "native_subscribe" });
  };

  const stopNative = () => {
    send({ action: "native_unsubscribe" });
    setNativeStreaming(false);
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
    <section className="rounded-3xl bg-white/70 p-4 shadow-soft ring-1 ring-slate-200/70 backdrop-blur dark:bg-slate-900/55 dark:ring-slate-800/70">
      <div className="flex flex-wrap items-start justify-between gap-3">
        <h2 className="inline-flex items-center gap-2 text-base font-semibold tracking-tight text-slate-900 dark:text-slate-50">
          <FiMonitor /> {t("screen.title")}
        </h2>

        <div className="flex w-full flex-col gap-2 sm:flex-row sm:items-center lg:w-auto lg:flex-row lg:flex-wrap">
          <input
            value={roomId}
            onChange={(event) => setRoomId(event.target.value)}
            className="h-10 w-full rounded-2xl border border-slate-200 bg-white px-3 text-sm text-slate-900 shadow-sm outline-none placeholder:text-slate-400 focus:border-slate-300 dark:border-slate-800 dark:bg-slate-950/40 dark:text-slate-50 dark:placeholder:text-slate-500 dark:focus:border-slate-700 sm:w-52"
          />
          <button
            className="inline-flex h-10 items-center justify-center gap-2 rounded-2xl bg-slate-900 px-4 text-sm font-semibold text-white shadow-sm hover:bg-slate-800 dark:bg-slate-50 dark:text-slate-900 dark:hover:bg-white"
            onClick={joinRoom}
          >
            <FiSend /> {t("screen.join")}
          </button>
          <button
            className="inline-flex h-10 items-center justify-center gap-2 rounded-2xl bg-slate-100 px-4 text-sm font-semibold text-slate-700 hover:bg-slate-200 dark:bg-slate-800 dark:text-slate-100 dark:hover:bg-slate-700"
            onClick={() => void shareScreen()}
          >
            <FiCast /> {t("screen.share")}
          </button>
          <button
            className="inline-flex h-10 items-center justify-center gap-2 rounded-2xl bg-slate-100 px-4 text-sm font-semibold text-slate-700 hover:bg-slate-200 dark:bg-slate-800 dark:text-slate-100 dark:hover:bg-slate-700"
            onClick={startNative}
          >
            <FiPlay /> {t("screen.native_start")}
          </button>
          <button
            className="inline-flex h-10 items-center justify-center gap-2 rounded-2xl bg-slate-100 px-4 text-sm font-semibold text-slate-700 hover:bg-slate-200 dark:bg-slate-800 dark:text-slate-100 dark:hover:bg-slate-700"
            onClick={stopNative}
          >
            <FiPause /> {t("screen.native_stop")}
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

      <div className="mt-3 flex flex-wrap items-center gap-2">
        <span className="inline-flex items-center gap-2 rounded-full bg-white/70 px-3 py-1 text-xs font-semibold text-slate-700 shadow-sm ring-1 ring-slate-200/60 dark:bg-slate-950/30 dark:text-slate-100 dark:ring-slate-800/70">
          {t("terminal.status")}: {status}
        </span>
        {peerId ? (
          <span className="inline-flex items-center gap-2 rounded-full bg-white/70 px-3 py-1 font-mono text-[11px] text-slate-600 shadow-sm ring-1 ring-slate-200/60 dark:bg-slate-950/30 dark:text-slate-200 dark:ring-slate-800/70">
            {t("screen.me")}: {peerId}
          </span>
        ) : null}
        <span className="inline-flex items-center gap-2 rounded-full bg-white/70 px-3 py-1 text-xs font-semibold text-slate-700 shadow-sm ring-1 ring-slate-200/60 dark:bg-slate-950/30 dark:text-slate-100 dark:ring-slate-800/70">
          {t("screen.peers")}: {peers.length}
        </span>
        <span className="inline-flex items-center gap-2 rounded-full bg-white/70 px-3 py-1 text-xs font-semibold text-slate-700 shadow-sm ring-1 ring-slate-200/60 dark:bg-slate-950/30 dark:text-slate-100 dark:ring-slate-800/70">
          {t("screen.native")}: {nativeStreaming ? t("common.on") : t("common.off")}
        </span>
        <span className="inline-flex items-center gap-2 rounded-full bg-white/70 px-3 py-1 text-xs font-semibold text-slate-700 shadow-sm ring-1 ring-slate-200/60 dark:bg-slate-950/30 dark:text-slate-100 dark:ring-slate-800/70">
          {t("screen.keyboard")}: {nativeInputFocused ? t("common.on") : t("common.off")}
        </span>
      </div>

      <div className="mt-4 grid grid-cols-1 gap-4 xl:grid-cols-2">
        <div className="rounded-3xl bg-white/70 p-3 shadow-sm ring-1 ring-slate-200/60 dark:bg-slate-950/20 dark:ring-slate-800/70">
          <div className="mb-2 text-xs font-semibold text-slate-500 dark:text-slate-400">
            {t("screen.webrtc_source")}
          </div>
          <video ref={localVideoRef} autoPlay playsInline muted className="h-[260px] w-full rounded-2xl bg-slate-950 object-contain" />
        </div>
        <div className="rounded-3xl bg-white/70 p-3 shadow-sm ring-1 ring-slate-200/60 dark:bg-slate-950/20 dark:ring-slate-800/70">
          <div className="mb-2 text-xs font-semibold text-slate-500 dark:text-slate-400">
            {t("screen.webrtc_sink")}
          </div>
          <video ref={remoteVideoRef} autoPlay playsInline className="h-[260px] w-full rounded-2xl bg-slate-950 object-contain" />
        </div>
      </div>

      <div className="mt-4">
        <div className="mb-2 text-xs font-semibold text-slate-500 dark:text-slate-400">
          {t("screen.native_stream")}
        </div>
        <div
          ref={nativeSurfaceRef}
          className={cn(
            "native-surface rounded-2xl bg-slate-950 shadow-sm ring-1 ring-slate-200/70 outline-none dark:ring-slate-800/70",
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
              className="h-[320px] w-full select-none rounded-2xl object-contain"
              alt="native screen frame"
              onMouseMove={onNativeMouseMove}
              onClick={onNativeClick}
              onWheel={onNativeWheel}
              onContextMenu={(event) => event.preventDefault()}
              draggable={false}
            />
          ) : (
            <div className="grid h-[320px] place-items-center rounded-2xl bg-slate-950 text-sm text-slate-300">
              {nativeStreaming ? t("screen.native_wait") : t("screen.native_start_hint")}
            </div>
          )}
        </div>
      </div>
    </section>
  );
}
