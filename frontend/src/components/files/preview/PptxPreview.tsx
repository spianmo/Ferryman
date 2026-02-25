import { useEffect, useState } from "react";
import { pptxToHtml } from "@jvmr/pptx-to-html";

type Props = {
  bytes: Uint8Array | null;
  loadingText: string;
  emptyText: string;
  slideLabel: (index: number) => string;
  onParseFailed: () => void;
};

function toArrayBuffer(bytes: Uint8Array): ArrayBuffer {
  const copy = new Uint8Array(bytes.byteLength);
  copy.set(bytes);
  return copy.buffer;
}

export default function PptxPreview({
  bytes,
  loadingText,
  emptyText,
  slideLabel,
  onParseFailed,
}: Props) {
  const [slides, setSlides] = useState<string[]>([]);
  const [rendering, setRendering] = useState(false);

  useEffect(() => {
    if (!bytes) return;

    let cancelled = false;
    setSlides([]);
    setRendering(true);

    void pptxToHtml(toArrayBuffer(bytes), {
      width: 960,
      height: 540,
      scaleToFit: true,
      letterbox: true,
    })
      .then((nextSlides) => {
        if (!cancelled) {
          setSlides(nextSlides);
        }
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
    };
  }, [bytes, onParseFailed]);

  if (!bytes || rendering) {
    return (
      <div className="mt-4 min-h-0 flex-1 overflow-auto rounded-2xl border border-slate-200 bg-white p-4 dark:border-neutral-800 dark:bg-neutral-950/50">
        <div className="grid h-full w-full place-items-center text-sm text-slate-500 dark:text-neutral-400">
          {loadingText}
        </div>
      </div>
    );
  }

  return (
    <div className="mt-4 min-h-0 flex-1 overflow-auto rounded-2xl border border-slate-200 bg-white p-4 dark:border-neutral-800 dark:bg-neutral-950/50">
      {slides.length > 0 ? (
        <div className="space-y-4">
          {slides.map((slideHtml, idx) => (
            <section
              key={idx}
              className="rounded-xl border border-slate-200 bg-slate-50 p-3 dark:border-neutral-800 dark:bg-neutral-900/50"
            >
              <div className="mb-2 text-xs font-semibold text-slate-500 dark:text-neutral-400">
                {slideLabel(idx + 1)}
              </div>
              <div className="overflow-x-auto rounded-lg bg-black/5 p-2 dark:bg-black/25">
                <div
                  className="[&_img]:max-w-none"
                  dangerouslySetInnerHTML={{ __html: slideHtml }}
                />
              </div>
            </section>
          ))}
        </div>
      ) : (
        <div className="grid h-full w-full place-items-center text-sm text-slate-500 dark:text-neutral-400">
          {emptyText}
        </div>
      )}
    </div>
  );
}
