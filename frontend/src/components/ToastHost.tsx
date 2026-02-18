import { useEffect, useRef, useState } from "react";
import { FiCheckCircle, FiInfo, FiX, FiXCircle } from "react-icons/fi";

import { TOAST_EVENT, type ToastEventDetail, type ToastKind } from "../toast";
import { cn } from "../util/cn";

type ToastItem = {
  id: number;
  kind: ToastKind;
  message: string;
};

function iconFor(kind: ToastKind) {
  if (kind === "success") return <FiCheckCircle className="text-emerald-600 dark:text-emerald-200" />;
  if (kind === "error") return <FiXCircle className="text-rose-600 dark:text-rose-200" />;
  return <FiInfo className="text-slate-600 dark:text-neutral-200" />;
}

function toneFor(kind: ToastKind) {
  if (kind === "success") {
    return "bg-white/95 ring-slate-200/80 dark:bg-emerald-950/90 dark:ring-emerald-700/70";
  }
  if (kind === "error") {
    return "bg-white/95 ring-slate-200/80 dark:bg-rose-950/90 dark:ring-rose-700/70";
  }
  return "bg-white/95 ring-slate-200/80 dark:bg-neutral-900/95 dark:ring-neutral-600/80";
}

export default function ToastHost() {
  const [items, setItems] = useState<ToastItem[]>([]);
  const recentRef = useRef<Map<string, number>>(new Map());

  useEffect(() => {
    let nextId = 1;
    const dedupeWindowMs = 1200;
    const autoDismissMs = 2000;

    const onToast = (event: Event) => {
      const detail = (event as CustomEvent<ToastEventDetail>).detail;
      if (!detail?.message) return;

      const now = Date.now();
      const key = `${detail.kind}:${detail.message}`;
      const lastAt = recentRef.current.get(key) ?? 0;
      if (now - lastAt < dedupeWindowMs) {
        return;
      }
      recentRef.current.set(key, now);
      if (recentRef.current.size > 64) {
        for (const [k, ts] of recentRef.current.entries()) {
          if (now - ts > dedupeWindowMs * 4) {
            recentRef.current.delete(k);
          }
        }
      }

      const id = nextId++;
      const item: ToastItem = { id, kind: detail.kind, message: detail.message };
      setItems((prev) => {
        const merged = [item, ...prev];
        return merged.slice(0, 4);
      });

      window.setTimeout(() => {
        setItems((prev) => prev.filter((t) => t.id !== id));
      }, autoDismissMs);
    };

    window.addEventListener(TOAST_EVENT, onToast as EventListener);
    return () => window.removeEventListener(TOAST_EVENT, onToast as EventListener);
  }, []);

  return (
    <div className="pointer-events-none fixed inset-x-0 bottom-3 z-50 flex justify-center px-3 sm:inset-x-auto sm:right-4 sm:top-4 sm:bottom-auto sm:px-0">
      <div className="flex w-[min(420px,calc(100vw-24px))] flex-col gap-2">
        {items.map((item) => (
          <div
            key={item.id}
            className={cn(
              "pointer-events-auto flex items-center gap-3 rounded-2xl p-3 text-slate-900 shadow-soft ring-1 backdrop-blur",
              "dark:text-neutral-50 dark:shadow-[0_12px_28px_rgba(0,0,0,0.45)]",
              toneFor(item.kind)
            )}
          >
            <div className="grid h-5 w-5 shrink-0 place-items-center text-lg leading-none">
              {iconFor(item.kind)}
            </div>
            <div className="min-w-0 flex-1">
              <div className="break-words text-sm font-semibold leading-5">
                {item.message}
              </div>
            </div>
            <button
              className="grid h-8 w-8 place-items-center rounded-xl text-slate-500 transition-colors hover:bg-slate-100 hover:text-slate-700 dark:text-neutral-200 dark:hover:bg-black/30 dark:hover:text-white"
              onClick={() => setItems((prev) => prev.filter((t) => t.id !== item.id))}
              title="Close"
            >
              <FiX />
            </button>
          </div>
        ))}
      </div>
    </div>
  );
}
