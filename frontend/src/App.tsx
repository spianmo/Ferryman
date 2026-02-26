import { useCallback, useEffect, useMemo, useRef, useState } from "react";
import { toast } from "./toast";
import {
  FiActivity,
  FiBox,
  FiClipboard,
  FiCommand,
  FiFolder,
  FiGlobe,
  FiLogOut,
  FiMenu,
  FiMoon,
  FiMonitor,
  FiSearch,
  FiServer,
  FiSun,
  FiTerminal,
  FiX,
} from "react-icons/fi";

import { getSessionMe, login, UNAUTHORIZED_EVENT } from "./api/client";
import { useI18n } from "./i18n";
import DockerPage from "./pages/DockerPage";
import FilesPage from "./pages/FilesPage";
import LoginPage from "./pages/LoginPage";
import LogsPage from "./pages/LogsPage";
import DockurrPage from "./pages/DockurrPage";
import MonitorPage from "./pages/MonitorPage";
import ScreenPage from "./pages/ScreenPage";
import TasksPage from "./pages/TasksPage";
import TerminalPage from "./pages/TerminalPage";
import TunnelPage from "./pages/TunnelPage";
import { useTheme } from "./theme";
import type { SessionEnvironment, SessionInfo } from "./types";
import { cn } from "./util/cn";

type TabKey = "files" | "terminal" | "tasks" | "dockurr" | "docker" | "screen" | "monitor" | "tunnel" | "logs";

type NavItem = {
  key: TabKey;
  labelKey: string;
  icon: JSX.Element;
};

const navItems: NavItem[] = [
  { key: "monitor", labelKey: "nav.monitor", icon: <FiActivity /> },
  { key: "tunnel", labelKey: "nav.tunnel", icon: <FiGlobe /> },
  { key: "docker", labelKey: "nav.docker", icon: <FiBox /> },
  { key: "files", labelKey: "nav.files", icon: <FiFolder /> },
  { key: "terminal", labelKey: "nav.terminal", icon: <FiTerminal /> },
  { key: "tasks", labelKey: "nav.tasks", icon: <FiCommand /> },
  { key: "dockurr", labelKey: "nav.dockurr", icon: <FiServer /> },
  { key: "screen", labelKey: "nav.screen", icon: <FiMonitor /> },
  { key: "logs", labelKey: "nav.logs", icon: <FiClipboard /> },
];

const SESSION_KEY = "ferryman.session";
const SIDEBAR_COLLAPSED_KEY = "ferryman.sidebar.collapsed";
const DEFAULT_TAB: TabKey = "monitor";

const VALID_TABS: TabKey[] = ["files", "terminal", "tasks", "dockurr", "docker", "screen", "monitor", "tunnel", "logs"];

const DEFAULT_SESSION_ENV: SessionEnvironment = {
  host_os: "unknown",
  docker_installed: true,
  kvm_installed: true,
};

function isTabKey(value: string): value is TabKey {
  return VALID_TABS.includes(value as TabKey);
}

function tabFromHash(hash: string): TabKey | null {
  const normalized = hash.replace(/^#\/?/, "").trim();
  if (!normalized) return null;
  return isTabKey(normalized) ? normalized : null;
}

function hashForTab(tab: TabKey) {
  return `#/${tab}`;
}

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
  const [activeTab, setActiveTab] = useState<TabKey>(() => {
    if (typeof window === "undefined") return DEFAULT_TAB;
    return tabFromHash(window.location.hash) ?? DEFAULT_TAB;
  });
  const [loadingLogin, setLoadingLogin] = useState(false);
  const [navOpen, setNavOpen] = useState(false);
  const sessionRef = useRef<SessionInfo | null>(session);
  const unauthorizedNotifiedRef = useRef(false);
  const [sidebarCollapsed, setSidebarCollapsed] = useState(() => {
    if (typeof window === "undefined") return false;
    const raw = window.localStorage.getItem(SIDEBAR_COLLAPSED_KEY);
    return raw === "1" || raw === "true";
  });
  const [search, setSearch] = useState("");
  const [sessionEnv, setSessionEnv] = useState<SessionEnvironment>(DEFAULT_SESSION_ENV);

  const refreshSessionEnv = useCallback(async (token: string) => {
    const res = await getSessionMe(token);
    if (!res.ok) {
      return;
    }
    setSessionEnv({
      host_os: typeof res.host_os === "string" ? res.host_os.toLowerCase() : "unknown",
      docker_installed: res.docker_installed !== false,
      kvm_installed: res.kvm_installed !== false,
    });
  }, []);

  useEffect(() => {
    sessionRef.current = session;
    if (session) {
      unauthorizedNotifiedRef.current = false;
    } else {
      setSessionEnv(DEFAULT_SESSION_ENV);
    }
  }, [session]);

  useEffect(() => {
    if (!session) return;
    void refreshSessionEnv(session.token);
  }, [refreshSessionEnv, session]);

  useEffect(() => {
    const onUnauthorized = (event: Event) => {
      const detail = (event as CustomEvent<{ reason?: string }>).detail;
      if (!sessionRef.current || unauthorizedNotifiedRef.current) {
        return;
      }
      unauthorizedNotifiedRef.current = true;
      sessionRef.current = null;
      setSession(null);
      saveSession(null);
      toast.error(detail?.reason ?? t("toast.session_expired"));
      setNavOpen(false);
    };
    window.addEventListener(UNAUTHORIZED_EVENT, onUnauthorized as EventListener);
    return () => window.removeEventListener(UNAUTHORIZED_EVENT, onUnauthorized as EventListener);
  }, [t]);

  const visibleNavItems = useMemo(() => {
    return navItems.filter((item) => {
      if (item.key === "dockurr") {
        return sessionEnv.host_os === "linux";
      }
      return true;
    });
  }, [sessionEnv.host_os]);

  useEffect(() => {
    if (!session) return;
    if (visibleNavItems.some((item) => item.key === activeTab)) {
      return;
    }
    const fallback = visibleNavItems[0]?.key ?? DEFAULT_TAB;
    setActiveTab(fallback);
    const nextHash = hashForTab(fallback);
    if (window.location.hash !== nextHash) {
      window.history.replaceState(null, "", nextHash);
    }
  }, [activeTab, session, visibleNavItems]);

  useEffect(() => {
    const onHashChange = () => {
      const next = tabFromHash(window.location.hash);
      if (next) {
        setActiveTab(next);
      }
    };
    window.addEventListener("hashchange", onHashChange);

    const initial = tabFromHash(window.location.hash);
    if (initial) {
      setActiveTab(initial);
    } else {
      window.history.replaceState(null, "", hashForTab(activeTab));
    }

    return () => window.removeEventListener("hashchange", onHashChange);
  }, []);

  useEffect(() => {
    if (typeof window === "undefined") return;
    window.localStorage.setItem(SIDEBAR_COLLAPSED_KEY, sidebarCollapsed ? "1" : "0");
  }, [sidebarCollapsed]);

  const goTab = (tab: TabKey) => {
    setActiveTab(tab);
    const nextHash = hashForTab(tab);
    if (window.location.hash !== nextHash) {
      window.location.hash = nextHash;
    }
  };

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
      case "dockurr":
        return (
          <DockurrPage
            session={session}
            hostOs={sessionEnv.host_os}
            kvmInstalled={sessionEnv.kvm_installed}
          />
        );
      case "docker":
        return (
          <DockerPage
            session={session}
            hostOs={sessionEnv.host_os}
            dockerInstalled={sessionEnv.docker_installed}
          />
        );
      case "screen":
        return <ScreenPage session={session} />;
      case "monitor":
        return <MonitorPage session={session} />;
      case "tunnel":
        return <TunnelPage token={session.token} />;
      case "logs":
        return <LogsPage session={session} />;
      default:
        return null;
    }
  }, [activeTab, search, session, sessionEnv.docker_installed, sessionEnv.host_os, sessionEnv.kvm_installed]);

  if (!session) {
    return <LoginPage loading={loadingLogin} onLogin={doLogin} />;
  }

  const activeItem = visibleNavItems.find((item) => item.key === activeTab) ?? visibleNavItems[0] ?? navItems[0];

  return (
    <div className="min-h-screen">
      <div className="mx-auto max-w-[1560px] p-3 sm:p-4">
        <div
          className={cn(
            "grid grid-cols-1 gap-4 lg:transition-[grid-template-columns] lg:duration-300 lg:ease-in-out",
            sidebarCollapsed ? "lg:grid-cols-[72px_0px_1fr]" : "lg:grid-cols-[72px_280px_1fr]"
          )}
        >
          <aside className="hidden lg:flex">
            <div className="flex w-full flex-col items-center gap-2 rounded-3xl bg-white/70 p-2 shadow-soft ring-1 ring-slate-200/70 backdrop-blur dark:bg-neutral-900/55 dark:ring-neutral-800/70">
              <button
                className="grid h-11 w-11 place-items-center rounded-2xl bg-slate-100 text-slate-700 transition-colors duration-200 hover:bg-slate-200 dark:bg-neutral-800 dark:text-neutral-100 dark:hover:bg-neutral-700"
                onClick={() => setSidebarCollapsed((prev) => !prev)}
                title="Toggle"
              >
                <FiMenu />
              </button>
              <div className="my-1 h-px w-10 bg-slate-200/70 dark:bg-neutral-800/70" />
              {visibleNavItems.map((item) => (
                <button
                  key={item.key}
                  className={cn(
                    "grid h-11 w-11 place-items-center rounded-2xl text-slate-700 transition-[background-color,color,transform,box-shadow] duration-200 ease-out hover:bg-slate-100 active:scale-95 dark:text-neutral-100 dark:hover:bg-neutral-800",
                    activeTab === item.key &&
                      "bg-slate-900 text-white shadow-sm hover:bg-slate-900 dark:bg-neutral-50 dark:text-neutral-900 dark:hover:bg-neutral-50"
                  )}
                  onClick={() => goTab(item.key)}
                  title={t(item.labelKey)}
                >
                  {item.icon}
                </button>
              ))}
            </div>
          </aside>

          <aside className="hidden lg:flex">
            <div
              className={cn(
                "flex w-[280px] min-w-[280px] shrink-0 min-h-[calc(100vh-2rem)] flex-col justify-between rounded-3xl bg-white/70 p-4 shadow-soft ring-1 ring-slate-200/70 backdrop-blur transition-[opacity,transform] duration-300 ease-out dark:bg-neutral-900/55 dark:ring-neutral-800/70",
                sidebarCollapsed ? "pointer-events-none -translate-x-3 opacity-0" : "translate-x-0 opacity-100"
              )}
            >
              <div>
                <div className="flex items-center justify-between gap-3">
                  <div className="min-w-0">
                    <div className="truncate whitespace-nowrap text-base font-semibold tracking-tight">
                      {t("app.name")}
                    </div>
                    <div className="mt-1 truncate whitespace-nowrap text-xs text-slate-500 dark:text-neutral-400">
                      {window.location.hostname}
                    </div>
                  </div>
                </div>

                <div className="mt-5 space-y-1">
                  {visibleNavItems.map((item) => (
                    <button
                      key={item.key}
                      className={cn(
                        "flex w-full min-w-0 items-center gap-3 rounded-2xl border border-transparent px-3 py-2 text-left text-sm font-semibold text-slate-700 transition-[background-color,color,transform,box-shadow] duration-200 ease-out hover:bg-slate-100 active:scale-[0.99] dark:text-neutral-100 dark:hover:bg-neutral-800",
                        activeTab === item.key &&
                          "bg-slate-900 text-white shadow-sm hover:bg-slate-900 dark:bg-neutral-50 dark:text-neutral-900 dark:hover:bg-neutral-50"
                      )}
                      onClick={() => goTab(item.key)}
                    >
                      <span className="text-base">{item.icon}</span>
                      <span className="min-w-0 flex-1 truncate whitespace-nowrap">
                        {t(item.labelKey)}
                      </span>
                    </button>
                  ))}
                </div>
              </div>

              <button
                className="flex items-center justify-center gap-2 whitespace-nowrap rounded-2xl border border-slate-200 bg-white px-3 py-2 text-sm font-semibold text-slate-700 transition-colors duration-200 hover:bg-slate-50 dark:border-neutral-800 dark:bg-neutral-950/40 dark:text-neutral-100 dark:hover:bg-neutral-900"
                onClick={() => {
                  setSession(null);
                  saveSession(null);
                }}
              >
                <FiLogOut /> {t("nav.logout")}
              </button>
            </div>
          </aside>

          <main className="min-w-0 flex h-[calc(100vh-2rem)] flex-col">
            <header className="sticky top-0 z-20">
              <div className="rounded-3xl bg-white/70 px-3 py-2 shadow-[0_8px_20px_rgba(2,6,23,0.06)] ring-1 ring-slate-200/70 backdrop-blur dark:bg-neutral-900/55 dark:ring-neutral-800/70">
                <div className="flex items-center gap-3">
                  <button
                    className="grid h-10 w-10 place-items-center rounded-2xl bg-slate-100 text-slate-700 hover:bg-slate-200 dark:bg-neutral-800 dark:text-neutral-100 dark:hover:bg-neutral-700 lg:hidden"
                    onClick={() => setNavOpen(true)}
                    title="Menu"
                  >
                    <FiMenu />
                  </button>

                  <div className="min-w-0 flex-1 md:flex-none">
                    <div className="truncate text-sm font-semibold text-slate-900 dark:text-neutral-50">
                      {t(activeItem.labelKey)}
                    </div>
                    <div className="mt-0.5 truncate font-mono text-[11px] text-slate-500 dark:text-neutral-400">
                      {session.token.slice(0, 10)}…
                    </div>
                  </div>

                  {activeTab === "files" ? (
                    <div className="hidden flex-1 md:block">
                      <div className="relative">
                        <FiSearch className="pointer-events-none absolute left-3 top-1/2 -translate-y-1/2 text-slate-400" />
                        <input
                          value={search}
                          onChange={(e) => setSearch(e.target.value)}
                          placeholder={t("top.search")}
                          className="w-full rounded-2xl border border-slate-200 bg-white/80 py-2 pl-10 pr-3 text-sm text-slate-900 shadow-sm outline-none ring-0 placeholder:text-slate-400 focus:border-slate-300 dark:border-neutral-800 dark:bg-neutral-950/40 dark:text-neutral-50 dark:placeholder:text-neutral-500 dark:focus:border-neutral-700"
                        />
                      </div>
                    </div>
                  ) : null}

                  <div className="ml-auto flex shrink-0 items-center gap-2">
                    <button
                      className="grid h-10 w-10 place-items-center rounded-2xl bg-white text-slate-700 shadow-sm ring-1 ring-slate-200 hover:bg-slate-50 dark:bg-neutral-950/40 dark:text-neutral-100 dark:ring-neutral-800 dark:hover:bg-neutral-900"
                      onClick={() => setLang(lang === "en" ? "zh-CN" : "en")}
                      title={t("top.lang")}
                    >
                      <FiGlobe />
                    </button>
                    <button
                      className="grid h-10 w-10 place-items-center rounded-2xl bg-white text-slate-700 shadow-sm ring-1 ring-slate-200 hover:bg-slate-50 dark:bg-neutral-950/40 dark:text-neutral-100 dark:ring-neutral-800 dark:hover:bg-neutral-900"
                      onClick={toggleTheme}
                      title={t("top.theme")}
                    >
                      {theme === "dark" ? <FiSun /> : <FiMoon />}
                    </button>
                    <button
                      className="hidden h-10 items-center gap-2 rounded-2xl bg-slate-900 px-4 text-sm font-semibold text-white shadow-sm hover:bg-slate-800 dark:bg-neutral-50 dark:text-neutral-900 dark:hover:bg-white sm:inline-flex"
                      onClick={() => {
                        setSession(null);
                        saveSession(null);
                      }}
                    >
                      <FiLogOut /> {t("nav.logout")}
                    </button>
                  </div>
                </div>

                {activeTab === "files" ? (
                  <div className="mt-3 md:hidden">
                    <div className="relative">
                      <FiSearch className="pointer-events-none absolute left-3 top-1/2 -translate-y-1/2 text-slate-400" />
                      <input
                        value={search}
                        onChange={(e) => setSearch(e.target.value)}
                        placeholder={t("top.search")}
                        className="w-full rounded-2xl border border-slate-200 bg-white/80 py-2 pl-10 pr-3 text-sm text-slate-900 shadow-sm outline-none ring-0 placeholder:text-slate-400 focus:border-slate-300 dark:border-neutral-800 dark:bg-neutral-950/40 dark:text-neutral-50 dark:placeholder:text-neutral-500 dark:focus:border-neutral-700"
                      />
                    </div>
                  </div>
                ) : null}
              </div>
            </header>

            <div className="mt-4 min-h-0 flex-1">
              <div className="h-[calc(100%+1.5rem)] -mx-3 -my-3 px-3 py-3">
                {page}
              </div>
            </div>
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
            "absolute inset-0 bg-neutral-950/30 backdrop-blur-sm transition-opacity",
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
          <div className="flex h-full flex-col justify-between rounded-3xl bg-white p-4 shadow-soft ring-1 ring-slate-200 dark:bg-neutral-900 dark:ring-neutral-800">
            <div>
              <div className="flex items-center justify-between">
                <div>
                  <div className="text-base font-semibold tracking-tight">{t("app.name")}</div>
                  <div className="mt-1 text-xs text-slate-500 dark:text-neutral-400">
                    {window.location.hostname}
                  </div>
                </div>
                <button
                  className="grid h-10 w-10 place-items-center rounded-2xl bg-slate-100 text-slate-700 hover:bg-slate-200 dark:bg-neutral-800 dark:text-neutral-100 dark:hover:bg-neutral-700"
                  onClick={() => setNavOpen(false)}
                  title="Close"
                >
                  <FiX />
                </button>
              </div>

              <div className="mt-5 space-y-1">
                {visibleNavItems.map((item) => (
                  <button
                    key={item.key}
                    className={cn(
                      "flex w-full items-center gap-3 rounded-2xl border border-transparent px-3 py-2 text-left text-sm font-semibold text-slate-700 transition-[background-color,color,transform,box-shadow] duration-200 ease-out hover:bg-slate-100 active:scale-[0.99] dark:text-neutral-100 dark:hover:bg-neutral-800",
                      activeTab === item.key &&
                        "bg-slate-900 text-white shadow-sm hover:bg-slate-900 dark:bg-neutral-50 dark:text-neutral-900 dark:hover:bg-neutral-50"
                    )}
                    onClick={() => {
                      goTab(item.key);
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
              className="flex items-center justify-center gap-2 rounded-2xl border border-slate-200 bg-white px-3 py-2 text-sm font-semibold text-slate-700 hover:bg-slate-50 dark:border-neutral-800 dark:bg-neutral-950/40 dark:text-neutral-100 dark:hover:bg-neutral-900"
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
