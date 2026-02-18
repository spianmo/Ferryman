type Props = {
  url: string;
  loadingText: string;
};

export default function AudioPreview({ url, loadingText }: Props) {
  return (
    <div className="mt-4 min-h-0 flex-1 overflow-hidden rounded-2xl border border-slate-200 bg-white p-5 dark:border-neutral-800 dark:bg-neutral-950/50">
      {url ? (
        <div className="grid h-full w-full place-items-center">
          <audio src={url} controls className="w-full max-w-xl" />
        </div>
      ) : (
        <div className="grid h-full w-full place-items-center text-sm text-slate-500 dark:text-neutral-400">
          {loadingText}
        </div>
      )}
    </div>
  );
}
