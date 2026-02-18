type Props = {
  content: string;
  emptyText: string;
};

export default function HtmlPreview({ content, emptyText }: Props) {
  const hasContent = content.trim().length > 0;

  return (
    <div className="mt-4 min-h-0 flex-1 overflow-hidden rounded-2xl border border-slate-200 bg-white p-3 dark:border-neutral-800 dark:bg-neutral-950/50">
      {hasContent ? (
        <iframe
          title="HTML Preview"
          sandbox=""
          srcDoc={content}
          className="h-full w-full rounded-xl border border-slate-200 bg-white dark:border-neutral-800 dark:bg-neutral-900"
        />
      ) : (
        <div className="grid h-full w-full place-items-center text-sm text-slate-500 dark:text-neutral-400">
          {emptyText}
        </div>
      )}
    </div>
  );
}
