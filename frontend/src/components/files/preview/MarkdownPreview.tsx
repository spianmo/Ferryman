import ReactMarkdown from "react-markdown";

type Props = {
  content: string;
};

export default function MarkdownPreview({ content }: Props) {
  return (
    <div className="mt-4 min-h-0 flex-1 overflow-auto rounded-2xl border border-slate-200 bg-white p-5 dark:border-neutral-800 dark:bg-neutral-950/50">
      <article className="whitespace-pre-wrap text-sm leading-relaxed text-slate-800 dark:text-neutral-100">
        <ReactMarkdown>{content}</ReactMarkdown>
      </article>
    </div>
  );
}
