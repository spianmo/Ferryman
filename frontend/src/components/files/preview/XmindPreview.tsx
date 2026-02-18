import { useEffect, useState } from "react";
import JSZip from "jszip";

type Props = {
  bytes: Uint8Array | null;
  loadingText: string;
  emptyText: string;
  onParseFailed: () => void;
};

type XmindTopicNode = {
  title: string;
  children: XmindTopicNode[];
};

type XmindSheetNode = {
  id: string;
  title: string;
  rootTopic: XmindTopicNode | null;
};

const MAX_TOPIC_COUNT = 4000;

function normalizeText(value: unknown) {
  if (typeof value !== "string") return "";
  return value.trim();
}

function parseTopicNode(rawTopic: unknown, budget: { count: number }): XmindTopicNode | null {
  if (!rawTopic || typeof rawTopic !== "object") return null;
  if (budget.count >= MAX_TOPIC_COUNT) return null;

  budget.count += 1;
  const topic = rawTopic as Record<string, unknown>;
  const title = normalizeText(topic.title) || "Untitled";
  const parsedChildren: XmindTopicNode[] = [];
  const childrenRaw = topic.children;

  if (childrenRaw && typeof childrenRaw === "object") {
    const childGroups = Object.values(childrenRaw as Record<string, unknown>);
    for (const group of childGroups) {
      if (!Array.isArray(group)) continue;
      for (const item of group) {
        const child = parseTopicNode(item, budget);
        if (child) {
          parsedChildren.push(child);
        }
      }
    }
  }

  return { title, children: parsedChildren };
}

function parseSheets(raw: unknown): XmindSheetNode[] {
  let sheetsRaw: unknown[] = [];
  if (Array.isArray(raw)) {
    sheetsRaw = raw;
  } else if (raw && typeof raw === "object") {
    const sheetField = (raw as Record<string, unknown>).sheets;
    if (Array.isArray(sheetField)) {
      sheetsRaw = sheetField;
    }
  }

  const budget = { count: 0 };
  return sheetsRaw
    .map((sheetRaw, index) => {
      if (!sheetRaw || typeof sheetRaw !== "object") return null;
      const sheet = sheetRaw as Record<string, unknown>;
      const id = normalizeText(sheet.id) || `sheet-${index + 1}`;
      const title = normalizeText(sheet.title) || `Sheet ${index + 1}`;
      const rootTopic = parseTopicNode(sheet.rootTopic, budget);
      return { id, title, rootTopic };
    })
    .filter((sheet): sheet is XmindSheetNode => Boolean(sheet));
}

function TopicNode({ topic, depth }: { topic: XmindTopicNode; depth: number }) {
  return (
    <li className={depth === 0 ? "" : "mt-2"}>
      <div className="rounded-lg border border-slate-200 bg-white px-3 py-2 text-sm text-slate-800 dark:border-neutral-700 dark:bg-neutral-900/70 dark:text-neutral-100">
        {topic.title}
      </div>
      {topic.children.length > 0 ? (
        <ul className="ml-4 mt-2 space-y-2 border-l border-slate-200 pl-3 dark:border-neutral-700">
          {topic.children.map((child, idx) => (
            <TopicNode key={`${depth}-${idx}-${child.title}`} topic={child} depth={depth + 1} />
          ))}
        </ul>
      ) : null}
    </li>
  );
}

export default function XmindPreview({ bytes, loadingText, emptyText, onParseFailed }: Props) {
  const [loading, setLoading] = useState(false);
  const [sheets, setSheets] = useState<XmindSheetNode[]>([]);

  useEffect(() => {
    if (!bytes) return;

    let cancelled = false;
    setLoading(true);
    setSheets([]);

    void (async () => {
      try {
        const zip = await JSZip.loadAsync(bytes);
        const contentFile = zip.file("content.json");
        if (!contentFile) {
          throw new Error("content.json not found");
        }
        const rawText = await contentFile.async("text");
        const parsed = JSON.parse(rawText) as unknown;
        const nextSheets = parseSheets(parsed);
        if (!cancelled) {
          setSheets(nextSheets);
        }
      } catch {
        if (!cancelled) {
          onParseFailed();
        }
      } finally {
        if (!cancelled) {
          setLoading(false);
        }
      }
    })();

    return () => {
      cancelled = true;
    };
  }, [bytes, onParseFailed]);

  if (!bytes || loading) {
    return (
      <div className="mt-4 grid min-h-0 flex-1 place-items-center rounded-2xl border border-slate-200 bg-white p-4 text-sm text-slate-500 dark:border-neutral-800 dark:bg-neutral-950/50 dark:text-neutral-400">
        {loadingText}
      </div>
    );
  }

  if (sheets.length === 0) {
    return (
      <div className="mt-4 grid min-h-0 flex-1 place-items-center rounded-2xl border border-slate-200 bg-white p-4 text-sm text-slate-500 dark:border-neutral-800 dark:bg-neutral-950/50 dark:text-neutral-400">
        {emptyText}
      </div>
    );
  }

  return (
    <div className="mt-4 min-h-0 flex-1 overflow-auto rounded-2xl border border-slate-200 bg-white p-4 dark:border-neutral-800 dark:bg-neutral-950/50">
      <div className="space-y-4">
        {sheets.map((sheet, sheetIndex) => (
          <section
            key={`${sheet.id}-${sheetIndex}`}
            className="rounded-xl border border-slate-200 bg-slate-50 p-4 dark:border-neutral-800 dark:bg-neutral-900/50"
          >
            <h3 className="mb-3 text-sm font-semibold text-slate-700 dark:text-neutral-200">{sheet.title}</h3>
            {sheet.rootTopic ? (
              <ul>
                <TopicNode topic={sheet.rootTopic} depth={0} />
              </ul>
            ) : (
              <div className="text-sm text-slate-500 dark:text-neutral-400">{emptyText}</div>
            )}
          </section>
        ))}
      </div>
    </div>
  );
}
