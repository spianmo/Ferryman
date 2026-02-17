import { useState } from "react";

import { useI18n } from "../i18n";

type Props = {
  loading: boolean;
  onLogin: (accessKey: string) => Promise<void>;
};

export default function LoginPage({ loading, onLogin }: Props) {
  const { t } = useI18n();
  const [key, setKey] = useState("");

  return (
    <div className="min-h-screen bg-slate-50 px-4 py-12 dark:bg-slate-950">
      <div className="mx-auto w-full max-w-md">
        <div className="rounded-3xl bg-white/80 p-6 shadow-soft ring-1 ring-slate-200/70 backdrop-blur dark:bg-slate-900/60 dark:ring-slate-800/70">
          <div className="flex items-center justify-between">
            <h1 className="text-lg font-semibold tracking-tight text-slate-900 dark:text-slate-50">
              {t("login.title")}
            </h1>
          </div>
          <p className="mt-2 text-sm text-slate-500 dark:text-slate-400">{t("login.subtitle")}</p>
        <form
          onSubmit={async (event) => {
            event.preventDefault();
            await onLogin(key.trim());
          }}
          className="mt-6 space-y-3"
        >
          <label className="block text-sm font-semibold text-slate-800 dark:text-slate-100" htmlFor="access-key">
            {t("login.access_key")}
          </label>
          <input
            id="access-key"
            type="password"
            value={key}
            onChange={(event) => setKey(event.target.value)}
            autoComplete="off"
            required
            placeholder={t("login.placeholder")}
            className="w-full rounded-2xl border border-slate-200 bg-white px-4 py-3 text-sm text-slate-900 shadow-sm outline-none placeholder:text-slate-400 focus:border-slate-300 dark:border-slate-800 dark:bg-slate-950/40 dark:text-slate-50 dark:placeholder:text-slate-500 dark:focus:border-slate-700"
          />
          <button
            type="submit"
            disabled={loading || key.trim().length === 0}
            className="inline-flex w-full items-center justify-center rounded-2xl bg-slate-900 px-4 py-3 text-sm font-semibold text-white shadow-sm transition hover:bg-slate-800 disabled:opacity-50 dark:bg-slate-50 dark:text-slate-900 dark:hover:bg-white"
          >
            {loading ? t("login.submitting") : t("login.submit")}
          </button>
        </form>
        </div>
      </div>
    </div>
  );
}
