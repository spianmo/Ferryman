import MonacoEditor from "@monaco-editor/react";
import type { Theme } from "../../../theme";

type Props = {
  language: string;
  value: string;
  onChange: (value: string) => void;
  theme: Theme;
};

export default function EditorPreview({ language, value, onChange, theme }: Props) {
  return (
    <div className="mt-4 min-h-0 flex-1 overflow-hidden rounded-2xl border border-slate-200 bg-white shadow-sm dark:border-neutral-800 dark:bg-neutral-950/50">
      <MonacoEditor
        language={language}
        value={value}
        onChange={(next) => onChange(next ?? "")}
        theme={theme === "dark" ? "vs-dark" : "vs"}
        options={{
          minimap: { enabled: false },
          fontSize: 13,
          fontFamily: "IBM Plex Mono, ui-monospace, SFMono-Regular, Menlo, monospace",
          wordWrap: "on",
          scrollBeyondLastLine: false,
          automaticLayout: true,
        }}
      />
    </div>
  );
}
