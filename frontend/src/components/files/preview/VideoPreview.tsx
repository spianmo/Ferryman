type Props = {
  url: string;
  loadingText: string;
};

export default function VideoPreview({ url, loadingText }: Props) {
  return (
    <div className="mt-4 min-h-0 flex-1 overflow-hidden rounded-2xl border border-slate-200 bg-black dark:border-neutral-800">
      {url ? (
        <video src={url} controls className="h-full w-full object-contain" />
      ) : (
        <div className="grid h-full w-full place-items-center text-sm text-neutral-300">
          {loadingText}
        </div>
      )}
    </div>
  );
}
