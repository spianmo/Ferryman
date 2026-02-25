import { useEffect, useRef, useState } from "react";
import type { ReactNode } from "react";
import type {
  ChangeEvent as ReactChangeEvent,
  KeyboardEvent as ReactKeyboardEvent,
  MouseEvent as ReactMouseEvent,
  WheelEvent as ReactWheelEvent,
} from "react";
import {
  FiChevronsUp,
  FiChevronLeft,
  FiChevronRight,
  FiClipboard,
  FiCommand,
  FiDelete,
  FiKey,
  FiMaximize,
  FiMinimize,
  FiMonitor,
  FiPause,
  FiPlay,
  FiXCircle,
  FiX,
} from "react-icons/fi";
import { FaLinux, FaWindows } from "react-icons/fa";
import { createPortal } from "react-dom";

import { emitUnauthorized, getScreenCapabilities, getScreenSources, wsUrl } from "../api/client";
import { useI18n } from "../i18n";
import { toast } from "../toast";
import type { ScreenSource, SessionInfo } from "../types";
import { cn } from "../util/cn";

type Props = {
  session: SessionInfo;
};

type NativeCodec = "jpeg" | "h264" | "h265" | "vp8" | "vp9" | "av1";
type ResolutionTier = "full" | "balanced" | "performance";
type BitrateTier = "sd" | "hd" | "uhd";
type RemotePlatform = "windows" | "macos" | "linux" | "unknown";

const NATIVE_CODEC_STORAGE_KEY = "ferryman.screen.native_codec";
const NATIVE_FPS_STORAGE_KEY = "ferryman.screen.native_fps";
const NATIVE_RESOLUTION_STORAGE_KEY = "ferryman.screen.native_resolution_tier";
const NATIVE_BITRATE_STORAGE_KEY = "ferryman.screen.native_bitrate_tier";
const NATIVE_FPS_OPTIONS = [5, 8, 10, 12, 15, 24, 30, 60] as const;
const EPOCH_MS_MIN = 1_000_000_000_000;
const NATIVE_BINARY_MAGIC = 0x314d5246;
const NATIVE_BINARY_HEADER_BYTES = 36;
const H264_WEBCODECS_CODEC = "avc1.42E01E";
const H265_WEBCODECS_CODEC = "hvc1.1.6.L93.B0";
const VP8_WEBCODECS_CODEC = "vp8";
const VP9_WEBCODECS_CODEC = "vp09.00.10.08";
const AV1_WEBCODECS_CODEC = "av01.0.08M.08";
const H265_WEBCODECS_CODEC_CANDIDATES = [
  "hvc1.1.6.L123.B0",
  "hev1.1.6.L123.B0",
  H265_WEBCODECS_CODEC,
  "hev1.1.6.L93.B0",
] as const;
const VP9_WEBCODECS_CODEC_CANDIDATES = [
  VP9_WEBCODECS_CODEC,
  "vp09.00.10.10",
  "vp09.00.10.08.01.01.01.01.00",
] as const;
const AV1_WEBCODECS_CODEC_CANDIDATES = [
  AV1_WEBCODECS_CODEC,
  "av01.0.05M.08",
  "av01.0.04M.08",
] as const;
const MAX_DECODE_QUEUE_SIZE = 3;
const CLIPBOARD_MAX_LENGTH = 4000;

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

type FullscreenDocument = Document & {
  webkitFullscreenElement?: Element | null;
  webkitExitFullscreen?: () => Promise<void> | void;
};

type FullscreenSurface = HTMLDivElement & {
  webkitRequestFullscreen?: () => Promise<void> | void;
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

function inferRemotePlatform(caps: Record<string, unknown>): RemotePlatform {
  const backend = String(caps.screen_backend ?? "").toLowerCase();
  const platform = String(caps.platform ?? caps.os ?? "").toLowerCase();
  const lookup = `${backend} ${platform}`;
  if (lookup.includes("gdi") || lookup.includes("windows") || lookup.includes("win32")) {
    return "windows";
  }
  if (
    lookup.includes("screencapturekit") ||
    lookup.includes("mac") ||
    lookup.includes("darwin") ||
    lookup.includes("osx")
  ) {
    return "macos";
  }
  if (lookup.includes("x11") || lookup.includes("linux") || lookup.includes("wayland")) {
    return "linux";
  }
  return "unknown";
}

function parseScreenSource(value: unknown): ScreenSource | null {
  if (!value || typeof value !== "object") {
    return null;
  }
  const raw = value as Record<string, unknown>;
  const id = String(raw.id ?? "");
  if (!id) {
    return null;
  }
  const widthRaw = Number(raw.width ?? 0);
  const heightRaw = Number(raw.height ?? 0);
  return {
    id,
    name: String(raw.name ?? id),
    width: Number.isFinite(widthRaw) && widthRaw > 0 ? Math.round(widthRaw) : 0,
    height: Number.isFinite(heightRaw) && heightRaw > 0 ? Math.round(heightRaw) : 0,
    is_default: raw.is_default === true || raw.is_default === "true" || raw.is_default === 1 || raw.is_default === "1",
  };
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
  if (codecByte === 6) {
    codec = "av1";
  } else if (codecByte === 5) {
    codec = "vp9";
  } else if (codecByte === 4) {
    codec = "vp8";
  } else if (codecByte === 3) {
    codec = "h265";
  } else if (codecByte === 2) {
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

function findAnnexBStartCode(bytes: Uint8Array, from: number): number {
  for (let idx = Math.max(0, from); idx + 3 < bytes.length; idx += 1) {
    if (bytes[idx] !== 0 || bytes[idx + 1] !== 0) {
      continue;
    }
    if (bytes[idx + 2] === 1) {
      return idx;
    }
    if (idx + 4 < bytes.length && bytes[idx + 2] === 0 && bytes[idx + 3] === 1) {
      return idx;
    }
  }
  return -1;
}

function hasH264Idr(bytes: Uint8Array): boolean {
  const firstStart = findAnnexBStartCode(bytes, 0);
  if (firstStart >= 0) {
    let cursor = 0;
    while (true) {
      const start = findAnnexBStartCode(bytes, cursor);
      if (start < 0) {
        return false;
      }
      const nalStart = start + (bytes[start + 2] === 1 ? 3 : 4);
      if (nalStart >= bytes.length) {
        return false;
      }
      const nalType = bytes[nalStart] & 0x1f;
      if (nalType === 5) {
        return true;
      }
      cursor = nalStart + 1;
    }
  }

  // Fallback for AVCC/length-prefixed payloads.
  let offset = 0;
  while (offset + 4 <= bytes.length) {
    const nalSize = (
      (bytes[offset] << 24) |
      (bytes[offset + 1] << 16) |
      (bytes[offset + 2] << 8) |
      bytes[offset + 3]
    ) >>> 0;
    offset += 4;
    if (nalSize === 0 || offset + nalSize > bytes.length) {
      return false;
    }
    const nalType = bytes[offset] & 0x1f;
    if (nalType === 5) {
      return true;
    }
    offset += nalSize;
  }
  return false;
}

function hasH265Idr(bytes: Uint8Array): boolean {
  const firstStart = findAnnexBStartCode(bytes, 0);
  if (firstStart >= 0) {
    let cursor = 0;
    while (true) {
      const start = findAnnexBStartCode(bytes, cursor);
      if (start < 0) {
        return false;
      }
      const nalStart = start + (bytes[start + 2] === 1 ? 3 : 4);
      if (nalStart >= bytes.length) {
        return false;
      }
      const nalType = (bytes[nalStart] >> 1) & 0x3f;
      if (nalType >= 16 && nalType <= 21) {
        return true;
      }
      cursor = nalStart + 1;
    }
  }

  // Fallback for AVCC/length-prefixed payloads.
  let offset = 0;
  while (offset + 4 <= bytes.length) {
    const nalSize = (
      (bytes[offset] << 24) |
      (bytes[offset + 1] << 16) |
      (bytes[offset + 2] << 8) |
      bytes[offset + 3]
    ) >>> 0;
    offset += 4;
    if (nalSize === 0 || offset + nalSize > bytes.length) {
      return false;
    }
    const nalType = (bytes[offset] >> 1) & 0x3f;
    if (nalType >= 16 && nalType <= 21) {
      return true;
    }
    offset += nalSize;
  }
  return false;
}

function hasVp8Keyframe(bytes: Uint8Array): boolean {
  if (bytes.length === 0) {
    return false;
  }
  return (bytes[0] & 0x01) === 0;
}

function hasVp9Keyframe(bytes: Uint8Array): boolean {
  if (bytes.length === 0) {
    return false;
  }
  const first = bytes[0];
  const frameMarker = (first >> 6) & 0x03;
  if (frameMarker !== 0x02) {
    return false;
  }
  const showExistingFrame = ((first >> 3) & 0x01) === 1;
  if (showExistingFrame) {
    return false;
  }
  const frameType = (first >> 2) & 0x01;
  return frameType === 0;
}

function isLikelyKeyframe(frame: NativeBinaryFrame): boolean {
  if (frame.keyframe) {
    return true;
  }
  if (frame.codec === "h264") {
    return hasH264Idr(frame.payload);
  }
  if (frame.codec === "h265") {
    return hasH265Idr(frame.payload);
  }
  if (frame.codec === "vp8") {
    return hasVp8Keyframe(frame.payload);
  }
  if (frame.codec === "vp9") {
    return hasVp9Keyframe(frame.payload);
  }
  return false;
}

function getFullscreenElement(): Element | null {
  const doc = document as FullscreenDocument;
  return document.fullscreenElement ?? doc.webkitFullscreenElement ?? null;
}

function shouldUsePseudoFullscreen(): boolean {
  if (typeof window === "undefined") {
    return false;
  }
  const coarsePointer =
    typeof window.matchMedia === "function" && window.matchMedia("(pointer: coarse)").matches;
  return coarsePointer || navigator.maxTouchPoints > 0;
}

function waitForFullscreenState(targetIsFullscreen: boolean, timeoutMs = 450): Promise<boolean> {
  if (targetIsFullscreen) {
    if (getFullscreenElement()) {
      return Promise.resolve(true);
    }
  } else if (!getFullscreenElement()) {
    return Promise.resolve(true);
  }

  return new Promise((resolve) => {
    let settled = false;
    let timer = 0;

    const cleanup = () => {
      if (timer) {
        window.clearTimeout(timer);
      }
      document.removeEventListener("fullscreenchange", onChange);
      document.removeEventListener("webkitfullscreenchange", onChange);
    };

    const finish = (ok: boolean) => {
      if (settled) {
        return;
      }
      settled = true;
      cleanup();
      resolve(ok);
    };

    const onChange = () => {
      const nowFullscreen = Boolean(getFullscreenElement());
      if (nowFullscreen === targetIsFullscreen) {
        finish(true);
      }
    };

    document.addEventListener("fullscreenchange", onChange);
    document.addEventListener("webkitfullscreenchange", onChange);
    timer = window.setTimeout(() => finish(false), timeoutMs);
  });
}

export default function ScreenPage({ session }: Props) {
  const { t } = useI18n();
  const wsRef = useRef<WebSocket | null>(null);
  const selectedScreenSourceIdRef = useRef("");
  const defaultScreenSourceIdRef = useRef("");
  const fallbackToastSourceIdRef = useRef("");
  const nativeSurfaceRef = useRef<HTMLDivElement | null>(null);
  const nativeCanvasRef = useRef<HTMLCanvasElement | null>(null);
  const nativeDecoderRef = useRef<VideoDecoder | null>(null);
  const nativeDecoderCodecRef = useRef(H264_WEBCODECS_CODEC);
  const supportedH264CodecRef = useRef<string | null>(H264_WEBCODECS_CODEC);
  const supportedH265CodecRef = useRef<string | null>(null);
  const supportedVP8CodecRef = useRef<string | null>(VP8_WEBCODECS_CODEC);
  const supportedVP9CodecRef = useRef<string | null>(null);
  const supportedAV1CodecRef = useRef<string | null>(null);
  const supportsH264DecodeRef = useRef<boolean | null>(null);
  const supportsH265DecodeRef = useRef<boolean | null>(null);
  const supportsVP8DecodeRef = useRef<boolean | null>(null);
  const supportsVP9DecodeRef = useRef<boolean | null>(null);
  const supportsAV1DecodeRef = useRef<boolean | null>(null);
  const nativeDecoderWidthRef = useRef(0);
  const nativeDecoderHeightRef = useRef(0);
  const nativeFrameWidthRef = useRef(0);
  const nativeFrameHeightRef = useRef(0);
  const nativeDecoderTimestampRef = useRef(0);
  const nativeDecoderSawKeyRef = useRef(false);
  const nativeDropUntilKeyframeRef = useRef(false);
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
  const pressedMouseButtonsRef = useRef(0);
  const pressedKeysRef = useRef(new Map<string, { key: string; code: string; location: number }>());
  const tappedShortcutKeysRef = useRef(new Set<string>());
  const pinnedKeysRef = useRef({ ctrl: false, alt: false, meta: false });
  const nativeWantedRef = useRef(false);
  const [dotState, setDotState] = useState<"idle" | "connecting" | "active" | "error">("idle");

  const [status, setStatus] = useState(t("terminal.disconnected"));
  const [nativeStreaming, setNativeStreaming] = useState(false);
  const [nativeCodec, setNativeCodec] = useState<NativeCodec>("jpeg");
  const [preferredNativeCodec, setPreferredNativeCodec] = useState<NativeCodec>(() => {
    const hasWebCodecs = typeof window !== "undefined" && typeof VideoDecoder !== "undefined";
    if (typeof window !== "undefined") {
      const stored = window.localStorage.getItem(NATIVE_CODEC_STORAGE_KEY);
      if (stored === "jpeg") {
        return "jpeg";
      }
      if (stored === "vp9" && hasWebCodecs) {
        return "vp9";
      }
      if (stored === "av1" && hasWebCodecs) {
        return "av1";
      }
      if (stored === "vp8" && hasWebCodecs) {
        return "vp8";
      }
      if (stored === "h265" && hasWebCodecs) {
        return "h265";
      }
      if (stored === "h264" && hasWebCodecs) {
        return "h264";
      }
    }
    if (hasWebCodecs) {
      return "h264";
    }
    return "jpeg";
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
  const [screenSources, setScreenSources] = useState<ScreenSource[]>([]);
  const [screenSourcesLoading, setScreenSourcesLoading] = useState(true);
  const [selectedScreenSourceId, setSelectedScreenSourceId] = useState("");
  const [nativeHasFrame, setNativeHasFrame] = useState(false);
  const [nativeFrameUrl, setNativeFrameUrl] = useState("");
  const [nativeActualFps, setNativeActualFps] = useState(0);
  const [nativeLatencyMs, setNativeLatencyMs] = useState<number | null>(null);
  const [nativeInputFocused, setNativeInputFocused] = useState(false);
  const [nativeFullscreen, setNativeFullscreen] = useState(false);
  const [nativePseudoFullscreen, setNativePseudoFullscreen] = useState(false);
  const [softKeyPanelOpen, setSoftKeyPanelOpen] = useState(false);
  const [clipboardPanelOpen, setClipboardPanelOpen] = useState(false);
  const [clipboardText, setClipboardText] = useState("");
  const [pinnedKeys, setPinnedKeys] = useState({ ctrl: false, alt: false, meta: false });
  const [remotePlatform, setRemotePlatform] = useState<RemotePlatform>("unknown");

  useEffect(() => {
    return () => {
      releasePinnedModifierKeys();
      wsRef.current?.close();
      wsRef.current = null;
      revokeNativeFrameObjectUrl();
    };
  }, []);

  useEffect(() => {
    const onFullscreenChange = () => {
      setNativeFullscreen(getFullscreenElement() === nativeSurfaceRef.current);
    };
    document.addEventListener("fullscreenchange", onFullscreenChange);
    document.addEventListener("webkitfullscreenchange", onFullscreenChange);
    return () => {
      document.removeEventListener("fullscreenchange", onFullscreenChange);
      document.removeEventListener("webkitfullscreenchange", onFullscreenChange);
    };
  }, []);

  useEffect(() => {
    if (!nativePseudoFullscreen) {
      return;
    }
    const prevBodyOverflow = document.body.style.overflow;
    const prevHtmlOverflow = document.documentElement.style.overflow;
    document.body.style.overflow = "hidden";
    document.documentElement.style.overflow = "hidden";
    return () => {
      document.body.style.overflow = prevBodyOverflow;
      document.documentElement.style.overflow = prevHtmlOverflow;
    };
  }, [nativePseudoFullscreen]);

  const probeDecodeSupport = (
    candidates: readonly string[],
    supportedRef: { current: boolean | null },
    codecRef: { current: string | null }
  ) => {
    if (supportedRef.current !== null) {
      return supportedRef.current;
    }
    if (typeof window === "undefined" || typeof VideoDecoder === "undefined") {
      supportedRef.current = false;
      codecRef.current = null;
      return false;
    }

    for (const candidate of candidates) {
      try {
        const decoder = new VideoDecoder({
          output: (frame) => frame.close(),
          error: () => {
            // Probe only.
          },
        });
        decoder.configure({
          codec: candidate,
          codedWidth: 1280,
          codedHeight: 720,
          optimizeForLatency: true,
        });
        decoder.close();
        supportedRef.current = true;
        codecRef.current = candidate;
        return true;
      } catch {
        // Keep trying alternative codec strings.
      }
    }

    supportedRef.current = false;
    codecRef.current = null;
    return false;
  };

  const supportsH264Decode = () => {
    return probeDecodeSupport([H264_WEBCODECS_CODEC], supportsH264DecodeRef, supportedH264CodecRef);
  };

  const supportsH265Decode = () => {
    return probeDecodeSupport(H265_WEBCODECS_CODEC_CANDIDATES, supportsH265DecodeRef, supportedH265CodecRef);
  };

  const supportsVP8Decode = () => {
    return probeDecodeSupport([VP8_WEBCODECS_CODEC], supportsVP8DecodeRef, supportedVP8CodecRef);
  };

  const supportsVP9Decode = () => {
    return probeDecodeSupport(VP9_WEBCODECS_CODEC_CANDIDATES, supportsVP9DecodeRef, supportedVP9CodecRef);
  };

  const supportsAV1Decode = () => {
    return probeDecodeSupport(AV1_WEBCODECS_CODEC_CANDIDATES, supportsAV1DecodeRef, supportedAV1CodecRef);
  };

  const supportsNativeVideoCodec = (codec: NativeCodec): boolean => {
    if (codec === "h264") {
      return supportsH264Decode();
    }
    if (codec === "h265") {
      return supportsH265Decode();
    }
    if (codec === "vp8") {
      return supportsVP8Decode();
    }
    if (codec === "vp9") {
      return supportsVP9Decode();
    }
    if (codec === "av1") {
      return supportsAV1Decode();
    }
    return true;
  };

  const requestedNativeCodecFor = (preferredCodec: NativeCodec): NativeCodec => {
    if (preferredCodec !== "jpeg" && supportsNativeVideoCodec(preferredCodec)) {
      return preferredCodec;
    }
    if (supportsH264Decode()) {
      return "h264";
    }
    if (supportsH265Decode()) {
      return "h265";
    }
    if (supportsVP8Decode()) {
      return "vp8";
    }
    if (supportsVP9Decode()) {
      return "vp9";
    }
    if (supportsAV1Decode()) {
      return "av1";
    }
    return "jpeg";
  };

  const revokeNativeFrameObjectUrl = () => {
    if (!nativeFrameObjectUrlRef.current) return;
    URL.revokeObjectURL(nativeFrameObjectUrlRef.current);
    nativeFrameObjectUrlRef.current = null;
  };

  const resetNativeFrameState = () => {
    nativeHasFrameRef.current = false;
    nativeFrameWidthRef.current = 0;
    nativeFrameHeightRef.current = 0;
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
    nativeDecoderCodecRef.current = H264_WEBCODECS_CODEC;
    nativeDecoderTimestampRef.current = 0;
    nativeDecoderSawKeyRef.current = false;
    nativeDropUntilKeyframeRef.current = false;
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
    if (preferredNativeCodec !== "jpeg" && !supportsNativeVideoCodec(preferredNativeCodec)) {
      setPreferredNativeCodec(requestedNativeCodecFor("jpeg"));
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
        setRemotePlatform(inferRemotePlatform(caps));
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
    selectedScreenSourceIdRef.current = selectedScreenSourceId;
    if (
      selectedScreenSourceId.length === 0 ||
      selectedScreenSourceId !== defaultScreenSourceIdRef.current
    ) {
      fallbackToastSourceIdRef.current = "";
    }
  }, [selectedScreenSourceId]);

  useEffect(() => {
    let active = true;
    setScreenSourcesLoading(true);
    void (async () => {
      try {
        const response = await getScreenSources(session.token);
        if (!active || !response.ok) {
          return;
        }
        const nextSources = Array.isArray(response.sources)
          ? response.sources.map((item) => parseScreenSource(item)).filter((item): item is ScreenSource => item !== null)
          : [];
        setScreenSources(nextSources);

        const responseActiveSourceId = String(response.active_source_id ?? "");
        const responseDefaultSourceId = String(response.default_source_id ?? "");
        const previousSourceId = selectedScreenSourceIdRef.current;
        const resolvedDefaultSourceId = nextSources.some((item) => item.id === responseDefaultSourceId)
          ? responseDefaultSourceId
          : nextSources.find((item) => item.is_default)?.id ?? nextSources[0]?.id ?? "";
        defaultScreenSourceIdRef.current = resolvedDefaultSourceId;
        const fallbackSourceId =
          nextSources.find((item) => item.is_default)?.id ?? nextSources[0]?.id ?? "";
        const nextSourceId = nextSources.some((item) => item.id === previousSourceId)
          ? previousSourceId
          : nextSources.some((item) => item.id === responseActiveSourceId)
          ? responseActiveSourceId
          : nextSources.some((item) => item.id === responseDefaultSourceId)
            ? responseDefaultSourceId
            : fallbackSourceId;
        const sourceWentOffline =
          previousSourceId.length > 0 &&
          !nextSources.some((item) => item.id === previousSourceId) &&
          nextSourceId.length > 0 &&
          nextSourceId !== previousSourceId;
        if (
          sourceWentOffline &&
          nextSourceId === resolvedDefaultSourceId &&
          fallbackToastSourceIdRef.current !== previousSourceId
        ) {
          fallbackToastSourceIdRef.current = previousSourceId;
          toast.info(t("screen.source_fallback_default"));
        }
        setSelectedScreenSourceId(nextSourceId);
      } catch {
        // Keep existing source list on transient failures.
      } finally {
        if (active) {
          setScreenSourcesLoading(false);
        }
      }
    })();
    return () => {
      active = false;
    };
  }, [session.token, t]);

  useEffect(() => {
    if (screenSources.length === 0) {
      if (selectedScreenSourceId !== "") {
        setSelectedScreenSourceId("");
      }
      return;
    }
    if (screenSources.some((item) => item.id === selectedScreenSourceId)) {
      return;
    }
    const fallbackSourceId =
      screenSources.find((item) => item.is_default)?.id ?? screenSources[0]?.id ?? "";
    setSelectedScreenSourceId(fallbackSourceId);
  }, [screenSources, selectedScreenSourceId]);

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
    if (nativeStreaming) {
      return;
    }
    pressedMouseButtonsRef.current = 0;
    setSoftKeyPanelOpen(false);
    setClipboardPanelOpen(false);
    releasePinnedModifierKeys();
  }, [nativeStreaming]);

  useEffect(() => {
    if (!nativeStreaming) {
      pressedKeysRef.current.clear();
      tappedShortcutKeysRef.current.clear();
      return;
    }

    const suppressKeyboardEvent = (event: KeyboardEvent) => {
      event.preventDefault();
      event.stopPropagation();
      if (typeof event.stopImmediatePropagation === "function") {
        event.stopImmediatePropagation();
      }
    };

    const keyIdForEvent = (event: KeyboardEvent) => {
      const code = event.code || "";
      const key = event.key || "";
      const location = Number.isFinite(event.location) ? event.location : 0;
      return `${code}:${key}:${location}`;
    };

    const shouldBypassKeyboardCapture = (event: KeyboardEvent) => {
      const target = event.target;
      if (!(target instanceof HTMLElement)) {
        return false;
      }
      if (target.closest("[data-screen-clipboard-panel='true']")) {
        return true;
      }
      if (target.isContentEditable) {
        return true;
      }
      const tag = target.tagName;
      return tag === "INPUT" || tag === "TEXTAREA" || tag === "SELECT";
    };

    const releasePressedKeys = () => {
      if (pressedKeysRef.current.size <= 0) {
        tappedShortcutKeysRef.current.clear();
        return;
      }
      const pending = Array.from(pressedKeysRef.current.values());
      pressedKeysRef.current.clear();
      tappedShortcutKeysRef.current.clear();
      for (const keyState of pending) {
        const mods = mergedModifierFlags();
        sendInput("key_up", {
          key: keyState.key,
          code: keyState.code,
          location: keyState.location,
          repeat: false,
          shiftKey: mods.shiftKey,
          ctrlKey: mods.ctrlKey,
          altKey: mods.altKey,
          metaKey: mods.metaKey,
        });
      }
    };

    const isModifierKey = (event: KeyboardEvent) => {
      if (event.key === "Shift" || event.key === "Control" || event.key === "Alt" || event.key === "Meta") {
        return true;
      }
      return (
        event.code === "ShiftLeft" ||
        event.code === "ShiftRight" ||
        event.code === "ControlLeft" ||
        event.code === "ControlRight" ||
        event.code === "AltLeft" ||
        event.code === "AltRight" ||
        event.code === "MetaLeft" ||
        event.code === "MetaRight"
      );
    };

    const onKeyDownCapture = (event: KeyboardEvent) => {
      if (shouldBypassKeyboardCapture(event)) {
        return;
      }
      suppressKeyboardEvent(event);
      const mods = mergedModifierFlags({
        shiftKey: event.shiftKey,
        ctrlKey: event.ctrlKey,
        altKey: event.altKey,
        metaKey: event.metaKey,
      });
      const payload = {
        key: event.key,
        code: event.code,
        location: event.location,
        repeat: event.repeat,
        shiftKey: mods.shiftKey,
        ctrlKey: mods.ctrlKey,
        altKey: mods.altKey,
        metaKey: mods.metaKey,
      };
      const keyId = keyIdForEvent(event);
      const isShortcutCombo = !isModifierKey(event) && (mods.ctrlKey || mods.metaKey || mods.altKey);
      if (isShortcutCombo && !event.repeat) {
        tappedShortcutKeysRef.current.add(keyId);
        sendInput("key_tap", payload);
        return;
      }

      pressedKeysRef.current.set(keyId, {
        key: payload.key,
        code: payload.code,
        location: payload.location,
      });
      sendInput("key_down", payload);
    };

    const onKeyUpCapture = (event: KeyboardEvent) => {
      if (shouldBypassKeyboardCapture(event)) {
        return;
      }
      suppressKeyboardEvent(event);
      const mods = mergedModifierFlags({
        shiftKey: event.shiftKey,
        ctrlKey: event.ctrlKey,
        altKey: event.altKey,
        metaKey: event.metaKey,
      });
      const payload = {
        key: event.key,
        code: event.code,
        location: event.location,
        repeat: event.repeat,
        shiftKey: mods.shiftKey,
        ctrlKey: mods.ctrlKey,
        altKey: mods.altKey,
        metaKey: mods.metaKey,
      };
      const keyId = keyIdForEvent(event);
      if (tappedShortcutKeysRef.current.has(keyId)) {
        tappedShortcutKeysRef.current.delete(keyId);
        return;
      }
      pressedKeysRef.current.delete(keyId);
      sendInput("key_up", payload);
    };

    const onKeyPressCapture = (event: KeyboardEvent) => {
      if (shouldBypassKeyboardCapture(event)) {
        return;
      }
      suppressKeyboardEvent(event);
    };

    const onWindowBlur = () => {
      releasePressedKeys();
    };

    window.addEventListener("keydown", onKeyDownCapture, true);
    window.addEventListener("keyup", onKeyUpCapture, true);
    window.addEventListener("keypress", onKeyPressCapture, true);
    window.addEventListener("blur", onWindowBlur);

    return () => {
      window.removeEventListener("keydown", onKeyDownCapture, true);
      window.removeEventListener("keyup", onKeyUpCapture, true);
      window.removeEventListener("keypress", onKeyPressCapture, true);
      window.removeEventListener("blur", onWindowBlur);
      releasePressedKeys();
    };
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

  const ensureVideoDecoder = (codedWidth: number, codedHeight: number, codec: string) => {
    if (typeof window === "undefined" || typeof VideoDecoder === "undefined") {
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
        nativeFrameWidthRef.current = frame.displayWidth;
        nativeFrameHeightRef.current = frame.displayHeight;
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
        closeNativeDecoder();
        if (nativeWantedRef.current) {
          setDotState("connecting");
        }
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
    return requestedNativeCodecFor(preferredNativeCodec);
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

  const requestedNativeSourceId = (): string => {
    if (screenSources.some((item) => item.id === selectedScreenSourceId)) {
      return selectedScreenSourceId;
    }
    return screenSources.find((item) => item.is_default)?.id ?? screenSources[0]?.id ?? "";
  };

  const decoderCodecFor = (codec: NativeCodec): string | null => {
    if (codec === "h264") {
      if (!supportsH264Decode()) {
        return null;
      }
      return supportedH264CodecRef.current ?? H264_WEBCODECS_CODEC;
    }
    if (codec === "h265") {
      if (!supportsH265Decode()) {
        return null;
      }
      return supportedH265CodecRef.current ?? H265_WEBCODECS_CODEC;
    }
    if (codec === "vp8") {
      if (!supportsVP8Decode()) {
        return null;
      }
      return supportedVP8CodecRef.current ?? VP8_WEBCODECS_CODEC;
    }
    if (codec === "vp9") {
      if (!supportsVP9Decode()) {
        return null;
      }
      return supportedVP9CodecRef.current ?? VP9_WEBCODECS_CODEC;
    }
    if (codec === "av1") {
      if (!supportsAV1Decode()) {
        return null;
      }
      return supportedAV1CodecRef.current ?? AV1_WEBCODECS_CODEC;
    }
    return null;
  };

  const shouldDropForBackpressure = (decoder: VideoDecoder, isKeyframe: boolean): boolean => {
    if (isKeyframe) {
      nativeDropUntilKeyframeRef.current = false;
      return false;
    }
    if (nativeDropUntilKeyframeRef.current) {
      return true;
    }

    const decodeQueueSize = (decoder as { decodeQueueSize?: number }).decodeQueueSize ?? 0;
    if (decodeQueueSize > MAX_DECODE_QUEUE_SIZE) {
      nativeDropUntilKeyframeRef.current = true;
      return true;
    }
    return false;
  };

  const handleNativeBinaryFrameBuffer = (buffer: ArrayBuffer) => {
    const frame = parseNativeBinaryFrame(buffer);
    if (!frame) return;
    nativeFrameWidthRef.current = frame.width;
    nativeFrameHeightRef.current = frame.height;

    if (
      frame.codec === "h264" ||
      frame.codec === "h265" ||
      frame.codec === "vp8" ||
      frame.codec === "vp9" ||
      frame.codec === "av1"
    ) {
      const decoderCodec = decoderCodecFor(frame.codec);
      if (!decoderCodec) {
        setStatus(t("terminal.failed"));
        setDotState("error");
        return;
      }
      if (!ensureVideoDecoder(frame.width, frame.height, decoderCodec)) {
        setStatus(t("terminal.failed"));
        setDotState("error");
        return;
      }

      const isKeyframe = isLikelyKeyframe(frame);
      if (!isKeyframe && !nativeDecoderSawKeyRef.current) {
        return;
      }
      if (isKeyframe) {
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
      if (shouldDropForBackpressure(decoder, isKeyframe)) {
        return;
      }

      try {
        decoder.decode(
          new EncodedVideoChunk({
            type: isKeyframe ? "key" : "delta",
            timestamp,
            data: frame.payload,
          })
        );
        setNativeCodec(frame.codec);
      } catch {
        closeNativeDecoder();
        if (nativeWantedRef.current) {
          setDotState("connecting");
        }
      }
      return;
    }

    closeNativeDecoder();
    setNativeCodec("jpeg");
    revokeNativeFrameObjectUrl();
    const jpegBytes = new Uint8Array(frame.payload.byteLength);
    jpegBytes.set(frame.payload);
    const blob = new Blob([jpegBytes], { type: "image/jpeg" });
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
          source_id: requestedNativeSourceId(),
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
        const negotiatedSourceId = String(payload.source_id ?? "");
        const nextCodec: NativeCodec =
          negotiatedCodec === "av1" && supportsAV1Decode()
            ? "av1"
            : negotiatedCodec === "vp9" && supportsVP9Decode()
            ? "vp9"
            : negotiatedCodec === "vp8" && supportsVP8Decode()
              ? "vp8"
              : negotiatedCodec === "h265" && supportsH265Decode()
            ? "h265"
            : negotiatedCodec === "h264" && supportsH264Decode()
              ? "h264"
              : "jpeg";
        const negotiatedResolutionTier = String(payload.resolution_tier ?? "");
        const negotiatedBitrateTier = String(payload.bitrate_tier ?? "");
        if (isResolutionTier(negotiatedResolutionTier)) {
          setPreferredResolutionTier(negotiatedResolutionTier);
        }
        if (isBitrateTier(negotiatedBitrateTier)) {
          setPreferredBitrateTier(negotiatedBitrateTier);
        }
        if (negotiatedSourceId.length > 0) {
          const currentSourceId = selectedScreenSourceIdRef.current;
          const defaultSourceId = defaultScreenSourceIdRef.current;
          if (
            currentSourceId.length > 0 &&
            currentSourceId !== negotiatedSourceId &&
            negotiatedSourceId === defaultSourceId &&
            fallbackToastSourceIdRef.current !== currentSourceId
          ) {
            fallbackToastSourceIdRef.current = currentSourceId;
            toast.info(t("screen.source_fallback_default"));
          }
          setSelectedScreenSourceId(negotiatedSourceId);
        }
        nativeWantedRef.current = true;
        setNativeStreaming(true);
        setNativeCodec(nextCodec);
        resetNativeFrameState();
        if (nextCodec === "jpeg") {
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
      if (
        eventType === "native_av1" ||
        eventType === "native_h264" ||
        eventType === "native_h265" ||
        eventType === "native_vp8" ||
        eventType === "native_vp9"
      ) {
        const streamCodec: NativeCodec =
          eventType === "native_av1"
            ? "av1"
            : eventType === "native_h265"
            ? "h265"
            : eventType === "native_vp8"
              ? "vp8"
              : eventType === "native_vp9"
                ? "vp9"
                : "h264";
        const frame = String(
          streamCodec === "av1"
            ? payload.av1_base64 ?? ""
            : streamCodec === "h265"
            ? payload.h265_base64 ?? ""
            : streamCodec === "vp8"
              ? payload.vp8_base64 ?? ""
              : streamCodec === "vp9"
                ? payload.vp9_base64 ?? ""
                : payload.h264_base64 ?? ""
        );
        if (!frame) return;
        const width = Number(payload.width ?? 0);
        const height = Number(payload.height ?? 0);
        const fallbackDecoderCodec = decoderCodecFor(streamCodec);
        const codec = String(payload.codec ?? fallbackDecoderCodec ?? "") || fallbackDecoderCodec;
        if (!Number.isFinite(width) || !Number.isFinite(height) || width <= 0 || height <= 0) {
          return;
        }
        if (!codec) {
          setStatus(t("terminal.failed"));
          setDotState("error");
          return;
        }
        if (!ensureVideoDecoder(width, height, codec)) {
          setStatus(t("terminal.failed"));
          setDotState("error");
          return;
        }

        const rawKeyframe = payload.keyframe;
        const hintedKeyframe =
          rawKeyframe === true || rawKeyframe === "true" || rawKeyframe === 1 || rawKeyframe === "1";
        const bytes = decodeBase64Bytes(frame);
        const isKeyframe = hintedKeyframe || (
          streamCodec === "h265"
            ? hasH265Idr(bytes)
            : streamCodec === "h264"
              ? hasH264Idr(bytes)
              : streamCodec === "vp8"
                ? hasVp8Keyframe(bytes)
                : streamCodec === "vp9"
                  ? hasVp9Keyframe(bytes)
                  : false
        );
        if (!isKeyframe && !nativeDecoderSawKeyRef.current) {
          return;
        }
        if (isKeyframe) {
          nativeDecoderSawKeyRef.current = true;
        }

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
        if (shouldDropForBackpressure(decoder, isKeyframe)) {
          return;
        }

        try {
          decoder.decode(
            new EncodedVideoChunk({
              type: isKeyframe ? "key" : "delta",
              timestamp,
              data: bytes,
            })
          );
          setNativeCodec(streamCodec);
        } catch {
          closeNativeDecoder();
          if (nativeWantedRef.current) {
            setDotState("connecting");
          }
        }
        return;
      }
      if (eventType === "native_frame") {
        const frame = String(payload.jpeg_base64 ?? "");
        if (frame.length > 0) {
          const capturedAtMs = Number(payload.captured_at_ms ?? 0);
          const width = Number(payload.width ?? 0);
          const height = Number(payload.height ?? 0);
          if (Number.isFinite(width) && Number.isFinite(height) && width > 0 && height > 0) {
            nativeFrameWidthRef.current = width;
            nativeFrameHeightRef.current = height;
          }
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
    if (nextWanted && !requestedNativeSourceId()) {
      setStatus(t("screen.source_empty"));
      setDotState("error");
      nativeWantedRef.current = false;
      return;
    }
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
            source_id: requestedNativeSourceId(),
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
    const rawValue = event.target.value;
    const nextCodec: NativeCodec =
      rawValue === "av1"
        ? "av1"
        : rawValue === "vp9"
        ? "vp9"
        : rawValue === "vp8"
          ? "vp8"
          : rawValue === "h265"
            ? "h265"
            : rawValue === "h264"
              ? "h264"
              : "jpeg";
    setPreferredNativeCodec(nextCodec);
    const ws = wsRef.current;
    if (!nativeWantedRef.current || !ws || ws.readyState !== WebSocket.OPEN) {
      return;
    }
    setDotState("connecting");
    resetNativeFrameState();
    if (nextCodec === "jpeg") {
      closeNativeDecoder();
    }
    const requestedCodec = requestedNativeCodecFor(nextCodec);
    send({
      action: "native_subscribe",
      source_id: requestedNativeSourceId(),
      codec: requestedCodec,
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
      source_id: requestedNativeSourceId(),
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
      source_id: requestedNativeSourceId(),
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
      source_id: requestedNativeSourceId(),
      codec: requestedNativeCodec(),
      fps: requestedNativeFps(),
      resolution_tier: requestedResolutionTier(),
      bitrate_tier: nextTier,
    });
  };

  const onNativeSourceChange = (event: ReactChangeEvent<HTMLSelectElement>) => {
    const nextSourceId = event.target.value;
    setSelectedScreenSourceId(nextSourceId);

    const ws = wsRef.current;
    if (!nativeWantedRef.current || !ws || ws.readyState !== WebSocket.OPEN) {
      return;
    }
    setDotState("connecting");
    resetNativeFrameState();
    closeNativeDecoder();
    send({
      action: "native_subscribe",
      source_id: nextSourceId,
      codec: requestedNativeCodec(),
      fps: requestedNativeFps(),
      resolution_tier: requestedResolutionTier(),
      bitrate_tier: requestedBitrateTier(),
    });
  };

  const toggleNativeFullscreen = async () => {
    const surface = nativeSurfaceRef.current as FullscreenSurface | null;
    const doc = document as FullscreenDocument;
    if (!surface) return;
    if (nativePseudoFullscreen) {
      setNativePseudoFullscreen(false);
      return;
    }
    if (shouldUsePseudoFullscreen()) {
      setNativePseudoFullscreen(true);
      window.setTimeout(() => nativeSurfaceRef.current?.focus(), 0);
      return;
    }
    try {
      if (getFullscreenElement()) {
        if (typeof document.exitFullscreen === "function") {
          await document.exitFullscreen();
          const exited = await waitForFullscreenState(false);
          if (!exited) {
            toast.error(t("screen.fullscreen_failed"));
          }
          return;
        }
        if (typeof doc.webkitExitFullscreen === "function") {
          await doc.webkitExitFullscreen();
          const exited = await waitForFullscreenState(false);
          if (!exited) {
            toast.error(t("screen.fullscreen_failed"));
          }
          return;
        }
        toast.error(t("screen.fullscreen_unsupported"));
        return;
      }
      if (typeof surface.requestFullscreen === "function") {
        await surface.requestFullscreen();
        const entered = await waitForFullscreenState(true);
        if (!entered || getFullscreenElement() !== surface) {
          toast.error(t("screen.fullscreen_unsupported"));
        }
        return;
      }
      if (typeof surface.webkitRequestFullscreen === "function") {
        await surface.webkitRequestFullscreen();
        const entered = await waitForFullscreenState(true);
        if (!entered || getFullscreenElement() !== surface) {
          toast.error(t("screen.fullscreen_unsupported"));
        }
        return;
      }
    } catch {
      toast.error(t("screen.fullscreen_failed"));
      return;
    }
    toast.error(t("screen.fullscreen_unsupported"));
  };

  const sendInput = (type: string, payload: Record<string, unknown>) => {
    send({ action: "input_event", type, payload });
  };

  const mergedModifierFlags = (flags?: Partial<{
    shiftKey: boolean;
    ctrlKey: boolean;
    altKey: boolean;
    metaKey: boolean;
  }>) => {
    const pinned = pinnedKeysRef.current;
    return {
      shiftKey: flags?.shiftKey ?? false,
      ctrlKey: Boolean(flags?.ctrlKey) || pinned.ctrl,
      altKey: Boolean(flags?.altKey) || pinned.alt,
      metaKey: Boolean(flags?.metaKey) || pinned.meta,
    };
  };

  const keyPayloadExactFlags = (
    key: string,
    code: string,
    location = 0,
    flags?: Partial<{ shiftKey: boolean; ctrlKey: boolean; altKey: boolean; metaKey: boolean }>
  ) => {
    return {
      key,
      code,
      location,
      repeat: false,
      shiftKey: Boolean(flags?.shiftKey),
      ctrlKey: Boolean(flags?.ctrlKey),
      altKey: Boolean(flags?.altKey),
      metaKey: Boolean(flags?.metaKey),
    };
  };

  const setPinnedKeysState = (next: { ctrl: boolean; alt: boolean; meta: boolean }) => {
    pinnedKeysRef.current = next;
    setPinnedKeys(next);
  };

  const modifierKeyMeta = (modifier: "ctrl" | "alt" | "meta") => {
    if (modifier === "ctrl") {
      return { key: "Control", code: "ControlLeft", location: 1 };
    }
    if (modifier === "alt") {
      return { key: "Alt", code: "AltLeft", location: 1 };
    }
    return { key: "Meta", code: "MetaLeft", location: 1 };
  };

  const releasePinnedModifierKeys = () => {
    const current = pinnedKeysRef.current;
    if (!current.ctrl && !current.alt && !current.meta) {
      return;
    }
    const order: Array<"meta" | "alt" | "ctrl"> = ["meta", "alt", "ctrl"];
    const next = { ...current };
    for (const modifier of order) {
      if (!next[modifier]) {
        continue;
      }
      const meta = modifierKeyMeta(modifier);
      next[modifier] = false;
      sendInput("key_up", {
        key: meta.key,
        code: meta.code,
        location: meta.location,
        repeat: false,
        shiftKey: false,
        ctrlKey: next.ctrl,
        altKey: next.alt,
        metaKey: next.meta,
      });
    }
    setPinnedKeysState({ ctrl: false, alt: false, meta: false });
  };

  const togglePinnedModifier = (modifier: "ctrl" | "alt" | "meta") => {
    if (!nativeStreaming) {
      return;
    }
    nativeSurfaceRef.current?.focus();
    const prev = pinnedKeysRef.current;
    const next = { ...prev, [modifier]: !prev[modifier] };
    const meta = modifierKeyMeta(modifier);
    const eventType = next[modifier] ? "key_down" : "key_up";
    sendInput(eventType, {
      key: meta.key,
      code: meta.code,
      location: meta.location,
      repeat: false,
      shiftKey: false,
      ctrlKey: next.ctrl,
      altKey: next.alt,
      metaKey: next.meta,
    });
    setPinnedKeysState(next);
  };

  const sendSoftKeyTap = (
    key: string,
    code: string,
    location = 0,
    flags?: Partial<{ shiftKey: boolean; ctrlKey: boolean; altKey: boolean; metaKey: boolean }>
  ) => {
    if (!nativeStreaming) {
      return;
    }
    nativeSurfaceRef.current?.focus();
    const mods = mergedModifierFlags(flags);
    const payload = keyPayloadExactFlags(key, code, location, mods);
    // Use explicit down/up to avoid key_tap releasing pinned modifiers on Windows.
    sendInput("key_down", payload);
    sendInput("key_up", payload);
  };

  const sendCtrlAltDel = () => {
    if (!nativeStreaming) {
      return;
    }
    nativeSurfaceRef.current?.focus();
    sendInput("key_tap", {
      key: "Delete",
      code: "Delete",
      location: 0,
      repeat: false,
      shiftKey: false,
      ctrlKey: true,
      altKey: true,
      metaKey: false,
    });
  };

  const sendCommandOptionEsc = () => {
    if (!nativeStreaming) {
      return;
    }
    nativeSurfaceRef.current?.focus();
    sendInput("key_tap", keyPayloadExactFlags("Escape", "Escape", 0, { altKey: true, metaKey: true }));
  };

  const sendSystemAttention = () => {
    if (remotePlatform === "macos") {
      sendCommandOptionEsc();
      return;
    }
    sendCtrlAltDel();
  };

  const sendClipboardToRemote = () => {
    if (!nativeStreaming) {
      return;
    }
    const text = clipboardText.replace(/\r\n/g, "\n");
    if (!text) {
      return;
    }
    const clipped = text.length > CLIPBOARD_MAX_LENGTH ? text.slice(0, CLIPBOARD_MAX_LENGTH) : text;
    if (text.length > CLIPBOARD_MAX_LENGTH) {
      toast.info(t("screen.clipboard_truncated"));
    }
    nativeSurfaceRef.current?.focus();
    for (const ch of clipped) {
      if (ch === "\n") {
        sendInput("key_tap", keyPayloadExactFlags("Enter", "Enter"));
        continue;
      }
      if (ch === "\t") {
        sendInput("key_tap", keyPayloadExactFlags("Tab", "Tab"));
        continue;
      }
      sendInput("key_tap", keyPayloadExactFlags(ch, ""));
    }
  };

  const pointerPayloadForEvent = (element: HTMLElement, clientX: number, clientY: number) => {
    const rect = element.getBoundingClientRect();
    if (rect.width <= 0 || rect.height <= 0) {
      return null;
    }

    let sourceWidth = 0;
    let sourceHeight = 0;
    if (element instanceof HTMLCanvasElement) {
      sourceWidth = element.width;
      sourceHeight = element.height;
    } else if (element instanceof HTMLImageElement) {
      sourceWidth = element.naturalWidth;
      sourceHeight = element.naturalHeight;
    }
    if (sourceWidth <= 0 || sourceHeight <= 0) {
      sourceWidth = nativeFrameWidthRef.current;
      sourceHeight = nativeFrameHeightRef.current;
    }

    let viewLeft = rect.left;
    let viewTop = rect.top;
    let viewWidth = rect.width;
    let viewHeight = rect.height;
    if (sourceWidth > 0 && sourceHeight > 0) {
      const scale = Math.min(rect.width / sourceWidth, rect.height / sourceHeight);
      if (Number.isFinite(scale) && scale > 0) {
        viewWidth = sourceWidth * scale;
        viewHeight = sourceHeight * scale;
        viewLeft = rect.left + (rect.width - viewWidth) / 2;
        viewTop = rect.top + (rect.height - viewHeight) / 2;
      }
    }

    const x = Math.min(Math.max(clientX - viewLeft, 0), viewWidth);
    const y = Math.min(Math.max(clientY - viewTop, 0), viewHeight);
    return { x, y, width: viewWidth, height: viewHeight };
  };

  const mouseButtonMask = (button: number) => {
    if (button === 0) {
      return 1;
    }
    if (button === 1) {
      return 4;
    }
    if (button === 2) {
      return 2;
    }
    return 0;
  };

  const mouseButtonsFromMask = (mask: number) => {
    const buttons: number[] = [];
    if ((mask & 1) !== 0) {
      buttons.push(0);
    }
    if ((mask & 2) !== 0) {
      buttons.push(2);
    }
    if ((mask & 4) !== 0) {
      buttons.push(1);
    }
    return buttons;
  };

  const releasePressedMouseButtons = () => {
    if (!nativeStreaming) {
      pressedMouseButtonsRef.current = 0;
      return;
    }
    const pressedMask = pressedMouseButtonsRef.current;
    if (pressedMask === 0) {
      return;
    }
    pressedMouseButtonsRef.current = 0;
    for (const button of mouseButtonsFromMask(pressedMask)) {
      sendInput("mouse_up", {
        button,
        buttons: 0,
      });
    }
  };

  const onNativeMouseMove = (event: ReactMouseEvent<HTMLElement>) => {
    if (!nativeStreaming) return;

    const buttons = event.buttons || pressedMouseButtonsRef.current;
    const now = Date.now();
    const throttleMs = buttons !== 0 ? 8 : 20;
    if (now - lastMouseMoveAtRef.current < throttleMs) {
      return;
    }
    lastMouseMoveAtRef.current = now;

    const payload = pointerPayloadForEvent(event.currentTarget, event.clientX, event.clientY);
    if (!payload) {
      return;
    }
    sendInput("mouse_move", {
      ...payload,
      buttons,
      shiftKey: event.shiftKey,
      ctrlKey: event.ctrlKey,
      altKey: event.altKey,
      metaKey: event.metaKey,
    });
  };

  const onNativeMouseDown = (event: ReactMouseEvent<HTMLElement>) => {
    if (!nativeStreaming) return;
    nativeSurfaceRef.current?.focus();
    const payload = pointerPayloadForEvent(event.currentTarget, event.clientX, event.clientY);
    if (!payload) {
      return;
    }

    const fallbackMask = pressedMouseButtonsRef.current | mouseButtonMask(event.button);
    const buttons = event.buttons || fallbackMask;
    pressedMouseButtonsRef.current = buttons;
    sendInput("mouse_down", {
      ...payload,
      button: event.button,
      buttons,
      click_count: Math.max(1, Math.round(event.detail || 1)),
      shiftKey: event.shiftKey,
      ctrlKey: event.ctrlKey,
      altKey: event.altKey,
      metaKey: event.metaKey,
    });
    event.preventDefault();
  };

  const onNativeMouseUp = (event: ReactMouseEvent<HTMLElement>) => {
    if (!nativeStreaming) return;
    const payload = pointerPayloadForEvent(event.currentTarget, event.clientX, event.clientY);
    if (!payload) {
      return;
    }

    const nextButtons = event.buttons;
    pressedMouseButtonsRef.current = nextButtons;
    sendInput("mouse_up", {
      ...payload,
      button: event.button,
      buttons: nextButtons,
      click_count: Math.max(1, Math.round(event.detail || 1)),
      shiftKey: event.shiftKey,
      ctrlKey: event.ctrlKey,
      altKey: event.altKey,
      metaKey: event.metaKey,
    });
    event.preventDefault();
  };

  const onNativeWheel = (event: ReactWheelEvent<HTMLElement>) => {
    if (!nativeStreaming) return;
    event.preventDefault();
    const payload = pointerPayloadForEvent(event.currentTarget, event.clientX, event.clientY);
    if (!payload) {
      return;
    }
    sendInput("mouse_wheel", {
      ...payload,
      delta_x: Math.round(event.deltaX),
      delta_y: Math.round(event.deltaY),
      buttons: event.buttons || pressedMouseButtonsRef.current,
      shiftKey: event.shiftKey,
      ctrlKey: event.ctrlKey,
      altKey: event.altKey,
      metaKey: event.metaKey,
    });
  };

  const onNativeMouseLeave = () => {
    if (!nativeStreaming) {
      return;
    }
    releasePressedMouseButtons();
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
    event.stopPropagation();
  };

  const onNativeKeyDown = (event: ReactKeyboardEvent<HTMLDivElement>) => {
    sendKeyEvent("key_down", event);
  };

  const onNativeKeyUp = (event: ReactKeyboardEvent<HTMLDivElement>) => {
    sendKeyEvent("key_up", event);
  };

  const screenIsFullscreen = nativeFullscreen || nativePseudoFullscreen;
  const requestedSourceId = requestedNativeSourceId();
  const hasScreenSources = screenSources.length > 0;
  const disableNativeStart = !nativeStreaming && (!hasScreenSources || screenSourcesLoading);

  useEffect(() => {
    if (!nativeStreaming) {
      return;
    }
    const onWindowMouseUp = () => {
      releasePressedMouseButtons();
    };
    const onWindowPointerUp = () => {
      releasePressedMouseButtons();
    };
    const onWindowBlur = () => {
      releasePressedMouseButtons();
    };
    window.addEventListener("mouseup", onWindowMouseUp);
    window.addEventListener("pointerup", onWindowPointerUp);
    window.addEventListener("blur", onWindowBlur);
    return () => {
      window.removeEventListener("mouseup", onWindowMouseUp);
      window.removeEventListener("pointerup", onWindowPointerUp);
      window.removeEventListener("blur", onWindowBlur);
    };
  }, [nativeStreaming]);

  useEffect(() => {
    if (screenIsFullscreen) {
      setSoftKeyPanelOpen(false);
      setClipboardPanelOpen(false);
      return;
    }
    setClipboardPanelOpen(false);
    setSoftKeyPanelOpen(false);
    releasePinnedModifierKeys();
  }, [screenIsFullscreen]);

  const softKeyButtonClass = (active = false) => {
    return cn(
      "inline-flex min-h-10 w-full min-w-0 items-center justify-start gap-2 rounded-xl border px-3 py-2.5 text-left text-[11px] font-semibold leading-tight transition-colors",
      active
        ? "border-emerald-300 bg-emerald-500/25 text-emerald-100"
        : "border-white/20 bg-white/8 text-white hover:bg-white/15"
    );
  };

  const softKeyModifierLabels = (() => {
    const ctrlLabel = remotePlatform === "macos" ? t("screen.softkey_control") : t("screen.softkey_ctrl");
    const altLabel = remotePlatform === "macos" ? t("screen.softkey_option") : t("screen.softkey_alt");
    if (remotePlatform === "windows") {
      return {
        ctrlLabel,
        altLabel,
        metaLabel: t("screen.softkey_win"),
      };
    }
    if (remotePlatform === "macos") {
      return {
        ctrlLabel,
        altLabel,
        metaLabel: t("screen.softkey_command"),
      };
    }
    return {
      ctrlLabel,
      altLabel,
      metaLabel: t("screen.softkey_super"),
    };
  })();

  const systemAttentionLabel =
    remotePlatform === "macos" ? t("screen.softkey_cmd_opt_esc") : t("screen.softkey_ctrl_alt_del");

  const metaModifierIcon: ReactNode =
    remotePlatform === "windows" ? (
      <FaWindows className="h-3.5 w-3.5 shrink-0" />
    ) : remotePlatform === "linux" ? (
      <FaLinux className="h-3.5 w-3.5 shrink-0" />
    ) : (
      <FiCommand className="h-3.5 w-3.5 shrink-0" />
    );

  const targetPlatformBadge: { label: string; icon: ReactNode } =
    remotePlatform === "windows"
      ? { label: "Windows", icon: <FaWindows className="h-3.5 w-3.5 shrink-0" /> }
      : remotePlatform === "macos"
        ? { label: "macOS", icon: <FiCommand className="h-3.5 w-3.5 shrink-0" /> }
        : remotePlatform === "linux"
          ? { label: "Linux", icon: <FaLinux className="h-3.5 w-3.5 shrink-0" /> }
          : { label: "Remote", icon: <FiMonitor className="h-3.5 w-3.5 shrink-0" /> };

  const renderNativeSurface = () => (
    <div
      ref={nativeSurfaceRef}
      className={cn(
        nativePseudoFullscreen
          ? "native-surface h-full w-full bg-black outline-none"
          : "native-surface min-h-0 flex-1 rounded-2xl bg-neutral-950 shadow-sm ring-1 ring-slate-200/70 outline-none dark:ring-neutral-800/70",
        nativeInputFocused &&
          !nativePseudoFullscreen &&
          "ring-2 ring-slate-900/70 dark:ring-neutral-50/70"
      )}
      tabIndex={0}
      onMouseDown={(event) => {
        if (!(event.target instanceof HTMLElement)) {
          nativeSurfaceRef.current?.focus();
          return;
        }
        if (
          event.target.closest("[data-screen-clipboard-panel='true']") ||
          event.target.closest("[data-screen-softkey-panel='true']") ||
          event.target.closest("[data-screen-softkey-toggle='true']")
        ) {
          return;
        }
        nativeSurfaceRef.current?.focus();
      }}
      onFocus={() => setNativeInputFocused(true)}
      onBlur={() => {
        setNativeInputFocused(false);
        releasePressedMouseButtons();
      }}
      onKeyDown={onNativeKeyDown}
      onKeyUp={onNativeKeyUp}
    >
      <div className="relative h-full w-full">
        {nativeStreaming && nativeCodec !== "jpeg" ? (
          <canvas
            ref={nativeCanvasRef}
            className={cn(
              "block h-full w-full select-none object-contain",
              nativePseudoFullscreen ? "rounded-none" : "rounded-2xl"
            )}
            onMouseDown={onNativeMouseDown}
            onMouseUp={onNativeMouseUp}
            onMouseMove={onNativeMouseMove}
            onWheel={onNativeWheel}
            onMouseLeave={onNativeMouseLeave}
            onContextMenu={(event) => event.preventDefault()}
            onDragStart={(event) => event.preventDefault()}
          />
        ) : nativeFrameUrl ? (
          <img
            src={nativeFrameUrl}
            className={cn(
              "block h-full w-full select-none object-contain",
              nativePseudoFullscreen ? "rounded-none" : "rounded-2xl"
            )}
            alt="native screen frame"
            onMouseDown={onNativeMouseDown}
            onMouseUp={onNativeMouseUp}
            onMouseMove={onNativeMouseMove}
            onWheel={onNativeWheel}
            onMouseLeave={onNativeMouseLeave}
            onContextMenu={(event) => event.preventDefault()}
            onDragStart={(event) => event.preventDefault()}
            draggable={false}
          />
        ) : (
          <div
            className={cn(
              "grid h-full w-full place-items-center bg-neutral-950 text-sm text-neutral-300",
              nativePseudoFullscreen ? "rounded-none" : "rounded-2xl"
            )}
          >
            {nativeStreaming ? t("screen.native_wait") : t("screen.native_start_hint")}
          </div>
        )}
        {nativeStreaming && nativeCodec !== "jpeg" && !nativeHasFrame ? (
          <div
            className={cn(
              "pointer-events-none absolute inset-0 grid place-items-center bg-neutral-950 text-sm text-neutral-300",
              nativePseudoFullscreen ? "rounded-none" : "rounded-2xl"
            )}
          >
            {t("screen.native_wait")}
          </div>
        ) : null}
        {screenIsFullscreen ? (
          <div className="absolute left-2 top-1/2 z-50 -translate-y-1/2">
              <button
                type="button"
                data-screen-softkey-toggle="true"
                className="inline-flex h-12 w-7 items-center justify-center rounded-xl bg-black/55 text-white ring-1 ring-white/20 backdrop-blur-sm transition-colors hover:bg-black/70"
                onClick={() => setSoftKeyPanelOpen((prev) => !prev)}
                aria-label={softKeyPanelOpen ? t("screen.softkeys_hide") : t("screen.softkeys_show")}
              >
                {softKeyPanelOpen ? <FiChevronLeft /> : <FiChevronRight />}
              </button>
              <div
                className={cn(
                  "absolute left-full top-1/2 ml-2 w-[min(17rem,calc(100vw-4rem))] -translate-y-1/2 transition-all duration-300 ease-out",
                  softKeyPanelOpen ? "pointer-events-auto translate-x-0 opacity-100" : "pointer-events-none -translate-x-3 opacity-0"
                )}
              >
                <div
                  data-screen-softkey-panel="true"
                  className="max-h-[calc(100dvh-2rem)] overflow-y-auto rounded-2xl bg-black/60 p-2.5 shadow-2xl ring-1 ring-white/20 backdrop-blur-md"
                >
                  <div className="mb-2 px-1 text-white/85">
                    <div className="inline-flex items-center gap-2 text-[11px] font-semibold tracking-wide">
                      {targetPlatformBadge.icon}
                      <span className="leading-tight">{targetPlatformBadge.label}</span>
                    </div>
                    <div className="mt-2 h-px bg-white/15" />
                  </div>
                  <div className="space-y-2">
                    <button
                      type="button"
                      className={softKeyButtonClass(pinnedKeys.ctrl)}
                      onClick={() => togglePinnedModifier("ctrl")}
                      aria-pressed={pinnedKeys.ctrl}
                    >
                      <FiChevronsUp className="h-3.5 w-3.5 shrink-0" />
                      <span className="min-w-0 whitespace-normal break-words">{softKeyModifierLabels.ctrlLabel}</span>
                    </button>
                    <button
                      type="button"
                      className={softKeyButtonClass(pinnedKeys.alt)}
                      onClick={() => togglePinnedModifier("alt")}
                      aria-pressed={pinnedKeys.alt}
                    >
                      <FiKey className="h-3.5 w-3.5 shrink-0" />
                      <span className="min-w-0 whitespace-normal break-words">{softKeyModifierLabels.altLabel}</span>
                    </button>
                    <button
                      type="button"
                      className={softKeyButtonClass(pinnedKeys.meta)}
                      onClick={() => togglePinnedModifier("meta")}
                      aria-pressed={pinnedKeys.meta}
                    >
                      {metaModifierIcon}
                      <span className="min-w-0 whitespace-normal break-words">{softKeyModifierLabels.metaLabel}</span>
                    </button>
                  </div>
                  <div className="mt-2 space-y-2 border-t border-white/15 pt-2">
                    <button
                      type="button"
                      className={softKeyButtonClass(false)}
                      onClick={() => sendSoftKeyTap("Tab", "Tab")}
                    >
                      <FiChevronsUp className="h-3.5 w-3.5 shrink-0 rotate-90" />
                      <span className="min-w-0 whitespace-normal break-words">{t("screen.softkey_tab")}</span>
                    </button>
                    <button
                      type="button"
                      className={softKeyButtonClass(false)}
                      onClick={() => sendSoftKeyTap("Escape", "Escape")}
                    >
                      <FiXCircle className="h-3.5 w-3.5 shrink-0" />
                      <span className="min-w-0 whitespace-normal break-words">{t("screen.softkey_esc")}</span>
                    </button>
                    <button
                      type="button"
                      className={softKeyButtonClass(false)}
                      onClick={sendSystemAttention}
                    >
                      <FiDelete className="h-3.5 w-3.5 shrink-0" />
                      <span className="min-w-0 whitespace-normal break-words">{systemAttentionLabel}</span>
                    </button>
                    <button
                      type="button"
                      className={softKeyButtonClass(false)}
                      onClick={() => setClipboardPanelOpen(true)}
                      aria-label={t("screen.clipboard")}
                    >
                      <FiClipboard className="h-3.5 w-3.5 shrink-0" />
                      <span className="min-w-0 whitespace-normal break-words">{t("screen.clipboard")}</span>
                    </button>
                  </div>
                </div>
              </div>
          </div>
        ) : null}
        {screenIsFullscreen && clipboardPanelOpen ? (
          <div className="pointer-events-auto absolute inset-0 z-50 grid place-items-center bg-black/35 p-4 backdrop-blur-[1px]">
            <div
              className="w-[min(42rem,100%)] rounded-3xl bg-slate-100/95 p-4 shadow-2xl ring-1 ring-slate-300 dark:bg-neutral-900/92 dark:ring-neutral-700"
              data-screen-clipboard-panel="true"
            >
              <div className="flex items-center justify-between gap-2 rounded-2xl bg-slate-700 px-3 py-2 text-white dark:bg-neutral-800">
                <div className="inline-flex items-center gap-2 text-sm font-semibold">
                  <FiClipboard />
                  <span>{t("screen.clipboard")}</span>
                </div>
                <button
                  type="button"
                  className="inline-flex h-8 w-8 items-center justify-center rounded-lg bg-white/10 text-white transition-colors hover:bg-white/20"
                  onClick={() => setClipboardPanelOpen(false)}
                  aria-label={t("screen.clipboard_close")}
                >
                  <FiX />
                </button>
              </div>
              <div className="mt-3 text-sm text-slate-700 dark:text-neutral-300">
                {t("screen.clipboard_hint")}
              </div>
              <textarea
                className="mt-3 h-56 w-full resize-none rounded-2xl border border-slate-300 bg-white px-3 py-2 text-sm text-slate-900 outline-none focus:border-slate-500 dark:border-neutral-700 dark:bg-neutral-950 dark:text-neutral-100 dark:focus:border-neutral-500"
                value={clipboardText}
                onChange={(event) => setClipboardText(event.target.value)}
                placeholder={t("screen.clipboard_placeholder")}
              />
              <div className="mt-3 flex flex-wrap justify-end gap-2">
                <button
                  type="button"
                  className="inline-flex h-9 items-center gap-1 rounded-xl bg-slate-900 px-3 text-xs font-semibold text-white transition-colors hover:bg-slate-800 dark:bg-neutral-100 dark:text-neutral-900 dark:hover:bg-white"
                  onClick={sendClipboardToRemote}
                >
                  {t("screen.clipboard_send_remote")}
                </button>
              </div>
            </div>
          </div>
        ) : null}
      </div>
    </div>
  );

  const pseudoFullscreenOverlay =
    nativePseudoFullscreen && typeof document !== "undefined"
      ? createPortal(
          <div className="fixed left-0 top-0 z-[120] h-[100dvh] w-screen bg-black">
            {renderNativeSurface()}
            <button
              className="absolute right-3 top-[calc(env(safe-area-inset-top)+0.5rem)] z-30 inline-flex h-9 items-center justify-center gap-1 rounded-xl bg-black/65 px-2.5 text-[11px] font-semibold text-white ring-1 ring-white/20 backdrop-blur-sm transition-colors hover:bg-black/75"
              onClick={() => setNativePseudoFullscreen(false)}
            >
              <FiMinimize />
              {t("screen.exit_fullscreen")}
            </button>
          </div>,
          document.body
        )
      : null;

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

        <div className="grid w-full grid-cols-2 gap-2 sm:flex sm:flex-row sm:items-center lg:w-auto lg:flex-wrap">
          <label className="col-span-2 flex h-10 w-full items-center justify-between gap-2 rounded-2xl bg-slate-100 px-3 text-xs font-semibold text-slate-600 sm:col-span-1 sm:w-auto dark:bg-neutral-800 dark:text-neutral-200">
            <span>{t("screen.source")}</span>
            <select
              className="h-7 min-w-[14rem] rounded-lg border border-slate-200 bg-white px-2 text-xs font-semibold text-slate-700 outline-none focus:border-slate-400 disabled:cursor-not-allowed disabled:opacity-70 dark:border-neutral-700 dark:bg-neutral-900 dark:text-neutral-100"
              value={requestedSourceId}
              onChange={onNativeSourceChange}
              disabled={screenSourcesLoading || !hasScreenSources}
            >
              {screenSourcesLoading ? (
                <option value="">{t("screen.source_loading")}</option>
              ) : !hasScreenSources ? (
                <option value="">{t("screen.source_empty")}</option>
              ) : (
                screenSources.map((source) => (
                  <option key={source.id} value={source.id}>
                    {source.name || source.id}
                  </option>
                ))
              )}
            </select>
          </label>
          <label className="flex h-10 w-full items-center justify-between gap-2 rounded-2xl bg-slate-100 px-3 text-xs font-semibold text-slate-600 sm:w-auto dark:bg-neutral-800 dark:text-neutral-200">
            <span>{t("screen.codec")}</span>
            <select
              className="h-7 rounded-lg border border-slate-200 bg-white px-2 text-xs font-semibold text-slate-700 outline-none focus:border-slate-400 dark:border-neutral-700 dark:bg-neutral-900 dark:text-neutral-100"
              value={preferredNativeCodec}
              onChange={onNativeCodecChange}
            >
              <option value="jpeg">{t("screen.codec_jpeg")}</option>
              <option value="av1" disabled={!supportsAV1Decode()}>
                {t("screen.codec_av1")}
              </option>
              <option value="h264" disabled={!supportsH264Decode()}>
                {t("screen.codec_h264")}
              </option>
              <option value="h265" disabled={!supportsH265Decode()}>
                {t("screen.codec_h265")}
              </option>
              <option value="vp8" disabled={!supportsVP8Decode()}>
                {t("screen.codec_vp8")}
              </option>
              <option value="vp9" disabled={!supportsVP9Decode()}>
                {t("screen.codec_vp9")}
              </option>
            </select>
          </label>
          <label className="flex h-10 w-full items-center justify-between gap-2 rounded-2xl bg-slate-100 px-3 text-xs font-semibold text-slate-600 sm:w-auto dark:bg-neutral-800 dark:text-neutral-200">
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
          <label className="flex h-10 w-full items-center justify-between gap-2 rounded-2xl bg-slate-100 px-3 text-xs font-semibold text-slate-600 sm:w-auto dark:bg-neutral-800 dark:text-neutral-200">
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
          <label className="flex h-10 w-full items-center justify-between gap-2 rounded-2xl bg-slate-100 px-3 text-xs font-semibold text-slate-600 sm:w-auto dark:bg-neutral-800 dark:text-neutral-200">
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
            className="inline-flex h-10 w-full items-center justify-center gap-2 rounded-2xl bg-slate-900 px-4 text-sm font-semibold text-white shadow-sm hover:bg-slate-800 disabled:cursor-not-allowed disabled:opacity-60 sm:w-auto dark:bg-neutral-50 dark:text-neutral-900 dark:hover:bg-white"
            onClick={toggleNativeStreaming}
            disabled={disableNativeStart}
          >
            {nativeStreaming ? <FiPause /> : <FiPlay />}{" "}
            {nativeStreaming ? t("screen.native_stop") : t("screen.native_start")}
          </button>
          <button
            className="inline-flex h-10 w-full items-center justify-center gap-2 rounded-2xl bg-slate-100 px-4 text-sm font-semibold text-slate-700 hover:bg-slate-200 sm:w-auto dark:bg-neutral-800 dark:text-neutral-100 dark:hover:bg-neutral-700"
            onClick={() => void toggleNativeFullscreen()}
          >
            {screenIsFullscreen ? <FiMinimize /> : <FiMaximize />}{" "}
            {screenIsFullscreen ? t("screen.exit_fullscreen") : t("screen.fullscreen")}
          </button>
        </div>
      </div>

      <div className="mt-4 flex min-h-0 flex-1 flex-col">
        {nativePseudoFullscreen ? null : renderNativeSurface()}
      </div>
      {pseudoFullscreenOverlay}
    </section>
  );
}
