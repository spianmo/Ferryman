import { MdEditor, MdPreview } from "md-editor-rt";
import type { Theme } from "../../../theme";
import "md-editor-rt/lib/style.css";
import "md-editor-rt/lib/preview.css";

type Props = {
  value: string;
  onChange: (value: string) => void;
  theme: Theme;
  preview: boolean;
  previewTheme: string;
  codeTheme: string;
};

export default function MdEditorPanel({ value, onChange, theme, preview, previewTheme, codeTheme }: Props) {
  return (
    <div className="mt-4 min-h-0 flex-1 overflow-auto rounded-2xl border border-slate-200 bg-white dark:border-neutral-800 dark:bg-neutral-950/50">
      {preview ? (
        <div className="h-full overflow-auto p-4">
          <MdPreview
            id="files-md-preview"
            modelValue={value}
            theme={theme === "dark" ? "dark" : "light"}
            previewTheme={previewTheme}
            codeTheme={codeTheme}
          />
        </div>
      ) : (
        <MdEditor
          modelValue={value}
          onChange={onChange}
          theme={theme === "dark" ? "dark" : "light"}
          preview={true}
          noUploadImg
          previewTheme={previewTheme}
          codeTheme={codeTheme}
          style={{ height: "100%" }}
        />
      )}
    </div>
  );
}
