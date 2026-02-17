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

if ("serviceWorker" in navigator) {
  window.addEventListener("load", () => {
    navigator.serviceWorker.register("/sw.js").catch(() => {
      // Best-effort PWA support.
    });
  });
}
