import { useEffect, useState } from "react";
import { toast } from "../toast";
import { FiPlayCircle, FiRefreshCw } from "react-icons/fi";

import { getTask, listTasks, startTask } from "../api/client";
import { useI18n } from "../i18n";
import { decodeBase64Utf8 } from "../util/codec";
import { cn } from "../util/cn";
import type { TaskInfo } from "../types";

type Props = {
  token: string;
};

export default function TasksPage({ token }: Props) {
  const { t } = useI18n();
  const [command, setCommand] = useState("uname -a");
  const [tasks, setTasks] = useState<TaskInfo[]>([]);
  const [selectedTask, setSelectedTask] = useState("");
  const [taskOutput, setTaskOutput] = useState("");

  const refreshTasks = async (notify = false) => {
    const res = await listTasks(token);
    if (!res.ok) {
      if (notify) {
        toast.error(res.error ?? t("toast.request_failed"));
      }
      return;
    }
    setTasks((res.tasks ?? []) as unknown as TaskInfo[]);
  };

  const refreshTaskDetail = async (taskId: string, notify = false) => {
    const res = await getTask(token, taskId);
    if (!res.ok) {
      if (notify) {
        toast.error(res.error ?? t("toast.request_failed"));
      }
      return;
    }
    setTaskOutput(decodeBase64Utf8(res.output_base64));
  };

  useEffect(() => {
    void refreshTasks();
    const timer = window.setInterval(() => {
      void refreshTasks();
      if (selectedTask) {
        void refreshTaskDetail(selectedTask);
      }
    }, 2000);
    return () => {
      window.clearInterval(timer);
    };
  }, [selectedTask, token]);

  const runTask = async () => {
    const text = command.trim();
    if (!text) return;

    const res = await startTask(token, text);
    if (!res.ok) {
      toast.error(res.error ?? t("toast.request_failed"));
      return;
    }

    setSelectedTask(res.task_id);
    toast.success(t("toast.task_started"));
    await refreshTasks();
  };

  return (
    <div className="grid grid-cols-1 gap-4 xl:grid-cols-2">
      <section className="rounded-3xl bg-white/70 p-4 shadow-soft ring-1 ring-slate-200/70 backdrop-blur dark:bg-slate-900/55 dark:ring-slate-800/70">
        <div className="flex items-start justify-between gap-3">
          <div>
            <h2 className="text-base font-semibold tracking-tight text-slate-900 dark:text-slate-50">
              {t("tasks.title")}
            </h2>
            <div className="mt-1 text-xs text-slate-500 dark:text-slate-400">
              {tasks.length}
            </div>
          </div>
          <button
            className="inline-flex h-10 items-center gap-2 rounded-2xl bg-slate-100 px-4 text-sm font-semibold text-slate-700 hover:bg-slate-200 dark:bg-slate-800 dark:text-slate-100 dark:hover:bg-slate-700"
            onClick={() => void refreshTasks(true)}
          >
            <FiRefreshCw /> {t("common.refresh")}
          </button>
        </div>

        <div className="mt-4 flex flex-col gap-2 sm:flex-row">
          <input
            value={command}
            onChange={(event) => setCommand(event.target.value)}
            placeholder={t("tasks.command_placeholder")}
            className="w-full rounded-2xl border border-slate-200 bg-white px-3 py-2 text-sm text-slate-900 shadow-sm outline-none placeholder:text-slate-400 focus:border-slate-300 dark:border-slate-800 dark:bg-slate-950/40 dark:text-slate-50 dark:placeholder:text-slate-500 dark:focus:border-slate-700"
          />
          <button
            className="inline-flex h-10 shrink-0 items-center justify-center gap-2 rounded-2xl bg-slate-900 px-4 text-sm font-semibold text-white shadow-sm hover:bg-slate-800 dark:bg-slate-50 dark:text-slate-900 dark:hover:bg-white"
            onClick={() => void runTask()}
          >
            <FiPlayCircle /> {t("tasks.run")}
          </button>
        </div>

        <div className="mt-4 max-h-[560px] space-y-2 overflow-auto pr-1">
          {tasks.map((task) => (
            <button
              key={task.task_id}
              className={cn(
                "w-full rounded-2xl border border-transparent bg-white/70 p-3 text-left shadow-sm ring-1 ring-slate-200/60 hover:bg-slate-50 dark:bg-slate-950/30 dark:ring-slate-800/70 dark:hover:bg-slate-900/60",
                selectedTask === task.task_id &&
                  "ring-2 ring-slate-900/70 dark:ring-slate-50/70"
              )}
              onClick={() => {
                setSelectedTask(task.task_id);
                void refreshTaskDetail(task.task_id, true);
              }}
            >
              <div className="truncate text-sm font-semibold text-slate-900 dark:text-slate-50">
                {task.command}
              </div>
              <div className="mt-1 flex items-center justify-between gap-3 text-xs text-slate-500 dark:text-slate-400">
                <span className="truncate">{task.status}</span>
                <span className="shrink-0 font-mono">code {task.exit_code}</span>
              </div>
            </button>
          ))}
          {tasks.length === 0 ? (
            <div className="rounded-2xl border border-dashed border-slate-200 p-5 text-center text-sm text-slate-500 dark:border-slate-800 dark:text-slate-400">
              {t("tasks.empty")}
            </div>
          ) : null}
        </div>
      </section>

      <section className="rounded-3xl bg-white/70 p-4 shadow-soft ring-1 ring-slate-200/70 backdrop-blur dark:bg-slate-900/55 dark:ring-slate-800/70">
        <div className="flex items-start justify-between gap-3">
          <div>
            <h2 className="text-base font-semibold tracking-tight text-slate-900 dark:text-slate-50">
              {t("tasks.output")}
            </h2>
            <div className="mt-1 font-mono text-xs text-slate-500 dark:text-slate-400">
              {selectedTask ? selectedTask : t("tasks.select_hint")}
            </div>
          </div>
        </div>

        <pre className="mt-4 max-h-[620px] min-h-[560px] overflow-auto rounded-2xl bg-slate-950 p-4 font-mono text-[12px] leading-relaxed text-slate-50 shadow-sm ring-1 ring-slate-900/10">
          {taskOutput || " "}
        </pre>
      </section>
    </div>
  );
}
