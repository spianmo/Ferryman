import { useCallback, useEffect, useMemo, useState } from "react";
import { FiActivity, FiPlus, FiRefreshCw, FiSave, FiTrash2 } from "react-icons/fi";

import {
  deleteTunnelMapping,
  getTunnelState,
  listListeningPorts,
  testTunnelMapping,
  updateTunnelConfig,
  upsertTunnelMapping,
} from "../api/client";
import { useI18n } from "../i18n";
import { toast } from "../toast";
import type { ListeningPortInfo, TunnelMappingState } from "../types";
import { cn } from "../util/cn";

type Props = {
  token: string;
};

function statusDotClass(status: string, active: boolean) {
  if (active) return "bg-emerald-500";
  const normalized = status.trim().toLowerCase();
  if (normalized === "syncing" || normalized === "pending") return "bg-amber-500";
  if (normalized === "disabled") return "bg-slate-500";
  return "bg-rose-500";
}

function sortedPorts(items: ListeningPortInfo[]) {
  return [...items].sort((a, b) => {
    if (a.port !== b.port) return a.port - b.port;
    if (a.protocol !== b.protocol) return a.protocol.localeCompare(b.protocol);
    return a.address.localeCompare(b.address);
  });
}

export default function TunnelPage({ token }: Props) {
  const { t } = useI18n();
  const [loading, setLoading] = useState(true);
  const [savingConfig, setSavingConfig] = useState(false);
  const [busy, setBusy] = useState(false);
  const [testingId, setTestingId] = useState("");
  const [proxyHost, setProxyHost] = useState("");
  const [proxyPort, setProxyPort] = useState("17000");
  const [mappings, setMappings] = useState<TunnelMappingState[]>([]);
  const [ports, setPorts] = useState<ListeningPortInfo[]>([]);

  const [name, setName] = useState("");
  const [protocol, setProtocol] = useState<"tcp" | "udp">("tcp");
  const [localHost, setLocalHost] = useState("127.0.0.1");
  const [localPort, setLocalPort] = useState("8080");
  const [remotePort, setRemotePort] = useState("18080");
  const [enabled, setEnabled] = useState(true);

  const loadData = useCallback(async () => {
    setLoading(true);
    try {
      const [stateRes, portsRes] = await Promise.all([
        getTunnelState(token),
        listListeningPorts(token),
      ]);
      if (!stateRes.ok) {
        toast.error(stateRes.error ?? t("toast.request_failed"));
      } else {
        setProxyHost(stateRes.proxy_host ?? "");
        setProxyPort(String(stateRes.proxy_port ?? 17000));
        setMappings(Array.isArray(stateRes.mappings) ? stateRes.mappings : []);
      }
      if (!portsRes.ok) {
        toast.error(portsRes.error ?? t("toast.request_failed"));
      } else {
        setPorts(Array.isArray(portsRes.items) ? portsRes.items : []);
      }
    } finally {
      setLoading(false);
    }
  }, [t, token]);

  useEffect(() => {
    void loadData();
  }, [loadData]);

  const runTest = useCallback(
    async (mappingId: string) => {
      if (!mappingId) return;
      setTestingId(mappingId);
      try {
        const res = await testTunnelMapping(token, mappingId);
        if (!res.ok) {
          toast.error(res.error ?? t("toast.request_failed"));
          return;
        }
        if (res.test_ok) {
          toast.success(t("tunnel.test_ok"));
        } else {
          toast.error((res.detail && `${t("tunnel.test_failed")}: ${res.detail}`) || t("tunnel.test_failed"));
        }
      } finally {
        setTestingId("");
        await loadData();
      }
    },
    [loadData, t, token]
  );

  const sortedListeningPorts = useMemo(() => sortedPorts(ports), [ports]);

  const onSaveProxy = async () => {
    const parsedPort = Number(proxyPort);
    if (!Number.isFinite(parsedPort) || parsedPort <= 0 || parsedPort > 65535) {
      toast.error(t("tunnel.invalid_port"));
      return;
    }
    setSavingConfig(true);
    try {
      const res = await updateTunnelConfig(token, proxyHost.trim(), parsedPort);
      if (!res.ok) {
        toast.error(res.error ?? t("toast.request_failed"));
        return;
      }
      toast.success(t("toast.saved"));
      await loadData();
    } finally {
      setSavingConfig(false);
    }
  };

  const onUpsertMapping = async () => {
    const parsedLocalPort = Number(localPort);
    const parsedRemotePort = Number(remotePort);
    if (!Number.isFinite(parsedLocalPort) || parsedLocalPort <= 0 || parsedLocalPort > 65535) {
      toast.error(t("tunnel.invalid_local_port"));
      return;
    }
    if (!Number.isFinite(parsedRemotePort) || parsedRemotePort <= 0 || parsedRemotePort > 65535) {
      toast.error(t("tunnel.invalid_remote_port"));
      return;
    }
    if (!localHost.trim()) {
      toast.error(t("tunnel.invalid_local_host"));
      return;
    }

    setBusy(true);
    try {
      const res = await upsertTunnelMapping(token, {
        name: name.trim(),
        protocol,
        local_host: localHost.trim(),
        local_port: parsedLocalPort,
        remote_port: parsedRemotePort,
        enabled,
      });
      if (!res.ok) {
        toast.error(res.error ?? t("toast.request_failed"));
        return;
      }

      toast.success(t("tunnel.mapping_saved"));
      const createdId = res.mapping?.id ?? "";
      setName("");
      await loadData();
      if (createdId) {
        await runTest(createdId);
      }
    } finally {
      setBusy(false);
    }
  };

  const onDeleteMapping = async (mappingId: string) => {
    setBusy(true);
    try {
      const res = await deleteTunnelMapping(token, mappingId);
      if (!res.ok) {
        toast.error(res.error ?? t("toast.request_failed"));
        return;
      }
      toast.success(t("tunnel.mapping_deleted"));
      await loadData();
    } finally {
      setBusy(false);
    }
  };

  return (
    <div className="flex h-full min-h-0 flex-col gap-4">
      <div className="grid min-h-0 grid-cols-1 gap-4 xl:grid-cols-[1.25fr_1fr]">
        <section className="min-h-0 rounded-3xl border border-slate-200/80 bg-white/80 p-4 shadow-soft dark:border-neutral-800/80 dark:bg-neutral-900/60">
          <div className="mb-3 flex items-center justify-between">
            <h2 className="text-lg font-semibold">{t("tunnel.title")}</h2>
            <button
              className="inline-flex items-center gap-2 rounded-xl border border-slate-200 px-3 py-1.5 text-sm font-semibold hover:bg-slate-50 dark:border-neutral-700 dark:hover:bg-neutral-800"
              onClick={() => void loadData()}
              disabled={loading}
            >
              <FiRefreshCw className={cn(loading && "animate-spin")} />
              {t("common.refresh")}
            </button>
          </div>

          <div className="rounded-2xl border border-slate-200/80 bg-white/70 p-3 dark:border-neutral-800/80 dark:bg-neutral-950/40">
            <div className="mb-2 text-sm font-semibold">{t("tunnel.proxy_settings")}</div>
            <div className="grid grid-cols-1 gap-2 md:grid-cols-[1fr_160px_auto]">
              <input
                className="rounded-xl border border-slate-200 bg-white px-3 py-2 text-sm dark:border-neutral-700 dark:bg-neutral-900"
                placeholder={t("tunnel.proxy_host")}
                value={proxyHost}
                onChange={(event) => setProxyHost(event.target.value)}
              />
              <input
                className="rounded-xl border border-slate-200 bg-white px-3 py-2 text-sm dark:border-neutral-700 dark:bg-neutral-900"
                placeholder={t("tunnel.proxy_port")}
                value={proxyPort}
                onChange={(event) => setProxyPort(event.target.value)}
              />
              <button
                className="inline-flex items-center justify-center gap-2 rounded-xl bg-slate-900 px-3 py-2 text-sm font-semibold text-white hover:bg-slate-800 disabled:cursor-not-allowed disabled:opacity-60 dark:bg-neutral-100 dark:text-neutral-900 dark:hover:bg-neutral-200"
                onClick={() => void onSaveProxy()}
                disabled={savingConfig}
              >
                <FiSave />
                {t("common.save")}
              </button>
            </div>
          </div>

          <div className="mt-3 rounded-2xl border border-slate-200/80 bg-white/70 p-3 dark:border-neutral-800/80 dark:bg-neutral-950/40">
            <div className="mb-2 text-sm font-semibold">{t("tunnel.add_mapping")}</div>
            <div className="grid grid-cols-1 gap-2 md:grid-cols-2">
              <input
                className="rounded-xl border border-slate-200 bg-white px-3 py-2 text-sm dark:border-neutral-700 dark:bg-neutral-900"
                placeholder={t("tunnel.name")}
                value={name}
                onChange={(event) => setName(event.target.value)}
              />
              <select
                className="rounded-xl border border-slate-200 bg-white px-3 py-2 text-sm dark:border-neutral-700 dark:bg-neutral-900"
                value={protocol}
                onChange={(event) => setProtocol(event.target.value === "udp" ? "udp" : "tcp")}
              >
                <option value="tcp">TCP</option>
                <option value="udp">UDP</option>
              </select>
              <input
                className="rounded-xl border border-slate-200 bg-white px-3 py-2 text-sm dark:border-neutral-700 dark:bg-neutral-900"
                placeholder={t("tunnel.local_host")}
                value={localHost}
                onChange={(event) => setLocalHost(event.target.value)}
              />
              <input
                className="rounded-xl border border-slate-200 bg-white px-3 py-2 text-sm dark:border-neutral-700 dark:bg-neutral-900"
                placeholder={t("tunnel.local_port")}
                value={localPort}
                onChange={(event) => setLocalPort(event.target.value)}
              />
              <input
                className="rounded-xl border border-slate-200 bg-white px-3 py-2 text-sm dark:border-neutral-700 dark:bg-neutral-900"
                placeholder={t("tunnel.remote_port")}
                value={remotePort}
                onChange={(event) => setRemotePort(event.target.value)}
              />
              <label className="inline-flex items-center gap-2 rounded-xl border border-slate-200 bg-white px-3 py-2 text-sm dark:border-neutral-700 dark:bg-neutral-900">
                <input
                  type="checkbox"
                  checked={enabled}
                  onChange={(event) => setEnabled(event.target.checked)}
                />
                {t("tunnel.enabled")}
              </label>
            </div>
            <div className="mt-2 flex justify-end">
              <button
                className="inline-flex items-center gap-2 rounded-xl bg-slate-900 px-3 py-2 text-sm font-semibold text-white hover:bg-slate-800 disabled:cursor-not-allowed disabled:opacity-60 dark:bg-neutral-100 dark:text-neutral-900 dark:hover:bg-neutral-200"
                onClick={() => void onUpsertMapping()}
                disabled={busy}
              >
                <FiPlus />
                {t("tunnel.add_mapping")}
              </button>
            </div>
          </div>

          <div className="mt-3 min-h-0 overflow-auto rounded-2xl border border-slate-200/80 dark:border-neutral-800/80">
            <table className="min-w-full text-sm">
              <thead className="sticky top-0 z-10 bg-slate-100/80 dark:bg-neutral-900/85">
                <tr className="text-left text-slate-600 dark:text-neutral-300">
                  <th className="px-3 py-2">{t("tunnel.name")}</th>
                  <th className="px-3 py-2">{t("tunnel.protocol")}</th>
                  <th className="px-3 py-2">{t("tunnel.mapping")}</th>
                  <th className="px-3 py-2">{t("tunnel.status")}</th>
                  <th className="px-3 py-2">{t("tunnel.actions")}</th>
                </tr>
              </thead>
              <tbody>
                {mappings.length === 0 ? (
                  <tr>
                    <td className="px-3 py-3 text-slate-500 dark:text-neutral-400" colSpan={5}>
                      {t("tunnel.empty")}
                    </td>
                  </tr>
                ) : (
                  mappings.map((item) => (
                    <tr
                      key={item.id}
                      className="border-t border-slate-200/80 align-top dark:border-neutral-800/80"
                    >
                      <td className="px-3 py-2">
                        <div className="font-semibold">{item.name || item.id}</div>
                        <div className="text-xs text-slate-500 dark:text-neutral-400">{item.id}</div>
                      </td>
                      <td className="px-3 py-2 uppercase">{item.protocol}</td>
                      <td className="px-3 py-2 font-mono text-xs">
                        {item.local_host}:{item.local_port} {"->"} *:{item.remote_port}
                      </td>
                      <td className="px-3 py-2">
                        <div className="inline-flex items-center gap-2">
                          <span className={cn("h-2.5 w-2.5 rounded-full", statusDotClass(item.status, item.active))} />
                          <span>{item.status || "--"}</span>
                        </div>
                        <div className="mt-1 text-xs text-slate-500 dark:text-neutral-400">{item.detail}</div>
                      </td>
                      <td className="px-3 py-2">
                        <div className="flex items-center gap-2">
                          <button
                            className="inline-flex items-center gap-1 rounded-lg border border-slate-200 px-2 py-1 text-xs font-semibold hover:bg-slate-50 dark:border-neutral-700 dark:hover:bg-neutral-800"
                            onClick={() => void runTest(item.id)}
                            disabled={testingId === item.id}
                          >
                            <FiActivity className={cn(testingId === item.id && "animate-pulse")} />
                            {t("tunnel.test")}
                          </button>
                          <button
                            className="inline-flex items-center gap-1 rounded-lg border border-rose-200 px-2 py-1 text-xs font-semibold text-rose-600 hover:bg-rose-50 dark:border-rose-900/60 dark:text-rose-300 dark:hover:bg-rose-950/40"
                            onClick={() => void onDeleteMapping(item.id)}
                            disabled={busy}
                          >
                            <FiTrash2 />
                            {t("tunnel.delete")}
                          </button>
                        </div>
                      </td>
                    </tr>
                  ))
                )}
              </tbody>
            </table>
          </div>
        </section>

        <section className="min-h-0 rounded-3xl border border-slate-200/80 bg-white/80 p-4 shadow-soft dark:border-neutral-800/80 dark:bg-neutral-900/60">
          <h2 className="mb-3 text-lg font-semibold">{t("tunnel.local_ports")}</h2>
          <div className="min-h-0 overflow-auto rounded-2xl border border-slate-200/80 dark:border-neutral-800/80">
            <table className="min-w-full text-sm">
              <thead className="sticky top-0 z-10 bg-slate-100/80 dark:bg-neutral-900/85">
                <tr className="text-left text-slate-600 dark:text-neutral-300">
                  <th className="px-3 py-2">{t("tunnel.protocol")}</th>
                  <th className="px-3 py-2">{t("tunnel.address")}</th>
                  <th className="px-3 py-2">{t("tunnel.port")}</th>
                  <th className="px-3 py-2">{t("tunnel.process")}</th>
                  <th className="px-3 py-2">{t("tunnel.pid")}</th>
                </tr>
              </thead>
              <tbody>
                {sortedListeningPorts.length === 0 ? (
                  <tr>
                    <td className="px-3 py-3 text-slate-500 dark:text-neutral-400" colSpan={5}>
                      {loading ? t("monitor.loading") : t("tunnel.ports_empty")}
                    </td>
                  </tr>
                ) : (
                  sortedListeningPorts.map((item, index) => (
                    <tr key={`${item.protocol}-${item.address}-${item.port}-${item.pid}-${index}`} className="border-t border-slate-200/80 dark:border-neutral-800/80">
                      <td className="px-3 py-2 uppercase">{item.protocol}</td>
                      <td className="px-3 py-2 font-mono text-xs">{item.address}</td>
                      <td className="px-3 py-2 font-mono">{item.port}</td>
                      <td className="px-3 py-2">{item.process || "--"}</td>
                      <td className="px-3 py-2 font-mono">{item.pid > 0 ? item.pid : "--"}</td>
                    </tr>
                  ))
                )}
              </tbody>
            </table>
          </div>
        </section>
      </div>
    </div>
  );
}
