import { useEffect, useRef, useState } from "react";
import { renderAsync } from "docx-preview";

type Props = {
  bytes: Uint8Array | null;
  loadingText: string;
  onParseFailed: () => void;
};

function toArrayBuffer(bytes: Uint8Array) {
  return bytes.buffer.slice(bytes.byteOffset, bytes.byteOffset + bytes.byteLength);
}

export default function DocxPreview({ bytes, loadingText, onParseFailed }: Props) {
  const containerRef = useRef<HTMLDivElement | null>(null);
  const [rendering, setRendering] = useState(false);

  useEffect(() => {
    if (!bytes || !containerRef.current) return;

    let cancelled = false;
    const container = containerRef.current;
    container.innerHTML = "";
    setRendering(true);

    void renderAsync(toArrayBuffer(bytes), container, container, {
      inWrapper: false,
      useBase64URL: true,
      ignoreLastRenderedPageBreak: false,
    })
      .catch(() => {
        if (!cancelled) {
          onParseFailed();
        }
      })
      .finally(() => {
        if (!cancelled) {
          setRendering(false);
        }
      });

    return () => {
      cancelled = true;
      container.innerHTML = "";
    };
  }, [bytes, onParseFailed]);

  return (
    <div className="mt-4 min-h-0 flex-1 overflow-auto rounded-2xl border border-slate-200 bg-white p-5 dark:border-neutral-800 dark:bg-neutral-950/50">
      {!bytes || rendering ? (
        <div className="grid h-full w-full place-items-center text-sm text-slate-500 dark:text-neutral-400">
          {loadingText}
        </div>
      ) : null}
      <div ref={containerRef} className="text-slate-800 dark:text-neutral-100" />
    </div>
  );
}
