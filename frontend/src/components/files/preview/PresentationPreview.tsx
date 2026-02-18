type Props = {
  slides: string[];
  slideLabel: (index: number) => string;
  emptyText: string;
};

export default function PresentationPreview({ slides, slideLabel, emptyText }: Props) {
  return (
    <div className="mt-4 min-h-0 flex-1 overflow-auto rounded-2xl border border-slate-200 bg-white p-4 dark:border-neutral-800 dark:bg-neutral-950/50">
      {slides.length > 0 ? (
        <div className="space-y-3">
          {slides.map((slide, idx) => (
            <section
              key={idx}
              className="rounded-xl border border-slate-200 bg-slate-50 p-3 dark:border-neutral-800 dark:bg-neutral-900/50"
            >
              <div className="mb-2 text-xs font-semibold text-slate-500 dark:text-neutral-400">
                {slideLabel(idx + 1)}
              </div>
              <pre className="whitespace-pre-wrap break-words font-mono text-xs text-slate-700 dark:text-neutral-200">
                {slide}
              </pre>
            </section>
          ))}
        </div>
      ) : (
        <div className="grid h-full w-full place-items-center text-sm text-slate-500 dark:text-neutral-400">
          {emptyText}
        </div>
      )}
    </div>
  );
}
