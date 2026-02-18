import { useEffect, useRef, useState } from "react";
import type {
  ChangeEvent as ReactChangeEvent,
  KeyboardEvent as ReactKeyboardEvent,
  MouseEvent as ReactMouseEvent,
  WheelEvent as ReactWheelEvent,
} from "react";
import { FiMaximize, FiMinimize, FiMonitor, FiPause, FiPlay } from "react-icons/fi";

import { emitUnauthorized, getScreenCapabilities, wsUrl } from "../api/client";
import { useI18n } from "../i18n";
import type { SessionInfo } from "../types";
import { cn } from "../util/cn";

type Props = {
  session: SessionInfo;
};

type NativeCodec = "jpeg" | "h264";
type ResolutionTier = "full" | "balanced" | "performance";
type BitrateTier = "sd" | "hd" | "uhd";

const NATIVE_CODEC_STORAGE_KEY = "ferryman.screen.native_codec";
const NATIVE_FPS_STORAGE_KEY = "ferryman.screen.native_fps";
const NATIVE_RESOLUTION_STORAGE_KEY = "ferryman.screen.native_resolution_tier";
const NATIVE_BITRATE_STORAGE_KEY = "ferryman.screen.native_bitrate_tier";
const NATIVE_FPS_OPTIONS = [5, 8, 10, 12, 15, 24, 30] as const;
const EPOCH_MS_MIN = 1_000_000_000_000;
const NATIVE_BINARY_MAGIC = 0x314d5246;
const NATIVE_BINARY_HEADER_BYTES = 36;

const DEFAULT_RESOLUTION_OPTIONS: Array<{ id: ResolutionTier; scalePercent: number }> = [
  { id: "full", scalePercent: 100 },
  { id: "balanced", scalePercent: 75 },
  { id: "performance", scalePercent: 50 },
];
const DEFAULT_BITRATE_OPTIONS: Array<{ id: BitrateTier; bitrateBps: number }> = [
  { id: "sd", bitrateBps: 1_500_000 },
  { id: "hd", bitrateBps: 3_000_000 },
  { id: "uhd", bitrateBps: 6_000_000 },
];

type NativeBinaryFrame = {
  codec: NativeCodec;
  keyframe: boolean;
  sequence: number;
  capturedAtMs: number;
  width: number;
  height: number;
  payload: Uint8Array;
};

function decodeBase64Bytes(value: string): Uint8Array {
  const binary = atob(value);
  const bytes = new Uint8Array(binary.length);
  for (let idx = 0; idx < binary.length; idx += 1) {
    bytes[idx] = binary.charCodeAt(idx);
  }
  return bytes;
}

function readUint64LE(view: DataView, offset: number): number {
  const lo = view.getUint32(offset, true);
  const hi = view.getUint32(offset + 4, true);
  return hi * 2 ** 32 + lo;
}

function isResolutionTier(value: string): value is ResolutionTier {
  return value === "full" || value === "balanced" || value === "performance";
}

function isBitrateTier(value: string): value is BitrateTier {
  return value === "sd" || value === "hd" || value === "uhd";
}

function parseNativeBinaryFrame(buffer: ArrayBuffer): NativeBinaryFrame | null {
  if (buffer.byteLength < NATIVE_BINARY_HEADER_BYTES) {
    return null;
  }
  const view = new DataView(buffer);
  const magic = view.getUint32(0, true);
  if (magic !== NATIVE_BINARY_MAGIC) {
    return null;
  }

  const codecByte = view.getUint8(4);
  let codec: NativeCodec | null = null;
  if (codecByte === 2) {
    codec = "h264";
  } else if (codecByte === 1) {
    codec = "jpeg";
  }
  if (codec === null) {
    return null;
  }
  const keyframe = (view.getUint8(5) & 1) === 1;
  const sequence = readUint64LE(view, 8);
  const capturedAtMs = readUint64LE(view, 16);
  const width = view.getUint32(24, true);
  const height = view.getUint32(28, true);
  const payloadSize = view.getUint32(32, true);
  if (payloadSize + NATIVE_BINARY_HEADER_BYTES > buffer.byteLength) {
    return null;
  }
  const payload = new Uint8Array(buffer, NATIVE_BINARY_HEADER_BYTES, payloadSize);
  return {
    codec,
    keyframe,
    sequence,
    capturedAtMs,
    width,
    height,
    payload,
  };
}

export default function ScreenPage({ session }: Props) {
  const { t } = useI18n();
  const wsRef = useRef<WebSocket | null>(null);
  const nativeSurfaceRef = useRef<HTMLDivElement | null>(null);
  const nativeCanvasRef = useRef<HTMLCanvasElement | null>(null);
  const nativeDecoderRef = useRef<VideoDecoder | null>(null);
  const nativeDecoderCodecRef = useRef("avc1.42E01E");
  const nativeDecoderWidthRef = useRef(0);
  const nativeDecoderHeightRef = useRef(0);
  const nativeDecoderTimestampRef = useRef(0);
  const nativeDecoderSawKeyRef = useRef(false);
  const nativeHasFrameRef = useRef(false);
  const nativeFrameObjectUrlRef = useRef<string | null>(null);
  const nativeStatsRef = useRef({
    fpsWindowStartMs: 0,
    fpsFrameCount: 0,
    lastFrameAtMs: 0,
    lastPublishAtMs: 0,
    hasLatency: false,
    latencyEmaMs: 0,
  });
  const lastMouseMoveAtRef = useRef(0);
  const nativeWantedRef = useRef(false);
  const [dotState, setDotState] = useState<"idle" | "connecting" | "active" | "error">("idle");

  const [status, setStatus] = useState(t("terminal.disconnected"));
  const [nativeStreaming, setNativeStreaming] = useState(false);
  const [nativeCodec, setNativeCodec] = useState<NativeCodec>("jpeg");
  const [preferredNativeCodec, setPreferredNativeCodec] = useState<NativeCodec>(() => {
    const h264Supported = typeof window !== "undefined" && typeof VideoDecoder !== "undefined";
    if (typeof window !== "undefined") {
      const stored = window.localStorage.getItem(NATIVE_CODEC_STORAGE_KEY);
      if (stored === "jpeg") {
        return "jpeg";
      }
      if (stored === "h264" && h264Supported) {
        return "h264";
      }
    }
    return h264Supported ? "h264" : "jpeg";
  });
  const [preferredNativeFps, setPreferredNativeFps] = useState<number>(() => {
    if (typeof window === "undefined") {
      return 8;
    }
    const stored = window.localStorage.getItem(NATIVE_FPS_STORAGE_KEY);
    if (!stored) {
      return 8;
    }
    const parsed = Number.parseInt(stored, 10);
    return NATIVE_FPS_OPTIONS.includes(parsed as (typeof NATIVE_FPS_OPTIONS)[number]) ? parsed : 8;
  });
  const [resolutionOptions, setResolutionOptions] = useState(DEFAULT_RESOLUTION_OPTIONS);
  const [bitrateOptions, setBitrateOptions] = useState(DEFAULT_BITRATE_OPTIONS);
  const [preferredResolutionTier, setPreferredResolutionTier] = useState<ResolutionTier>(() => {
    if (typeof window === "undefined") {
      return "balanced";
    }
    const stored = window.localStorage.getItem(NATIVE_RESOLUTION_STORAGE_KEY) ?? "";
    return isResolutionTier(stored) ? stored : "balanced";
  });
  const [preferredBitrateTier, setPreferredBitrateTier] = useState<BitrateTier>(() => {
    if (typeof window === "undefined") {
      return "hd";
    }
    const stored = window.localStorage.getItem(NATIVE_BITRATE_STORAGE_KEY) ?? "";
    return isBitrateTier(stored) ? stored : "hd";
  });
  const [nativeHasFrame, setNativeHasFrame] = useState(false);
  const [nativeFrameUrl, setNativeFrameUrl] = useState("");
  const [nativeActualFps, setNativeActualFps] = useState(0);
  const [nativeLatencyMs, setNativeLatencyMs] = useState<number | null>(null);
  const [nativeInputFocused, setNativeInputFocused] = useState(false);
  const [nativeFullscreen, setNativeFullscreen] = useState(false);

  useEffect(() => {
    return () => {
      wsRef.current?.close();
      wsRef.current = null;
      revokeNativeFrameObjectUrl();
    };
  }, []);

  useEffect(() => {
    const onFullscreenChange = () => {
      setNativeFullscreen(document.fullscreenElement === nativeSurfaceRef.current);
    };
    document.addEventListener("fullscreenchange", onFullscreenChange);
    return () => document.removeEventListener("fullscreenchange", onFullscreenChange);
  }, []);

  const supportsH264Decode = () => {
    return typeof window !== "undefined" && typeof VideoDecoder !== "undefined";
  };

  const revokeNativeFrameObjectUrl = () => {
    if (!nativeFrameObjectUrlRef.current) return;
    URL.revokeObjectURL(nativeFrameObjectUrlRef.current);
    nativeFrameObjectUrlRef.current = null;
  };

  const resetNativeFrameState = () => {
    nativeHasFrameRef.current = false;
    nativeStatsRef.current.fpsWindowStartMs = 0;
    nativeStatsRef.current.fpsFrameCount = 0;
    nativeStatsRef.current.lastFrameAtMs = 0;
    nativeStatsRef.current.lastPublishAtMs = 0;
    nativeStatsRef.current.hasLatency = false;
    nativeStatsRef.current.latencyEmaMs = 0;
    revokeNativeFrameObjectUrl();
    setNativeHasFrame(false);
    setNativeFrameUrl("");
    setNativeActualFps(0);
    setNativeLatencyMs(null);
  };

  const recordNativeFrameStats = (capturedAtMs?: number) => {
    const perfNow = performance.now();
    const wallNow = Date.now();
    const stats = nativeStatsRef.current;

    if (stats.fpsWindowStartMs <= 0) {
      stats.fpsWindowStartMs = perfNow;
    }
    stats.fpsFrameCount += 1;
    stats.lastFrameAtMs = perfNow;

    if (Number.isFinite(capturedAtMs) && (capturedAtMs ?? 0) > 0) {
      const sampleLatency = Math.max(0, wallNow - (capturedAtMs as number));
      if (!stats.hasLatency) {
        stats.latencyEmaMs = sampleLatency;
        stats.hasLatency = true;
        setNativeLatencyMs(sampleLatency);
      } else {
        stats.latencyEmaMs = stats.latencyEmaMs * 0.8 + sampleLatency * 0.2;
      }
    }

    const elapsedMs = perfNow - stats.fpsWindowStartMs;
    if (elapsedMs < 500) {
      return;
    }

    const fps = elapsedMs > 0 ? (stats.fpsFrameCount * 1000) / elapsedMs : 0;
    setNativeActualFps(Number.isFinite(fps) ? fps : 0);
    setNativeLatencyMs(stats.hasLatency ? stats.latencyEmaMs : null);

    stats.fpsWindowStartMs = perfNow;
    stats.fpsFrameCount = 0;
    stats.lastPublishAtMs = perfNow;
  };

  const closeNativeDecoder = () => {
    const decoder = nativeDecoderRef.current;
    nativeDecoderRef.current = null;
    nativeDecoderWidthRef.current = 0;
    nativeDecoderHeightRef.current = 0;
    nativeDecoderCodecRef.current = "avc1.42E01E";
    nativeDecoderTimestampRef.current = 0;
    nativeDecoderSawKeyRef.current = false;
    if (!decoder) return;
    try {
      decoder.close();
    } catch {
      // Ignore decoder close errors.
    }
  };

  useEffect(() => {
    return () => {
      closeNativeDecoder();
    };
  }, []);

  useEffect(() => {
    if (typeof window !== "undefined") {
      window.localStorage.setItem(NATIVE_CODEC_STORAGE_KEY, preferredNativeCodec);
    }
  }, [preferredNativeCodec]);

  useEffect(() => {
    if (typeof window !== "undefined") {
      window.localStorage.setItem(NATIVE_FPS_STORAGE_KEY, String(preferredNativeFps));
    }
  }, [preferredNativeFps]);

  useEffect(() => {
    if (typeof window !== "undefined") {
      window.localStorage.setItem(NATIVE_RESOLUTION_STORAGE_KEY, preferredResolutionTier);
    }
  }, [preferredResolutionTier]);

  useEffect(() => {
    if (typeof window !== "undefined") {
      window.localStorage.setItem(NATIVE_BITRATE_STORAGE_KEY, preferredBitrateTier);
    }
  }, [preferredBitrateTier]);

  useEffect(() => {
    let active = true;
    void (async () => {
      try {
        const response = await getScreenCapabilities(session.token);
        if (!active || !response.ok) {
          return;
        }
        const capabilities = response.capabilities;
        if (!capabilities || typeof capabilities !== "object") {
          return;
        }

        const caps = capabilities as Record<string, unknown>;
        const resolutionRaw = caps.native_resolution_tiers;
        if (Array.isArray(resolutionRaw)) {
          const nextResolution = resolutionRaw
            .map((item) => {
              if (!item || typeof item !== "object") return null;
              const record = item as Record<string, unknown>;
              const idRaw = String(record.id ?? "");
              if (!isResolutionTier(idRaw)) return null;
              const scalePercent = Number(record.scale_percent ?? 0);
              if (!Number.isFinite(scalePercent) || scalePercent <= 0) return null;
              return { id: idRaw, scalePercent: Math.round(scalePercent) };
            })
            .filter((item): item is { id: ResolutionTier; scalePercent: number } => item !== null);
          if (nextResolution.length > 0) {
            setResolutionOptions(nextResolution);
          }
        }

        const bitrateRaw = caps.native_bitrate_tiers;
        if (Array.isArray(bitrateRaw)) {
          const nextBitrate = bitrateRaw
            .map((item) => {
              if (!item || typeof item !== "object") return null;
              const record = item as Record<string, unknown>;
              const idRaw = String(record.id ?? "");
              if (!isBitrateTier(idRaw)) return null;
              const bitrateBps = Number(record.bitrate_bps ?? 0);
              if (!Number.isFinite(bitrateBps) || bitrateBps <= 0) return null;
              return { id: idRaw, bitrateBps: Math.round(bitrateBps) };
            })
            .filter((item): item is { id: BitrateTier; bitrateBps: number } => item !== null);
          if (nextBitrate.length > 0) {
            setBitrateOptions(nextBitrate);
          }
        }
      } catch {
        // Ignore capability fetch errors; local defaults remain available.
      }
    })();
    return () => {
      active = false;
    };
  }, [session.token]);

  useEffect(() => {
    if (!nativeStreaming) {
      return;
    }
    const timer = window.setInterval(() => {
      const stats = nativeStatsRef.current;
      if (stats.lastFrameAtMs <= 0) {
        return;
      }
      if (performance.now() - stats.lastFrameAtMs > 1500) {
        setNativeActualFps(0);
      }
    }, 500);
    return () => window.clearInterval(timer);
  }, [nativeStreaming]);

  useEffect(() => {
    if (!resolutionOptions.some((item) => item.id === preferredResolutionTier)) {
      setPreferredResolutionTier(resolutionOptions[0]?.id ?? "balanced");
    }
  }, [preferredResolutionTier, resolutionOptions]);

  useEffect(() => {
    if (!bitrateOptions.some((item) => item.id === preferredBitrateTier)) {
      setPreferredBitrateTier(bitrateOptions[0]?.id ?? "hd");
    }
  }, [preferredBitrateTier, bitrateOptions]);

  const ensureH264Decoder = (codedWidth: number, codedHeight: number, codec: string) => {
    if (!supportsH264Decode()) {
      return false;
    }
    if (
      nativeDecoderRef.current &&
      nativeDecoderCodecRef.current === codec &&
      nativeDecoderWidthRef.current === codedWidth &&
      nativeDecoderHeightRef.current === codedHeight
    ) {
      return true;
    }

    closeNativeDecoder();

    const decoder = new VideoDecoder({
      output: (frame) => {
        const canvas = nativeCanvasRef.current;
        if (!canvas) {
          frame.close();
          return;
        }
        if (canvas.width !== frame.displayWidth || canvas.height !== frame.displayHeight) {
          canvas.width = frame.displayWidth;
          canvas.height = frame.displayHeight;
        }
        const ctx = canvas.getContext("2d");
        ctx?.drawImage(frame, 0, 0, canvas.width, canvas.height);
        const frameTimestamp = Number(frame.timestamp ?? 0);
        const capturedAtMsCandidate = Number.isFinite(frameTimestamp) && frameTimestamp > 0
          ? frameTimestamp / 1000
          : undefined;
        const capturedAtMs = capturedAtMsCandidate && capturedAtMsCandidate >= EPOCH_MS_MIN
          ? capturedAtMsCandidate
          : undefined;
        recordNativeFrameStats(capturedAtMs);
        frame.close();
        if (!nativeHasFrameRef.current) {
          nativeHasFrameRef.current = true;
          setNativeHasFrame(true);
        }
      },
      error: () => {
        setStatus(t("terminal.failed"));
        setDotState("error");
      },
    });

    try {
      decoder.configure({
        codec,
        codedWidth,
        codedHeight,
        optimizeForLatency: true,
      });
    } catch {
      decoder.close();
      return false;
    }

    nativeDecoderRef.current = decoder;
    nativeDecoderCodecRef.current = codec;
    nativeDecoderWidthRef.current = codedWidth;
    nativeDecoderHeightRef.current = codedHeight;
    nativeDecoderTimestampRef.current = 0;
    nativeDecoderSawKeyRef.current = false;
    return true;
  };

  const send = (payload: Record<string, unknown>) => {
    const ws = wsRef.current;
    if (!ws || ws.readyState !== WebSocket.OPEN) return;
    ws.send(JSON.stringify(payload));
  };

  const requestedNativeCodec = (): NativeCodec => {
    if (preferredNativeCodec === "h264" && supportsH264Decode()) {
      return "h264";
    }
    return "jpeg";
  };

  const requestedNativeFps = (): number => {
    return NATIVE_FPS_OPTIONS.includes(preferredNativeFps as (typeof NATIVE_FPS_OPTIONS)[number])
      ? preferredNativeFps
      : 8;
  };

  const requestedResolutionTier = (): ResolutionTier => {
    return resolutionOptions.some((item) => item.id === preferredResolutionTier)
      ? preferredResolutionTier
      : (resolutionOptions[0]?.id ?? "balanced");
  };

  const requestedBitrateTier = (): BitrateTier => {
    return bitrateOptions.some((item) => item.id === preferredBitrateTier)
      ? preferredBitrateTier
      : (bitrateOptions[0]?.id ?? "hd");
  };

  const handleNativeBinaryFrameBuffer = (buffer: ArrayBuffer) => {
    const frame = parseNativeBinaryFrame(buffer);
    if (!frame) return;

    if (frame.codec === "h264") {
      if (!ensureH264Decoder(frame.width, frame.height, "avc1.42E01E")) {
        setStatus(t("terminal.failed"));
        setDotState("error");
        return;
      }

      if (!frame.keyframe && !nativeDecoderSawKeyRef.current) {
        return;
      }
      if (frame.keyframe) {
        nativeDecoderSawKeyRef.current = true;
      }

      let timestamp = frame.capturedAtMs > 0
        ? Math.trunc(frame.capturedAtMs * 1000)
        : nativeDecoderTimestampRef.current + 40_000;
      if (timestamp <= nativeDecoderTimestampRef.current) {
        timestamp = nativeDecoderTimestampRef.current + 1;
      }
      nativeDecoderTimestampRef.current = timestamp;

      const decoder = nativeDecoderRef.current;
      if (!decoder) return;

      try {
        decoder.decode(
          new EncodedVideoChunk({
            type: frame.keyframe ? "key" : "delta",
            timestamp,
            data: frame.payload,
          })
        );
        setNativeCodec("h264");
      } catch {
        setStatus(t("terminal.failed"));
        setDotState("error");
      }
      return;
    }

    closeNativeDecoder();
    setNativeCodec("jpeg");
    revokeNativeFrameObjectUrl();
    const blob = new Blob([frame.payload], { type: "image/jpeg" });
    const objectUrl = URL.createObjectURL(blob);
    nativeFrameObjectUrlRef.current = objectUrl;
    setNativeFrameUrl(objectUrl);
    recordNativeFrameStats(frame.capturedAtMs >= EPOCH_MS_MIN ? frame.capturedAtMs : undefined);
    if (!nativeHasFrameRef.current) {
      nativeHasFrameRef.current = true;
      setNativeHasFrame(true);
    }
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
    ws.binaryType = "arraybuffer";
    wsRef.current = ws;
    setStatus(t("terminal.connecting"));
    setDotState("connecting");

    ws.onopen = () => {
      setStatus(t("terminal.connected"));
      if (!nativeWantedRef.current) {
        setDotState("idle");
      }
      if (nativeWantedRef.current) {
        send({
          action: "native_subscribe",
          codec: requestedNativeCodec(),
          fps: requestedNativeFps(),
          resolution_tier: requestedResolutionTier(),
          bitrate_tier: requestedBitrateTier(),
        });
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
      setNativeCodec("jpeg");
      resetNativeFrameState();
      closeNativeDecoder();
      setStatus(t("terminal.closed"));
      setDotState("idle");
    };

    ws.onmessage = (event) => {
      if (event.data instanceof ArrayBuffer) {
        handleNativeBinaryFrameBuffer(event.data);
        return;
      }
      if (event.data instanceof Blob) {
        void event.data.arrayBuffer().then((buffer) => {
          handleNativeBinaryFrameBuffer(buffer);
        }).catch(() => {
          // Ignore malformed binary payloads.
        });
        return;
      }
      if (typeof event.data !== "string") {
        return;
      }

      let payload: Record<string, unknown>;
      try {
        payload = JSON.parse(event.data) as Record<string, unknown>;
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
        const negotiatedCodec = String(payload.codec ?? "");
        const nextCodec: NativeCodec =
          negotiatedCodec === "h264" && supportsH264Decode() ? "h264" : "jpeg";
        const negotiatedResolutionTier = String(payload.resolution_tier ?? "");
        const negotiatedBitrateTier = String(payload.bitrate_tier ?? "");
        if (isResolutionTier(negotiatedResolutionTier)) {
          setPreferredResolutionTier(negotiatedResolutionTier);
        }
        if (isBitrateTier(negotiatedBitrateTier)) {
          setPreferredBitrateTier(negotiatedBitrateTier);
        }
        nativeWantedRef.current = true;
        setNativeStreaming(true);
        setNativeCodec(nextCodec);
        resetNativeFrameState();
        if (nextCodec !== "h264") {
          closeNativeDecoder();
        }
        setStatus(t("screen.native_start"));
        setDotState("active");
        return;
      }
      if (eventType === "native_unsubscribed") {
        nativeWantedRef.current = false;
        setNativeStreaming(false);
        setNativeCodec("jpeg");
        resetNativeFrameState();
        closeNativeDecoder();
        setStatus(t("terminal.connected"));
        setDotState("idle");
        return;
      }
      if (eventType === "native_h264") {
        const frame = String(payload.h264_base64 ?? "");
        if (!frame) return;
        const width = Number(payload.width ?? 0);
        const height = Number(payload.height ?? 0);
        const codec = String(payload.codec ?? "avc1.42E01E") || "avc1.42E01E";
        if (!Number.isFinite(width) || !Number.isFinite(height) || width <= 0 || height <= 0) {
          return;
        }
        if (!ensureH264Decoder(width, height, codec)) {
          setStatus(t("terminal.failed"));
          setDotState("error");
          return;
        }

        const rawKeyframe = payload.keyframe;
        const isKeyframe =
          rawKeyframe === true || rawKeyframe === "true" || rawKeyframe === 1 || rawKeyframe === "1";
        if (!isKeyframe && !nativeDecoderSawKeyRef.current) {
          return;
        }
        if (isKeyframe) {
          nativeDecoderSawKeyRef.current = true;
        }

        const bytes = decodeBase64Bytes(frame);
        const capturedAtMs = Number(payload.captured_at_ms ?? 0);
        let timestamp = Number.isFinite(capturedAtMs) && capturedAtMs > 0
          ? Math.trunc(capturedAtMs * 1000)
          : nativeDecoderTimestampRef.current + 40_000;
        if (timestamp <= nativeDecoderTimestampRef.current) {
          timestamp = nativeDecoderTimestampRef.current + 1;
        }
        nativeDecoderTimestampRef.current = timestamp;

        const decoder = nativeDecoderRef.current;
        if (!decoder) return;

        try {
          decoder.decode(
            new EncodedVideoChunk({
              type: isKeyframe ? "key" : "delta",
              timestamp,
              data: bytes,
            })
          );
          setNativeCodec("h264");
        } catch {
          setStatus(t("terminal.failed"));
          setDotState("error");
        }
        return;
      }
      if (eventType === "native_frame") {
        const frame = String(payload.jpeg_base64 ?? "");
        if (frame.length > 0) {
          const capturedAtMs = Number(payload.captured_at_ms ?? 0);
          closeNativeDecoder();
          setNativeCodec("jpeg");
          revokeNativeFrameObjectUrl();
          setNativeFrameUrl(`data:image/jpeg;base64,${frame}`);
          const validCapturedAtMs =
            Number.isFinite(capturedAtMs) && capturedAtMs >= EPOCH_MS_MIN ? capturedAtMs : undefined;
          recordNativeFrameStats(validCapturedAtMs);
          if (!nativeHasFrameRef.current) {
            nativeHasFrameRef.current = true;
            setNativeHasFrame(true);
          }
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
        setNativeCodec("jpeg");
        resetNativeFrameState();
        closeNativeDecoder();
        setDotState("idle");
        return;
      }
      connectWs();
      return;
    }

    if (ws.readyState === WebSocket.OPEN) {
      send(
        nextWanted
          ? {
            action: "native_subscribe",
            codec: requestedNativeCodec(),
            fps: requestedNativeFps(),
            resolution_tier: requestedResolutionTier(),
            bitrate_tier: requestedBitrateTier(),
          }
          : { action: "native_unsubscribe" }
      );
      if (!nextWanted) {
        setNativeStreaming(false);
        setNativeCodec("jpeg");
        resetNativeFrameState();
        closeNativeDecoder();
        setDotState("idle");
      }
      return;
    }

    if (!nextWanted) {
      setNativeStreaming(false);
      setNativeCodec("jpeg");
      resetNativeFrameState();
      closeNativeDecoder();
      setDotState("idle");
    }
  };

  const onNativeCodecChange = (event: ReactChangeEvent<HTMLSelectElement>) => {
    const nextCodec: NativeCodec = event.target.value === "h264" ? "h264" : "jpeg";
    setPreferredNativeCodec(nextCodec);
    const ws = wsRef.current;
    if (!nativeWantedRef.current || !ws || ws.readyState !== WebSocket.OPEN) {
      return;
    }
    setDotState("connecting");
    resetNativeFrameState();
    if (nextCodec !== "h264") {
      closeNativeDecoder();
    }
    send({
      action: "native_subscribe",
      codec: nextCodec === "h264" && supportsH264Decode() ? "h264" : "jpeg",
      fps: requestedNativeFps(),
      resolution_tier: requestedResolutionTier(),
      bitrate_tier: requestedBitrateTier(),
    });
  };

  const onNativeFpsChange = (event: ReactChangeEvent<HTMLSelectElement>) => {
    const parsed = Number.parseInt(event.target.value, 10);
    const nextFps = NATIVE_FPS_OPTIONS.includes(parsed as (typeof NATIVE_FPS_OPTIONS)[number]) ? parsed : 8;
    setPreferredNativeFps(nextFps);

    const ws = wsRef.current;
    if (!nativeWantedRef.current || !ws || ws.readyState !== WebSocket.OPEN) {
      return;
    }
    setDotState("connecting");
    send({
      action: "native_subscribe",
      codec: requestedNativeCodec(),
      fps: nextFps,
      resolution_tier: requestedResolutionTier(),
      bitrate_tier: requestedBitrateTier(),
    });
  };

  const onNativeResolutionTierChange = (event: ReactChangeEvent<HTMLSelectElement>) => {
    const nextTier = event.target.value;
    if (!isResolutionTier(nextTier)) {
      return;
    }
    setPreferredResolutionTier(nextTier);

    const ws = wsRef.current;
    if (!nativeWantedRef.current || !ws || ws.readyState !== WebSocket.OPEN) {
      return;
    }
    setDotState("connecting");
    resetNativeFrameState();
    send({
      action: "native_subscribe",
      codec: requestedNativeCodec(),
      fps: requestedNativeFps(),
      resolution_tier: nextTier,
      bitrate_tier: requestedBitrateTier(),
    });
  };

  const onNativeBitrateTierChange = (event: ReactChangeEvent<HTMLSelectElement>) => {
    const nextTier = event.target.value;
    if (!isBitrateTier(nextTier)) {
      return;
    }
    setPreferredBitrateTier(nextTier);

    const ws = wsRef.current;
    if (!nativeWantedRef.current || !ws || ws.readyState !== WebSocket.OPEN) {
      return;
    }
    setDotState("connecting");
    send({
      action: "native_subscribe",
      codec: requestedNativeCodec(),
      fps: requestedNativeFps(),
      resolution_tier: requestedResolutionTier(),
      bitrate_tier: nextTier,
    });
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

  const onNativeMouseMove = (event: ReactMouseEvent<HTMLElement>) => {
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

  const onNativeClick = (event: ReactMouseEvent<HTMLElement>) => {
    if (!nativeStreaming) return;
    nativeSurfaceRef.current?.focus();
    sendInput("mouse_click", {
      button: event.button,
    });
  };

  const onNativeWheel = (event: ReactWheelEvent<HTMLElement>) => {
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
    <section className="flex h-full min-h-[460px] flex-col rounded-3xl bg-white/70 p-4 shadow-soft ring-1 ring-slate-200/70 backdrop-blur dark:bg-neutral-900/55 dark:ring-neutral-800/70">
      <div className="flex flex-wrap items-start justify-between gap-3">
        <div>
          <div className="inline-flex items-center gap-2">
            <h2 className="inline-flex items-center gap-2 text-base font-semibold tracking-tight text-slate-900 dark:text-neutral-50">
              <FiMonitor />
              {t("screen.title")}
            </h2>
            <span className="inline-flex h-2.5 w-2.5 shrink-0 items-center justify-center">
              <span
                className={cn(
                  "block h-2.5 w-2.5 rounded-full border border-white/90 transition-all duration-300 ease-out dark:border-neutral-900/90",
                  dotState === "active" &&
                    "scale-100 bg-emerald-500 shadow-[0_0_0_3px_rgba(16,185,129,0.20)]",
                  dotState === "connecting" && "animate-pulse scale-95 bg-amber-400",
                  dotState === "error" && "scale-100 bg-rose-500 shadow-[0_0_0_3px_rgba(244,63,94,0.16)]",
                  dotState === "idle" && "scale-90 bg-slate-400 dark:bg-neutral-500"
                )}
                title={status}
                aria-label={`${t("terminal.status")}: ${status}`}
              />
            </span>
          </div>
          <div className="mt-1 text-xs font-semibold text-slate-500 dark:text-neutral-400">
            {t("screen.native_stream")}
          </div>
          <div className="mt-1 flex flex-wrap items-center gap-x-3 gap-y-1 text-[11px] font-semibold text-slate-500 dark:text-neutral-400">
            <span>
              {t("screen.real_fps")}: {nativeActualFps.toFixed(1)}
            </span>
            <span>
              {t("screen.latency")}: {nativeLatencyMs === null ? "--" : `${Math.round(nativeLatencyMs)} ms`}
            </span>
          </div>
        </div>

        <div className="flex w-full flex-col gap-2 sm:flex-row sm:items-center lg:w-auto lg:flex-row lg:flex-wrap">
          <label className="inline-flex h-10 items-center gap-2 rounded-2xl bg-slate-100 px-3 text-xs font-semibold text-slate-600 dark:bg-neutral-800 dark:text-neutral-200">
            <span>{t("screen.codec")}</span>
            <select
              className="h-7 rounded-lg border border-slate-200 bg-white px-2 text-xs font-semibold text-slate-700 outline-none focus:border-slate-400 dark:border-neutral-700 dark:bg-neutral-900 dark:text-neutral-100"
              value={preferredNativeCodec}
              onChange={onNativeCodecChange}
            >
              <option value="jpeg">{t("screen.codec_jpeg")}</option>
              <option value="h264" disabled={!supportsH264Decode()}>
                {t("screen.codec_h264")}
              </option>
            </select>
          </label>
          <label className="inline-flex h-10 items-center gap-2 rounded-2xl bg-slate-100 px-3 text-xs font-semibold text-slate-600 dark:bg-neutral-800 dark:text-neutral-200">
            <span>{t("screen.fps")}</span>
            <select
              className="h-7 rounded-lg border border-slate-200 bg-white px-2 text-xs font-semibold text-slate-700 outline-none focus:border-slate-400 dark:border-neutral-700 dark:bg-neutral-900 dark:text-neutral-100"
              value={preferredNativeFps}
              onChange={onNativeFpsChange}
            >
              {NATIVE_FPS_OPTIONS.map((fps) => (
                <option key={fps} value={fps}>
                  {fps}
                </option>
              ))}
            </select>
          </label>
          <label className="inline-flex h-10 items-center gap-2 rounded-2xl bg-slate-100 px-3 text-xs font-semibold text-slate-600 dark:bg-neutral-800 dark:text-neutral-200">
            <span>{t("screen.resolution")}</span>
            <select
              className="h-7 rounded-lg border border-slate-200 bg-white px-2 text-xs font-semibold text-slate-700 outline-none focus:border-slate-400 dark:border-neutral-700 dark:bg-neutral-900 dark:text-neutral-100"
              value={preferredResolutionTier}
              onChange={onNativeResolutionTierChange}
            >
              {resolutionOptions.map((option) => (
                <option key={option.id} value={option.id}>
                  {t(`screen.resolution_${option.id}`)}
                </option>
              ))}
            </select>
          </label>
          <label className="inline-flex h-10 items-center gap-2 rounded-2xl bg-slate-100 px-3 text-xs font-semibold text-slate-600 dark:bg-neutral-800 dark:text-neutral-200">
            <span>{t("screen.bitrate")}</span>
            <select
              className="h-7 rounded-lg border border-slate-200 bg-white px-2 text-xs font-semibold text-slate-700 outline-none focus:border-slate-400 dark:border-neutral-700 dark:bg-neutral-900 dark:text-neutral-100"
              value={preferredBitrateTier}
              onChange={onNativeBitrateTierChange}
            >
              {bitrateOptions.map((option) => (
                <option key={option.id} value={option.id}>
                  {t(`screen.bitrate_${option.id}`)}
                </option>
              ))}
            </select>
          </label>
          <button
            className="inline-flex h-10 items-center justify-center gap-2 rounded-2xl bg-slate-900 px-4 text-sm font-semibold text-white shadow-sm hover:bg-slate-800 dark:bg-neutral-50 dark:text-neutral-900 dark:hover:bg-white"
            onClick={toggleNativeStreaming}
          >
            {nativeStreaming ? <FiPause /> : <FiPlay />}{" "}
            {nativeStreaming ? t("screen.native_stop") : t("screen.native_start")}
          </button>
          <button
            className="inline-flex h-10 items-center justify-center gap-2 rounded-2xl bg-slate-100 px-4 text-sm font-semibold text-slate-700 hover:bg-slate-200 dark:bg-neutral-800 dark:text-neutral-100 dark:hover:bg-neutral-700"
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
            "native-surface min-h-0 flex-1 rounded-2xl bg-neutral-950 shadow-sm ring-1 ring-slate-200/70 outline-none dark:ring-neutral-800/70",
            nativeInputFocused && "ring-2 ring-slate-900/70 dark:ring-neutral-50/70"
          )}
          tabIndex={0}
          onMouseDown={() => nativeSurfaceRef.current?.focus()}
          onFocus={() => setNativeInputFocused(true)}
          onBlur={() => setNativeInputFocused(false)}
          onKeyDown={onNativeKeyDown}
          onKeyUp={onNativeKeyUp}
        >
          {nativeStreaming && nativeCodec === "h264" ? (
            <div className="relative h-full w-full">
              <canvas
                ref={nativeCanvasRef}
                className="block h-full w-full select-none rounded-2xl object-contain"
                onMouseMove={onNativeMouseMove}
                onClick={onNativeClick}
                onWheel={onNativeWheel}
                onContextMenu={(event) => event.preventDefault()}
              />
              {!nativeHasFrame ? (
                <div className="pointer-events-none absolute inset-0 grid place-items-center rounded-2xl bg-neutral-950 text-sm text-neutral-300">
                  {t("screen.native_wait")}
                </div>
              ) : null}
            </div>
          ) : nativeFrameUrl ? (
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
            <div className="grid h-full w-full place-items-center rounded-2xl bg-neutral-950 text-sm text-neutral-300">
              {nativeStreaming ? t("screen.native_wait") : t("screen.native_start_hint")}
            </div>
          )}
        </div>
      </div>
    </section>
  );
}
