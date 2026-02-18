import type { SpreadsheetPreviewData } from "./types";

type Props = {
  data: SpreadsheetPreviewData;
  sheetLabel: string;
  emptyText: string;
};

export default function SpreadsheetPreview({ data, sheetLabel, emptyText }: Props) {
  return (
    <div className="mt-4 min-h-0 flex-1 overflow-auto rounded-2xl border border-slate-200 bg-white p-4 dark:border-neutral-800 dark:bg-neutral-950/50">
      {data.rows.length > 0 ? (
        <div className="min-w-max">
          <div className="mb-2 text-xs font-semibold text-slate-500 dark:text-neutral-400">
            {sheetLabel}: {data.sheetName || "-"}
          </div>
          <table className="w-full border-collapse text-xs">
            <tbody>
              {data.rows.map((row, rowIdx) => (
                <tr key={rowIdx}>
                  {row.map((cell, colIdx) => (
                    <td
                      key={colIdx}
                      className="border border-slate-200 px-2 py-1 text-slate-700 dark:border-neutral-800 dark:text-neutral-200"
                    >
                      {cell}
                    </td>
                  ))}
                </tr>
              ))}
            </tbody>
          </table>
        </div>
      ) : (
        <div className="grid h-full w-full place-items-center text-sm text-slate-500 dark:text-neutral-400">
          {emptyText}
        </div>
      )}
    </div>
  );
}
