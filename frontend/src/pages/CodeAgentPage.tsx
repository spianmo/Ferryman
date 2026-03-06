import CodeAgentPanelApp from "../codeagent/CodeAgentPanelApp";
import type { SessionInfo } from "../types";

type Props = {
  session: SessionInfo;
};

export default function CodeAgentPage({ session }: Props) {
  return (
    <section className="h-full min-h-0 overflow-hidden rounded-3xl bg-white/70 shadow-soft ring-1 ring-slate-200/70 backdrop-blur dark:bg-neutral-900/55 dark:ring-neutral-800/70">
      <CodeAgentPanelApp key={session.token} />
    </section>
  );
}
