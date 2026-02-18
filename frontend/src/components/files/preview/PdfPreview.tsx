import { useMemo, useState } from "react";
import { Document, Page, pdfjs } from "react-pdf";
import pdfWorker from "pdfjs-dist/build/pdf.worker.min.mjs?url";
import "react-pdf/dist/Page/AnnotationLayer.css";
import "react-pdf/dist/Page/TextLayer.css";

pdfjs.GlobalWorkerOptions.workerSrc = pdfWorker;

type Props = {
  bytes: Uint8Array | null;
  loadingText: string;
  onParseFailed: () => void;
};

export default function PdfPreview({ bytes, loadingText, onParseFailed }: Props) {
  const [pageCount, setPageCount] = useState(0);
  const documentFile = useMemo(() => {
    if (!bytes) return null;
    // Copy once per file load to avoid detached ArrayBuffer errors on worker postMessage.
    return { data: bytes.slice() };
  }, [bytes]);

  return (
    <div className="mt-4 min-h-0 flex-1 overflow-auto rounded-2xl border border-slate-200 bg-white p-3 dark:border-neutral-800 dark:bg-neutral-950/50">
      {documentFile ? (
        <Document
          file={documentFile}
          loading={
            <div className="grid h-full w-full place-items-center text-sm text-slate-500 dark:text-neutral-400">
              {loadingText}
            </div>
          }
          onLoadSuccess={({ numPages }) => setPageCount(numPages)}
          onLoadError={onParseFailed}
        >
          <div className="space-y-3">
            {Array.from({ length: pageCount }, (_, idx) => (
              <div key={idx} className="flex justify-center">
                <Page
                  pageNumber={idx + 1}
                  width={860}
                  renderTextLayer={false}
                  renderAnnotationLayer={false}
                />
              </div>
            ))}
          </div>
        </Document>
      ) : (
        <div className="grid h-full w-full place-items-center text-sm text-slate-500 dark:text-neutral-400">
          {loadingText}
        </div>
      )}
    </div>
  );
}
