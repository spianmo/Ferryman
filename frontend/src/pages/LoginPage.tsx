import { useState } from "react";
import {
  FiArrowRight,
  FiCommand,
  FiFolder,
  FiGlobe,
  FiMonitor,
  FiMoon,
  FiSun,
  FiTerminal,
} from "react-icons/fi";

import { useI18n } from "../i18n";
import { useTheme } from "../theme";

type Props = {
  loading: boolean;
  onLogin: (accessKey: string) => Promise<void>;
};

export default function LoginPage({ loading, onLogin }: Props) {
  const { t, lang, setLang } = useI18n();
  const { theme, toggle: toggleTheme } = useTheme();
  const [key, setKey] = useState("");
  const disabled = loading || key.trim().length === 0;
  const hostname = typeof window !== "undefined" ? window.location.hostname : "";
  const modules = [
    { icon: <FiFolder />, label: t("nav.files") },
    { icon: <FiTerminal />, label: t("nav.terminal") },
    { icon: <FiCommand />, label: t("nav.tasks") },
    { icon: <FiMonitor />, label: t("nav.screen") },
  ];

  return (
    <div className="relative min-h-[100dvh] w-full overflow-hidden bg-slate-100 p-3 dark:bg-neutral-950 sm:p-4">
      <div aria-hidden className="pointer-events-none absolute inset-0">
        <div className="absolute -left-28 -top-28 h-72 w-72 rounded-full bg-slate-300/40 blur-3xl dark:bg-neutral-700/25 sm:h-96 sm:w-96" />
        <div className="absolute -bottom-24 -right-16 h-72 w-72 rounded-full bg-slate-200/70 blur-3xl dark:bg-neutral-800/35 sm:h-96 sm:w-96" />
      </div>

      <div className="relative mx-auto flex min-h-[calc(100dvh-1.5rem)] w-full max-w-[1180px] flex-col sm:min-h-[calc(100dvh-2rem)]">
        <div className="mb-3 flex items-center justify-end gap-2 sm:mb-4">
          <button
            className="inline-flex h-10 items-center gap-2 rounded-2xl bg-white px-3 text-sm font-semibold text-slate-700 shadow-sm ring-1 ring-slate-200 transition-colors hover:bg-slate-50 dark:bg-neutral-950/45 dark:text-neutral-100 dark:ring-neutral-800 dark:hover:bg-neutral-900"
            onClick={() => setLang(lang === "en" ? "zh-CN" : "en")}
            title={t("top.lang")}
            aria-label={t("top.lang")}
          >
            <FiGlobe className="text-base" />
            <span>{lang === "en" ? "EN" : "中"}</span>
          </button>
          <button
            className="grid h-10 w-10 place-items-center rounded-2xl bg-white text-slate-700 shadow-sm ring-1 ring-slate-200 transition-colors hover:bg-slate-50 dark:bg-neutral-950/45 dark:text-neutral-100 dark:ring-neutral-800 dark:hover:bg-neutral-900"
            onClick={toggleTheme}
            title={t("top.theme")}
            aria-label={t("top.theme")}
          >
            {theme === "dark" ? <FiSun /> : <FiMoon />}
          </button>
        </div>

        <main className="flex min-h-0 flex-1 overflow-visible">
          <div className="grid w-full grid-cols-1 overflow-visible rounded-[30px] border border-slate-200/75 bg-white/80 shadow-[0_18px_40px_rgba(2,6,23,0.10)] backdrop-blur dark:border-neutral-800/75 dark:bg-neutral-900/55 lg:min-h-full lg:grid-cols-[minmax(0,1.2fr)_minmax(360px,430px)]">
            <section className="order-1 min-w-0 px-5 py-6 sm:px-7 sm:py-8 lg:order-1 lg:px-10 lg:py-10">
              <div className="flex flex-col gap-8 lg:h-full lg:justify-between">
                <div>
                  <div className="inline-flex items-center gap-2 rounded-full border border-slate-200/70 bg-white/85 px-3 py-1 text-[11px] font-semibold uppercase tracking-[0.12em] text-slate-600 dark:border-neutral-700/70 dark:bg-neutral-900/80 dark:text-neutral-300">
                    <span className="h-2 w-2 rounded-full bg-emerald-500" />
                    {hostname || "localhost"}
                  </div>
                  <div className="mt-5 sm:mt-6">
                    <p
                      aria-hidden
                      className="pointer-events-none select-none font-mono text-[clamp(2.2rem,8vw,5rem)] font-semibold uppercase leading-none tracking-[-0.06em] text-slate-300 dark:text-neutral-800"
                    >
                      {t("app.name")}
                    </p>
                    <h1 className="-mt-4 select-none font-mono text-[clamp(2.2rem,8vw,5rem)] font-semibold uppercase leading-none tracking-[-0.06em] text-slate-900 dark:text-neutral-100">
                      {t("app.name")}
                    </h1>
                  </div>
                  <p className="mt-5 max-w-[52ch] text-sm leading-relaxed text-slate-600 dark:text-neutral-300 sm:text-base">
                    {t("login.slogan")}
                  </p>
                </div>

                <div className="hidden rounded-3xl border border-slate-200/75 bg-white/70 p-2.5 dark:border-neutral-800/70 dark:bg-neutral-900/55 sm:p-3 lg:block">
                  <div className="grid grid-cols-2 gap-2.5">
                    {modules.map((item) => (
                      <div
                        key={item.label}
                        className="flex items-center gap-2.5 rounded-2xl border border-slate-200/70 bg-white/75 px-3 py-2.5 text-sm font-medium text-slate-700 transition-colors duration-200 hover:border-slate-300/80 hover:bg-white dark:border-neutral-700/70 dark:bg-neutral-800/70 dark:text-neutral-200 dark:hover:border-neutral-600 dark:hover:bg-neutral-800"
                      >
                        <span className="grid h-7 w-7 place-items-center rounded-xl bg-slate-900 text-[15px] text-white dark:bg-neutral-100 dark:text-neutral-900">
                          {item.icon}
                        </span>
                        <span className="truncate">{item.label}</span>
                      </div>
                    ))}
                  </div>
                </div>
              </div>
            </section>
            <section className="relative order-2 flex min-w-0 items-start px-4 pb-6 pt-4 before:pointer-events-none before:absolute before:left-4 before:right-4 before:top-0 before:h-px before:bg-slate-200/75 before:content-[''] dark:before:bg-neutral-800/70 sm:px-6 sm:pb-8 sm:pt-6 sm:before:left-6 sm:before:right-6 lg:order-2 lg:items-center lg:px-7 lg:py-8 lg:before:bottom-8 lg:before:left-0 lg:before:right-auto lg:before:top-8 lg:before:h-auto lg:before:w-px">
              <div className="mx-auto w-full max-w-[440px] rounded-3xl bg-white/90 p-5 shadow-soft ring-1 ring-slate-200/80 dark:bg-neutral-900/70 dark:ring-neutral-800/75 sm:p-6 lg:max-w-none">
                <p className="text-[1.55rem] font-semibold leading-tight text-slate-900 dark:text-neutral-50">
                  {lang === "en" ? "Sign in to continue" : "登录以继续"}
                </p>
                <p className="mt-2 text-sm text-slate-500 dark:text-neutral-400">
                  {t("login.subtitle")}
                </p>

                <form
                  onSubmit={async (event) => {
                    event.preventDefault();
                    await onLogin(key.trim());
                  }}
                  className="mt-6 space-y-4"
                >
                  <label
                    className="block text-xs font-semibold uppercase tracking-[0.12em] text-slate-600 dark:text-neutral-300"
                    htmlFor="access-key"
                  >
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
                    className="w-full rounded-2xl border border-slate-200 bg-white px-4 py-3 text-sm text-slate-900 shadow-sm outline-none transition-all placeholder:text-slate-400 focus:border-slate-300 focus:ring-2 focus:ring-slate-200 dark:border-neutral-800 dark:bg-neutral-950/45 dark:text-neutral-50 dark:placeholder:text-neutral-500 dark:focus:border-neutral-700 dark:focus:ring-neutral-800"
                  />
                  <button
                    type="submit"
                    disabled={disabled}
                    className="inline-flex w-full items-center justify-center gap-2 rounded-2xl bg-slate-900 px-4 py-3 text-sm font-semibold text-white shadow-sm transition-colors duration-200 hover:bg-slate-800 disabled:cursor-not-allowed disabled:opacity-55 dark:bg-neutral-50 dark:text-neutral-900 dark:hover:bg-white"
                  >
                    <span>{loading ? t("login.submitting") : t("login.submit")}</span>
                    <FiArrowRight className="text-base" />
                  </button>
                </form>

                <p className="mt-4 text-xs leading-relaxed text-slate-500 dark:text-neutral-400">
                  {lang === "en"
                    ? "Access Key is used locally for session bootstrap and is never shown in plain text."
                    : "Access Key 仅用于当前设备的会话初始化，输入内容不会明文展示。"}
                </p>
              </div>
            </section>
          </div>
        </main>
      </div>
    </div>
  );
}
