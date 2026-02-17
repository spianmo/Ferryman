import { useEffect, useMemo, useState } from "react";
import { toast } from "./toast";
import {
  FiClipboard,
  FiCommand,
  FiFolder,
  FiGlobe,
  FiLogOut,
  FiMenu,
  FiMoon,
  FiMonitor,
  FiSearch,
  FiSun,
  FiTerminal,
  FiX,
} from "react-icons/fi";

import { getSessionMe, login, UNAUTHORIZED_EVENT } from "./api/client";
import { useI18n } from "./i18n";
import FilesPage from "./pages/FilesPage";
import LoginPage from "./pages/LoginPage";
import LogsPage from "./pages/LogsPage";
import ScreenPage from "./pages/ScreenPage";
import TasksPage from "./pages/TasksPage";
import TerminalPage from "./pages/TerminalPage";
import { useTheme } from "./theme";
import type { SessionInfo } from "./types";
import { cn } from "./util/cn";

type TabKey = "files" | "terminal" | "tasks" | "screen" | "logs";

type NavItem = {
  key: TabKey;
  labelKey: string;
  icon: JSX.Element;
};

const navItems: NavItem[] = [
  { key: "files", labelKey: "nav.files", icon: <FiFolder /> },
  { key: "terminal", labelKey: "nav.terminal", icon: <FiTerminal /> },
  { key: "tasks", labelKey: "nav.tasks", icon: <FiCommand /> },
  { key: "screen", labelKey: "nav.screen", icon: <FiMonitor /> },
  { key: "logs", labelKey: "nav.logs", icon: <FiClipboard /> },
];

const SESSION_KEY = "ferryman.session";

function loadStoredSession(): SessionInfo | null {
  const raw = localStorage.getItem(SESSION_KEY);
  if (!raw) return null;
  try {
    return JSON.parse(raw) as SessionInfo;
  } catch {
    return null;
  }
}

function saveSession(session: SessionInfo | null) {
  if (!session) {
    localStorage.removeItem(SESSION_KEY);
    return;
  }
  localStorage.setItem(SESSION_KEY, JSON.stringify(session));
}

export default function App() {
  const { t, lang, setLang } = useI18n();
  const { theme, toggle: toggleTheme } = useTheme();

  const [session, setSession] = useState<SessionInfo | null>(() => loadStoredSession());
  const [activeTab, setActiveTab] = useState<TabKey>("files");
  const [loadingLogin, setLoadingLogin] = useState(false);
  const [navOpen, setNavOpen] = useState(false);
  const [sidebarCollapsed, setSidebarCollapsed] = useState(false);
  const [search, setSearch] = useState("");

  useEffect(() => {
    if (!session) return;
    void getSessionMe(session.token);
  }, [session]);

  useEffect(() => {
    const onUnauthorized = (event: Event) => {
      const detail = (event as CustomEvent<{ reason?: string }>).detail;
      setSession(null);
      saveSession(null);
      toast.error(detail?.reason ?? t("toast.session_expired"));
      setNavOpen(false);
    };
    window.addEventListener(UNAUTHORIZED_EVENT, onUnauthorized as EventListener);
    return () => window.removeEventListener(UNAUTHORIZED_EVENT, onUnauthorized as EventListener);
  }, [t]);

  const doLogin = async (accessKey: string) => {
    setLoadingLogin(true);
    try {
      const res = await login(accessKey);
      if (!res.ok) {
        toast.error(res.error ?? t("toast.login_failed"));
        return;
      }

      const created: SessionInfo = {
        token: res.session_token,
        host: res.host,
        httpPort: Number(res.http_port),
        wsPort: Number(res.ws_port),
      };
      setSession(created);
      saveSession(created);
      toast.success(t("toast.login_ok"));
    } finally {
      setLoadingLogin(false);
    }
  };

  const page = useMemo(() => {
    if (!session) return null;
    switch (activeTab) {
      case "files":
        return <FilesPage token={session.token} query={search} />;
      case "terminal":
        return <TerminalPage session={session} />;
      case "tasks":
        return <TasksPage token={session.token} />;
      case "screen":
        return <ScreenPage session={session} />;
      case "logs":
        return <LogsPage token={session.token} />;
      default:
        return null;
    }
  }, [activeTab, search, session]);

  if (!session) {
    return <LoginPage loading={loadingLogin} onLogin={doLogin} />;
  }

  const activeItem = navItems.find((item) => item.key === activeTab) ?? navItems[0];

  return (
    <div className="min-h-screen">
      <div className="mx-auto max-w-[1560px] p-3 sm:p-4">
        <div
          className={cn(
            "grid grid-cols-1 gap-4",
            sidebarCollapsed ? "lg:grid-cols-[72px_1fr]" : "lg:grid-cols-[72px_280px_1fr]"
          )}
        >
          <aside className="hidden lg:flex">
            <div className="flex w-full flex-col items-center gap-2 rounded-3xl bg-white/70 p-2 shadow-soft ring-1 ring-slate-200/70 backdrop-blur dark:bg-slate-900/55 dark:ring-slate-800/70">
              <button
                className="grid h-11 w-11 place-items-center rounded-2xl bg-slate-100 text-slate-700 hover:bg-slate-200 dark:bg-slate-800 dark:text-slate-100 dark:hover:bg-slate-700"
                onClick={() => setSidebarCollapsed((prev) => !prev)}
                title="Toggle"
              >
                <FiMenu />
              </button>
              <div className="my-1 h-px w-10 bg-slate-200/70 dark:bg-slate-800/70" />
              {navItems.map((item) => (
                <button
                  key={item.key}
                  className={cn(
                    "grid h-11 w-11 place-items-center rounded-2xl text-slate-700 hover:bg-slate-100 dark:text-slate-100 dark:hover:bg-slate-800",
                    activeTab === item.key &&
                      "bg-slate-900 text-white hover:bg-slate-900 dark:bg-slate-50 dark:text-slate-900 dark:hover:bg-slate-50"
                  )}
                  onClick={() => setActiveTab(item.key)}
                  title={t(item.labelKey)}
                >
                  {item.icon}
                </button>
              ))}
            </div>
          </aside>

          {!sidebarCollapsed ? (
          <aside className="hidden lg:flex">
            <div className="flex w-full min-h-[calc(100vh-2rem)] flex-col justify-between rounded-3xl bg-white/70 p-4 shadow-soft ring-1 ring-slate-200/70 backdrop-blur dark:bg-slate-900/55 dark:ring-slate-800/70">
              <div>
                <div className="flex items-center justify-between gap-3">
                  <div>
                    <div className="text-base font-semibold tracking-tight">{t("app.name")}</div>
                    <div className="mt-1 text-xs text-slate-500 dark:text-slate-400">
                      {window.location.hostname}
                    </div>
                  </div>
                </div>

                <div className="mt-5 space-y-1">
                  {navItems.map((item) => (
                    <button
                      key={item.key}
                      className={cn(
                        "flex w-full items-center gap-3 rounded-2xl border border-transparent px-3 py-2 text-left text-sm font-semibold text-slate-700 hover:bg-slate-100 dark:text-slate-100 dark:hover:bg-slate-800",
                        activeTab === item.key &&
                          "bg-slate-900 text-white hover:bg-slate-900 dark:bg-slate-50 dark:text-slate-900 dark:hover:bg-slate-50"
                      )}
                      onClick={() => setActiveTab(item.key)}
                    >
                      <span className="text-base">{item.icon}</span>
                      <span>{t(item.labelKey)}</span>
                    </button>
                  ))}
                </div>
              </div>

              <button
                className="flex items-center justify-center gap-2 rounded-2xl border border-slate-200 bg-white px-3 py-2 text-sm font-semibold text-slate-700 hover:bg-slate-50 dark:border-slate-800 dark:bg-slate-950/40 dark:text-slate-100 dark:hover:bg-slate-900"
                onClick={() => {
                  setSession(null);
                  saveSession(null);
                }}
              >
                <FiLogOut /> {t("nav.logout")}
              </button>
            </div>
          </aside>
          ) : null}

          <main className="min-w-0">
            <header className="sticky top-0 z-20">
              <div className="rounded-3xl bg-white/70 px-3 py-2 shadow-soft ring-1 ring-slate-200/70 backdrop-blur dark:bg-slate-900/55 dark:ring-slate-800/70">
                <div className="flex items-center gap-3">
                  <button
                    className="grid h-10 w-10 place-items-center rounded-2xl bg-slate-100 text-slate-700 hover:bg-slate-200 dark:bg-slate-800 dark:text-slate-100 dark:hover:bg-slate-700 lg:hidden"
                    onClick={() => setNavOpen(true)}
                    title="Menu"
                  >
                    <FiMenu />
                  </button>

                  <div className="min-w-0">
                    <div className="truncate text-sm font-semibold text-slate-900 dark:text-slate-50">
                      {t(activeItem.labelKey)}
                    </div>
                    <div className="mt-0.5 truncate font-mono text-[11px] text-slate-500 dark:text-slate-400">
                      {session.token.slice(0, 10)}…
                    </div>
                  </div>

                  <div className="hidden flex-1 md:block">
                    <div className="relative">
                      <FiSearch className="pointer-events-none absolute left-3 top-1/2 -translate-y-1/2 text-slate-400" />
                      <input
                        value={search}
                        onChange={(e) => setSearch(e.target.value)}
                        placeholder={t("top.search")}
                        className="w-full rounded-2xl border border-slate-200 bg-white/80 py-2 pl-10 pr-3 text-sm text-slate-900 shadow-sm outline-none ring-0 placeholder:text-slate-400 focus:border-slate-300 dark:border-slate-800 dark:bg-slate-950/40 dark:text-slate-50 dark:placeholder:text-slate-500 dark:focus:border-slate-700"
                      />
                    </div>
                  </div>

                  <div className="flex items-center gap-2">
                    <button
                      className="grid h-10 w-10 place-items-center rounded-2xl bg-white text-slate-700 shadow-sm ring-1 ring-slate-200 hover:bg-slate-50 dark:bg-slate-950/40 dark:text-slate-100 dark:ring-slate-800 dark:hover:bg-slate-900"
                      onClick={() => setLang(lang === "en" ? "zh-CN" : "en")}
                      title={t("top.lang")}
                    >
                      <FiGlobe />
                    </button>
                    <button
                      className="grid h-10 w-10 place-items-center rounded-2xl bg-white text-slate-700 shadow-sm ring-1 ring-slate-200 hover:bg-slate-50 dark:bg-slate-950/40 dark:text-slate-100 dark:ring-slate-800 dark:hover:bg-slate-900"
                      onClick={toggleTheme}
                      title={t("top.theme")}
                    >
                      {theme === "dark" ? <FiSun /> : <FiMoon />}
                    </button>
                    <button
                      className="hidden h-10 items-center gap-2 rounded-2xl bg-slate-900 px-4 text-sm font-semibold text-white shadow-sm hover:bg-slate-800 dark:bg-slate-50 dark:text-slate-900 dark:hover:bg-white sm:inline-flex"
                      onClick={() => {
                        setSession(null);
                        saveSession(null);
                      }}
                    >
                      <FiLogOut /> {t("nav.logout")}
                    </button>
                  </div>
                </div>

                <div className="mt-3 md:hidden">
                  <div className="relative">
                    <FiSearch className="pointer-events-none absolute left-3 top-1/2 -translate-y-1/2 text-slate-400" />
                    <input
                      value={search}
                      onChange={(e) => setSearch(e.target.value)}
                      placeholder={t("top.search")}
                      className="w-full rounded-2xl border border-slate-200 bg-white/80 py-2 pl-10 pr-3 text-sm text-slate-900 shadow-sm outline-none ring-0 placeholder:text-slate-400 focus:border-slate-300 dark:border-slate-800 dark:bg-slate-950/40 dark:text-slate-50 dark:placeholder:text-slate-500 dark:focus:border-slate-700"
                    />
                  </div>
                </div>
              </div>
            </header>

            <div className="mt-4">{page}</div>
          </main>
        </div>
      </div>

      <div
        className={cn(
          "fixed inset-0 z-40 lg:hidden",
          navOpen ? "pointer-events-auto" : "pointer-events-none"
        )}
      >
        <div
          className={cn(
            "absolute inset-0 bg-slate-950/30 backdrop-blur-sm transition-opacity",
            navOpen ? "opacity-100" : "opacity-0"
          )}
          onClick={() => setNavOpen(false)}
          role="presentation"
        />
        <div
          className={cn(
            "absolute left-0 top-0 h-full w-[86vw] max-w-sm p-4 transition-transform",
            navOpen ? "translate-x-0" : "-translate-x-full"
          )}
        >
          <div className="flex h-full flex-col justify-between rounded-3xl bg-white p-4 shadow-soft ring-1 ring-slate-200 dark:bg-slate-900 dark:ring-slate-800">
            <div>
              <div className="flex items-center justify-between">
                <div>
                  <div className="text-base font-semibold tracking-tight">{t("app.name")}</div>
                  <div className="mt-1 text-xs text-slate-500 dark:text-slate-400">
                    {window.location.hostname}
                  </div>
                </div>
                <button
                  className="grid h-10 w-10 place-items-center rounded-2xl bg-slate-100 text-slate-700 hover:bg-slate-200 dark:bg-slate-800 dark:text-slate-100 dark:hover:bg-slate-700"
                  onClick={() => setNavOpen(false)}
                  title="Close"
                >
                  <FiX />
                </button>
              </div>

              <div className="mt-5 space-y-1">
                {navItems.map((item) => (
                  <button
                    key={item.key}
                    className={cn(
                      "flex w-full items-center gap-3 rounded-2xl border border-transparent px-3 py-2 text-left text-sm font-semibold text-slate-700 hover:bg-slate-100 dark:text-slate-100 dark:hover:bg-slate-800",
                      activeTab === item.key &&
                        "bg-slate-900 text-white hover:bg-slate-900 dark:bg-slate-50 dark:text-slate-900 dark:hover:bg-slate-50"
                    )}
                    onClick={() => {
                      setActiveTab(item.key);
                      setNavOpen(false);
                    }}
                  >
                    <span className="text-base">{item.icon}</span>
                    <span>{t(item.labelKey)}</span>
                  </button>
                ))}
              </div>
            </div>

            <button
              className="flex items-center justify-center gap-2 rounded-2xl border border-slate-200 bg-white px-3 py-2 text-sm font-semibold text-slate-700 hover:bg-slate-50 dark:border-slate-800 dark:bg-slate-950/40 dark:text-slate-100 dark:hover:bg-slate-900"
              onClick={() => {
                setSession(null);
                saveSession(null);
                setNavOpen(false);
              }}
            >
              <FiLogOut /> {t("nav.logout")}
            </button>
          </div>
        </div>
      </div>
    </div>
  );
}
