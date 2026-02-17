import { useEffect, useState } from "react";
import { FiCheckCircle, FiInfo, FiX, FiXCircle } from "react-icons/fi";

import { TOAST_EVENT, type ToastEventDetail, type ToastKind } from "../toast";
import { cn } from "../util/cn";

type ToastItem = {
  id: number;
  kind: ToastKind;
  message: string;
};

function iconFor(kind: ToastKind) {
  if (kind === "success") return <FiCheckCircle className="text-emerald-500" />;
  if (kind === "error") return <FiXCircle className="text-rose-500" />;
  return <FiInfo className="text-sky-500" />;
}

export default function ToastHost() {
  const [items, setItems] = useState<ToastItem[]>([]);

  useEffect(() => {
    let nextId = 1;

    const onToast = (event: Event) => {
      const detail = (event as CustomEvent<ToastEventDetail>).detail;
      if (!detail?.message) return;

      const id = nextId++;
      const item: ToastItem = { id, kind: detail.kind, message: detail.message };
      setItems((prev) => {
        const merged = [item, ...prev];
        return merged.slice(0, 4);
      });

      window.setTimeout(() => {
        setItems((prev) => prev.filter((t) => t.id !== id));
      }, 3200);
    };

    window.addEventListener(TOAST_EVENT, onToast as EventListener);
    return () => window.removeEventListener(TOAST_EVENT, onToast as EventListener);
  }, []);

  return (
    <div className="pointer-events-none fixed right-3 top-3 z-50 flex w-[min(420px,calc(100vw-24px))] flex-col gap-2 sm:right-4 sm:top-4">
      {items.map((item) => (
        <div
          key={item.id}
          className={cn(
            "pointer-events-auto flex items-center gap-3 rounded-2xl bg-white/90 p-3 shadow-soft ring-1 ring-slate-200/70 backdrop-blur",
            "dark:bg-slate-900/80 dark:ring-slate-800/70"
          )}
        >
          <div className="text-lg">{iconFor(item.kind)}</div>
          <div className="min-w-0 flex-1">
            <div className="break-words text-sm font-semibold leading-5 text-slate-900 dark:text-slate-50">
              {item.message}
            </div>
          </div>
          <button
            className="grid h-8 w-8 place-items-center rounded-xl text-slate-500 hover:bg-slate-100 hover:text-slate-700 dark:text-slate-300 dark:hover:bg-slate-800 dark:hover:text-slate-50"
            onClick={() => setItems((prev) => prev.filter((t) => t.id !== item.id))}
            title="Close"
          >
            <FiX />
          </button>
        </div>
      ))}
    </div>
  );
}
