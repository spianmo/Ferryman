import React, { createContext, useContext, useEffect, useMemo, useState } from "react";

export type Theme = "light" | "dark";

type ThemeApi = {
  theme: Theme;
  setTheme: (next: Theme) => void;
  toggle: () => void;
};

const ThemeContext = createContext<ThemeApi | null>(null);

const STORAGE_KEY = "ferryman.theme";

function preferredTheme(): Theme {
  if (typeof window === "undefined") return "light";
  return window.matchMedia && window.matchMedia("(prefers-color-scheme: dark)").matches ? "dark" : "light";
}

function applyTheme(theme: Theme) {
  const root = document.documentElement;
  root.classList.toggle("dark", theme === "dark");
}

export function ThemeProvider({ children }: { children: React.ReactNode }) {
  const [theme, setThemeState] = useState<Theme>(() => {
    const stored = localStorage.getItem(STORAGE_KEY) as Theme | null;
    return stored === "dark" || stored === "light" ? stored : preferredTheme();
  });

  useEffect(() => {
    applyTheme(theme);
    localStorage.setItem(STORAGE_KEY, theme);
  }, [theme]);

  const api = useMemo<ThemeApi>(() => {
    return {
      theme,
      setTheme: setThemeState,
      toggle: () => setThemeState((prev) => (prev === "dark" ? "light" : "dark")),
    };
  }, [theme]);

  return <ThemeContext.Provider value={api}>{children}</ThemeContext.Provider>;
}

export function useTheme() {
  const ctx = useContext(ThemeContext);
  if (!ctx) throw new Error("useTheme must be used within ThemeProvider");
  return ctx;
}

