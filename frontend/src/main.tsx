import React from "react";
import ReactDOM from "react-dom/client";

import App from "./App";
import ToastHost from "./components/ToastHost";
import "./index.css";
import { I18nProvider } from "./i18n";
import { ThemeProvider } from "./theme";

const DEV_SW_RELOAD_KEY = "__ferryman_dev_sw_reload_once__";

async function unregisterAllServiceWorkers(): Promise<number> {
  if (!("serviceWorker" in navigator)) {
    return 0;
  }
  try {
    const registrations = await navigator.serviceWorker.getRegistrations();
    await Promise.all(registrations.map((registration) => registration.unregister()));
    return registrations.length;
  } catch {
    return 0;
  }
}

async function clearAllCaches(): Promise<number> {
  if (!("caches" in window)) {
    return 0;
  }
  try {
    const keys = await caches.keys();
    await Promise.all(keys.map((key) => caches.delete(key)));
    return keys.length;
  } catch {
    return 0;
  }
}

async function cleanupServiceWorkerState(): Promise<boolean> {
  const [registrationCount, cacheCount] = await Promise.all([
    unregisterAllServiceWorkers(),
    clearAllCaches(),
  ]);
  return registrationCount > 0 || cacheCount > 0;
}

async function registerProductionServiceWorker(): Promise<void> {
  if (!("serviceWorker" in navigator)) {
    return;
  }

  try {
    await navigator.serviceWorker.register("/sw.js", {
      scope: "/",
      updateViaCache: "none",
    });
  } catch {
    // Best-effort in production; app must keep running without offline mode.
  }
}

if (import.meta.env.DEV) {
  void (async () => {
    const cleaned = await cleanupServiceWorkerState();
    if (cleaned && sessionStorage.getItem(DEV_SW_RELOAD_KEY) !== "1") {
      sessionStorage.setItem(DEV_SW_RELOAD_KEY, "1");
      window.location.reload();
      return;
    }
    if (!cleaned) {
      sessionStorage.removeItem(DEV_SW_RELOAD_KEY);
    }
  })();
}

if (import.meta.env.PROD) {
  window.addEventListener("load", () => {
    void registerProductionServiceWorker();
  });
}

ReactDOM.createRoot(document.getElementById("root")!).render(
  <React.StrictMode>
    <ThemeProvider>
      <I18nProvider>
        <App />
        <ToastHost />
      </I18nProvider>
    </ThemeProvider>
  </React.StrictMode>
);
