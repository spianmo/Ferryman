type Props = {
  html: string;
  loadingText: string;
};

export default function WordPreview({ html, loadingText }: Props) {
  return (
    <div className="mt-4 min-h-0 flex-1 overflow-auto rounded-2xl border border-slate-200 bg-white p-5 dark:border-neutral-800 dark:bg-neutral-950/50">
      {html ? (
        <article
          className="text-sm leading-relaxed text-slate-800 dark:text-neutral-100"
          dangerouslySetInnerHTML={{ __html: html }}
        />
      ) : (
        <div className="grid h-full w-full place-items-center text-sm text-slate-500 dark:text-neutral-400">
          {loadingText}
        </div>
      )}
    </div>
  );
}
