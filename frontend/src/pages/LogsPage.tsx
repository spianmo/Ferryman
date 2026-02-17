import { useEffect, useState } from "react";
import { toast } from "../toast";
import { FiRefreshCw } from "react-icons/fi";

import { getLogs } from "../api/client";
import { useI18n } from "../i18n";

type Props = {
  token: string;
};

export default function LogsPage({ token }: Props) {
  const { t } = useI18n();
  const [items, setItems] = useState<Array<Record<string, unknown>>>([]);

  const refresh = async (notify = false) => {
    const res = await getLogs(token, 300);
    if (!res.ok) {
      if (notify) {
        toast.error(res.error ?? t("toast.request_failed"));
      }
      return;
    }
    setItems(res.items ?? []);
  };

  useEffect(() => {
    void refresh();
    const timer = window.setInterval(() => void refresh(), 3000);
    return () => {
      window.clearInterval(timer);
    };
  }, [token]);

  return (
    <section className="rounded-3xl bg-white/70 p-4 shadow-soft ring-1 ring-slate-200/70 backdrop-blur dark:bg-slate-900/55 dark:ring-slate-800/70">
      <div className="flex items-start justify-between gap-3">
        <div>
          <h2 className="text-base font-semibold tracking-tight text-slate-900 dark:text-slate-50">
            {t("logs.title")}
          </h2>
          <div className="mt-1 text-xs text-slate-500 dark:text-slate-400">{items.length}</div>
        </div>
        <button
          className="inline-flex h-10 items-center gap-2 rounded-2xl bg-slate-100 px-4 text-sm font-semibold text-slate-700 hover:bg-slate-200 dark:bg-slate-800 dark:text-slate-100 dark:hover:bg-slate-700"
          onClick={() => void refresh(true)}
        >
          <FiRefreshCw /> {t("logs.refresh")}
        </button>
      </div>

      <div className="mt-4 max-h-[680px] space-y-2 overflow-auto pr-1">
        {items.map((item, idx) => (
          <pre
            key={idx}
            className="rounded-2xl bg-white/70 p-3 font-mono text-[12px] leading-relaxed text-slate-800 shadow-sm ring-1 ring-slate-200/60 dark:bg-slate-950/30 dark:text-slate-50 dark:ring-slate-800/70"
          >
            {JSON.stringify(item, null, 2)}
          </pre>
        ))}
        {items.length === 0 ? (
          <div className="rounded-2xl border border-dashed border-slate-200 p-5 text-center text-sm text-slate-500 dark:border-slate-800 dark:text-slate-400">
            {t("logs.empty")}
          </div>
        ) : null}
      </div>
    </section>
  );
}
