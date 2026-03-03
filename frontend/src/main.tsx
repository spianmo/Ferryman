import React from "react";
import ReactDOM from "react-dom/client";

import App from "./App";
import ToastHost from "./components/ToastHost";
import "./index.css";
import { I18nProvider } from "./i18n";
import { ThemeProvider } from "./theme";

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

if (import.meta.env.DEV && "serviceWorker" in navigator) {
  window.addEventListener("load", () => {
    void navigator.serviceWorker
      .getRegistrations()
      .then((registrations) => Promise.all(registrations.map((registration) => registration.unregister())))
      .catch(() => {
        // Best-effort cleanup for development mode.
      });

    if ("caches" in window) {
      void caches
        .keys()
        .then((keys) => Promise.all(keys.map((key) => caches.delete(key))))
        .catch(() => {
          // Best-effort cleanup for development mode.
        });
    }
  });
}

if (import.meta.env.PROD && "serviceWorker" in navigator) {
  window.addEventListener("load", () => {
    navigator.serviceWorker.register("/sw.js").catch(() => {
      // Best-effort PWA support.
    });
  });
}
