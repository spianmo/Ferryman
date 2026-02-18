type Props = {
  url: string;
  alt: string;
  loadingText: string;
};

export default function ImagePreview({ url, alt, loadingText }: Props) {
  return (
    <div className="mt-4 min-h-0 flex-1 overflow-auto rounded-2xl border border-slate-200 bg-white p-3 dark:border-neutral-800 dark:bg-neutral-950/50">
      {url ? (
        <div className="grid h-full w-full place-items-center">
          <img src={url} alt={alt} className="max-h-full max-w-full object-contain" />
        </div>
      ) : (
        <div className="grid h-full w-full place-items-center text-sm text-slate-500 dark:text-neutral-400">
          {loadingText}
        </div>
      )}
    </div>
  );
}
