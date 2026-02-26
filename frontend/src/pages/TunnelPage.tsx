import { useCallback, useEffect, useMemo, useRef, useState } from "react";
import { FiActivity, FiPlus, FiRefreshCw, FiSave, FiTrash2, FiX } from "react-icons/fi";

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
import type { ListeningPortInfo, SessionInfo, TunnelMappingState } from "../types";
import { cn } from "../util/cn";
import { getTunnelSocket } from "../ws/tunnelSocket";

type Props = {
  session: SessionInfo;
};

function normalizeStatusText(value: string) {
  return value.trim().toLowerCase();
}

function isPendingStatus(status: string, detail = "") {
  const normalizedStatus = normalizeStatusText(status);
  const normalizedDetail = normalizeStatusText(detail);
  if (!normalizedStatus && !normalizedDetail) {
    return false;
  }
  if (normalizedStatus === "pending" || normalizedStatus === "syncing" || normalizedStatus === "testing") {
    return true;
  }
  if (
    normalizedStatus.includes("waiting proxy apply") ||
    normalizedStatus.includes("waiting for proxy synchronization") ||
    normalizedStatus.includes("pending") ||
    normalizedStatus.includes("sync") ||
    normalizedStatus.includes("testing")
  ) {
    return true;
  }
  return (
    normalizedDetail.includes("waiting proxy apply") ||
    normalizedDetail.includes("waiting for proxy synchronization")
  );
}

function statusDotClass(status: string, detail: string, active: boolean) {
  if (active) return "bg-emerald-500";
  const normalized = normalizeStatusText(status);
  if (isPendingStatus(status, detail)) return "bg-amber-500";
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

export default function TunnelPage({ session }: Props) {
  const token = session.token;
  const { t } = useI18n();
  const tunnelSocket = useMemo(() => getTunnelSocket(), []);
  const [loading, setLoading] = useState(true);
  const [savingConfig, setSavingConfig] = useState(false);
  const [busy, setBusy] = useState(false);
  const [testingId, setTestingId] = useState("");
  const [deletingId, setDeletingId] = useState("");
  const [togglingId, setTogglingId] = useState("");
  const [proxyDialogOpen, setProxyDialogOpen] = useState(false);
  const [mappingDialogOpen, setMappingDialogOpen] = useState(false);
  const [proxyHost, setProxyHost] = useState("");
  const [proxyPort, setProxyPort] = useState("17000");
  const [proxyToken, setProxyToken] = useState("");
  const [mappings, setMappings] = useState<TunnelMappingState[]>([]);
  const [ports, setPorts] = useState<ListeningPortInfo[]>([]);

  const [name, setName] = useState("");
  const [protocol, setProtocol] = useState<"tcp" | "udp">("tcp");
  const [localHost, setLocalHost] = useState("127.0.0.1");
  const [localPort, setLocalPort] = useState("8080");
  const [remotePort, setRemotePort] = useState("18080");
  const [enabled, setEnabled] = useState(true);
  const lastLoadIdRef = useRef(0);

  const loadData = useCallback(async (opts?: { background?: boolean; quietError?: boolean; withPorts?: boolean }) => {
    const loadId = ++lastLoadIdRef.current;
    const withPorts = opts?.withPorts ?? true;
    if (!opts?.background) {
      setLoading(true);
    }
    try {
      const stateReq = getTunnelState(token);
      const portsReq = withPorts ? listListeningPorts(token) : null;
      const stateRes = await stateReq;
      const portsRes = portsReq ? await portsReq : null;
      if (loadId !== lastLoadIdRef.current) {
        return;
      }
      if (!stateRes.ok) {
        if (!opts?.quietError) {
          toast.error(stateRes.error ?? t("toast.request_failed"));
        }
      } else {
        setProxyHost(stateRes.proxy_host ?? "");
        setProxyPort(String(stateRes.proxy_port ?? 17000));
        setProxyToken(stateRes.proxy_token ?? "");
        setMappings(Array.isArray(stateRes.mappings) ? stateRes.mappings : []);
      }
      if (withPorts && portsRes != null) {
        if (!portsRes.ok) {
          if (!opts?.quietError) {
            toast.error(portsRes.error ?? t("toast.request_failed"));
          }
        } else {
          setPorts(Array.isArray(portsRes.items) ? portsRes.items : []);
        }
      }
    } finally {
      if (!opts?.background && loadId === lastLoadIdRef.current) {
        setLoading(false);
      }
    }
  }, [t, token]);

  useEffect(() => {
    void loadData();
  }, [loadData]);

  useEffect(() => {
    const unsubscribeMessages = tunnelSocket.subscribeMessages((payload) => {
      if (String(payload.event ?? "") !== "tunnel_snapshot") {
        return;
      }
      if (!Array.isArray(payload.mappings)) {
        return;
      }
      setMappings(payload.mappings as TunnelMappingState[]);
    });

    tunnelSocket.start(session);
    return () => {
      unsubscribeMessages();
      tunnelSocket.stop();
    };
  }, [session, tunnelSocket]);

  const runTest = useCallback(
    async (mappingId: string) => {
      if (!mappingId) return;
      setMappings((prev) =>
        prev.map((item) =>
          item.id === mappingId
            ? {
                ...item,
                active: false,
                status: "testing",
              }
            : item
        )
      );
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
        await loadData({ background: true, withPorts: false });
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
      const res = await updateTunnelConfig(token, proxyHost.trim(), parsedPort, proxyToken.trim());
      if (!res.ok) {
        toast.error(res.error ?? t("toast.request_failed"));
        return;
      }
      toast.success(t("toast.saved"));
      setProxyDialogOpen(false);
      await loadData({ background: true, withPorts: false });
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
      setMappingDialogOpen(false);
      await loadData({ background: true, withPorts: false });
      if (createdId) {
        await runTest(createdId);
      }
    } finally {
      setBusy(false);
    }
  };

  const onDeleteMapping = async (mappingId: string) => {
    if (!mappingId || deletingId === mappingId) {
      return;
    }
    const previousMappings = mappings;
    setDeletingId(mappingId);
    setMappings((prev) => prev.filter((item) => item.id !== mappingId));
    try {
      const res = await deleteTunnelMapping(token, mappingId);
      if (!res.ok) {
        setMappings(previousMappings);
        toast.error(res.error ?? t("toast.request_failed"));
        return;
      }
      toast.success(t("tunnel.mapping_deleted"));
      void loadData({ background: true, quietError: true, withPorts: false });
    } finally {
      setDeletingId("");
    }
  };

  const onToggleMappingEnabled = async (mapping: TunnelMappingState) => {
    if (!mapping.id || togglingId === mapping.id) {
      return;
    }
    const nextEnabled = !mapping.enabled;
    setTogglingId(mapping.id);
    setMappings((prev) =>
      prev.map((item) =>
        item.id === mapping.id
          ? {
              ...item,
              enabled: nextEnabled,
              active: false,
              status: nextEnabled ? "syncing" : "disabled",
              detail: nextEnabled ? "waiting proxy apply" : "mapping disabled",
            }
          : item
      )
    );
    try {
      const res = await upsertTunnelMapping(token, {
        id: mapping.id,
        name: mapping.name,
        protocol: mapping.protocol === "udp" ? "udp" : "tcp",
        local_host: mapping.local_host,
        local_port: mapping.local_port,
        remote_port: mapping.remote_port,
        enabled: nextEnabled,
      });
      if (!res.ok) {
        toast.error(res.error ?? t("toast.request_failed"));
        await loadData({ background: true, quietError: true, withPorts: false });
        return;
      }
      toast.success(nextEnabled ? t("tunnel.mapping_enabled") : t("tunnel.mapping_disabled"));
      await loadData({ background: true, quietError: true, withPorts: false });
    } finally {
      setTogglingId("");
    }
  };

  return (
    <div className="h-full min-h-0 overflow-visible">
      <div className="h-full min-h-0 px-1 pb-2 pt-1 xl:px-0 xl:pb-0 xl:pt-0">
        <div className="grid h-full min-h-0 grid-cols-1 gap-4 px-1 pb-1 xl:grid-cols-[1.25fr_1fr] xl:px-0 xl:pb-0">
          <section className="flex h-full min-h-[320px] min-w-0 flex-col overflow-hidden rounded-3xl border border-slate-200/80 bg-white/80 p-4 shadow-soft xl:min-h-0 dark:border-neutral-800/80 dark:bg-neutral-900/60">
            <div className="mb-3 flex flex-col gap-2 sm:flex-row sm:items-center sm:justify-between">
              <h2 className="text-lg font-semibold sm:shrink-0">{t("tunnel.title")}</h2>
              <div className="flex w-full flex-wrap items-center gap-2 sm:w-auto sm:justify-end">
                <button
                  className="inline-flex flex-1 items-center justify-center gap-2 rounded-xl border border-slate-200 px-3 py-1.5 text-sm font-semibold hover:bg-slate-50 sm:flex-none sm:justify-start dark:border-neutral-700 dark:hover:bg-neutral-800"
                  onClick={() => void loadData()}
                  disabled={loading}
                >
                  <FiRefreshCw className={cn(loading && "animate-spin")} />
                  {t("common.refresh")}
                </button>
                <button
                  className="inline-flex flex-1 items-center justify-center gap-2 rounded-xl border border-slate-200 px-3 py-1.5 text-sm font-semibold hover:bg-slate-50 sm:flex-none sm:justify-start dark:border-neutral-700 dark:hover:bg-neutral-800"
                  onClick={() => setProxyDialogOpen(true)}
                >
                  {t("tunnel.proxy_settings")}
                </button>
                <button
                  className="inline-flex w-full items-center justify-center gap-2 rounded-xl bg-slate-900 px-3 py-1.5 text-sm font-semibold text-white hover:bg-slate-800 sm:w-auto sm:justify-start dark:bg-neutral-100 dark:text-neutral-900 dark:hover:bg-neutral-200"
                  onClick={() => setMappingDialogOpen(true)}
                >
                  <FiPlus />
                  {t("tunnel.add_mapping")}
                </button>
              </div>
            </div>

            <div className="mt-3 min-h-0 flex-1 overflow-auto rounded-2xl border border-slate-200/80 dark:border-neutral-800/80">
              <table className="w-full text-sm sm:min-w-[760px] md:min-w-[860px]">
                <thead className="sticky top-0 z-10 bg-slate-100 dark:bg-neutral-900">
                  <tr className="text-left text-slate-600 dark:text-neutral-300">
                    <th className="w-[188px] min-w-[188px] whitespace-nowrap px-2.5 py-2">{t("tunnel.name")}</th>
                    <th className="hidden w-[72px] min-w-[72px] whitespace-nowrap px-2.5 py-2 sm:table-cell">{t("tunnel.protocol")}</th>
                    <th className="hidden min-w-[240px] whitespace-nowrap px-2.5 py-2 sm:table-cell">{t("tunnel.mapping")}</th>
                    <th className="hidden w-[200px] min-w-[200px] whitespace-nowrap px-2.5 py-2 sm:table-cell">{t("tunnel.status")}</th>
                    <th className="sticky right-0 z-20 w-[188px] min-w-[188px] whitespace-nowrap bg-slate-100 px-2.5 py-2 shadow-[-10px_0_18px_-14px_rgba(15,23,42,0.65)] sm:w-[196px] sm:min-w-[196px] dark:bg-neutral-900 dark:shadow-[-10px_0_18px_-14px_rgba(0,0,0,0.85)]">
                      {t("tunnel.actions")}
                    </th>
                  </tr>
                </thead>
                <tbody>
                  {mappings.length === 0 ? (
                    <tr>
                      <td className="px-2.5 py-3 text-slate-500 dark:text-neutral-400" colSpan={5}>
                        {t("tunnel.empty")}
                      </td>
                    </tr>
                  ) : (
                    mappings.map((item) => (
                      <tr
                        key={item.id}
                        className="border-t border-slate-200/80 align-top dark:border-neutral-800/80"
                      >
                        <td className="w-[188px] min-w-[188px] px-2.5 py-2">
                          <div className="truncate font-semibold" title={item.name || item.id}>
                            {item.name || item.id}
                          </div>
                          <div className="truncate text-xs text-slate-500 dark:text-neutral-400" title={item.id}>
                            {item.id}
                          </div>
                          <div className="mt-1 space-y-0.5 text-[11px] text-slate-500 dark:text-neutral-400 sm:hidden">
                            <div className="uppercase">{item.protocol}</div>
                            <div className="font-mono">
                              {item.local_host}:{item.local_port} {"->"} *:{item.remote_port}
                            </div>
                            <div className="truncate" title={`${item.status || "--"} · ${item.detail || ""}`}>
                              {item.status || "--"} {item.detail ? `· ${item.detail}` : ""}
                            </div>
                          </div>
                        </td>
                        <td className="hidden w-[72px] min-w-[72px] px-2.5 py-2 uppercase sm:table-cell">{item.protocol}</td>
                        <td className="hidden min-w-[240px] whitespace-nowrap px-2.5 py-2 font-mono text-xs sm:table-cell">
                          {item.local_host}:{item.local_port} {"->"} *:{item.remote_port}
                        </td>
                        <td className="hidden w-[200px] min-w-[200px] px-2.5 py-2 sm:table-cell">
                          <div className="inline-flex items-center gap-2">
                            <span
                              className={cn("h-2.5 w-2.5 rounded-full", statusDotClass(item.status, item.detail, item.active))}
                            />
                            <span>{item.status || "--"}</span>
                          </div>
                          <div className="mt-1 text-xs text-slate-500 dark:text-neutral-400">{item.detail}</div>
                        </td>
                        <td className="sticky right-0 z-10 w-[188px] min-w-[188px] bg-white/95 px-2.5 py-2 shadow-[-10px_0_18px_-14px_rgba(15,23,42,0.65)] sm:w-[196px] sm:min-w-[196px] dark:bg-neutral-900/95 dark:shadow-[-10px_0_18px_-14px_rgba(0,0,0,0.85)]">
                          <div className="flex flex-wrap items-center gap-1.5 sm:gap-2">
                            <button
                              className={cn(
                                "inline-flex items-center gap-1 rounded-lg border px-1.5 py-1 text-xs font-semibold sm:px-2",
                                item.enabled
                                  ? "border-amber-200 text-amber-700 hover:bg-amber-50 dark:border-amber-900/60 dark:text-amber-300 dark:hover:bg-amber-950/30"
                                  : "border-emerald-200 text-emerald-700 hover:bg-emerald-50 dark:border-emerald-900/60 dark:text-emerald-300 dark:hover:bg-emerald-950/30"
                              )}
                              onClick={() => void onToggleMappingEnabled(item)}
                              disabled={
                                busy ||
                                testingId === item.id ||
                                deletingId === item.id ||
                                togglingId === item.id
                              }
                            >
                              {item.enabled ? t("tunnel.disable_action") : t("tunnel.enable_action")}
                            </button>
                            <button
                              className="inline-flex items-center gap-1 rounded-lg border border-slate-200 px-1.5 py-1 text-xs font-semibold hover:bg-slate-50 sm:px-2 dark:border-neutral-700 dark:hover:bg-neutral-800"
                              onClick={() => void runTest(item.id)}
                              disabled={
                                testingId === item.id ||
                                deletingId === item.id ||
                                togglingId === item.id ||
                                busy
                              }
                            >
                              <FiActivity className={cn(testingId === item.id && "animate-pulse")} />
                              {t("tunnel.test")}
                            </button>
                            <button
                              className="inline-flex items-center gap-1 rounded-lg border border-rose-200 px-1.5 py-1 text-xs font-semibold text-rose-600 hover:bg-rose-50 sm:px-2 dark:border-rose-900/60 dark:text-rose-300 dark:hover:bg-rose-950/40"
                              onClick={() => void onDeleteMapping(item.id)}
                              disabled={busy || togglingId === item.id || deletingId === item.id}
                            >
                              {deletingId === item.id ? <FiRefreshCw className="animate-spin" /> : <FiTrash2 />}
                              {deletingId === item.id ? t("tunnel.deleting") : t("tunnel.delete")}
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

          <section className="flex h-full min-h-[320px] min-w-0 flex-col overflow-hidden rounded-3xl border border-slate-200/80 bg-white/80 p-4 shadow-soft xl:min-h-0 dark:border-neutral-800/80 dark:bg-neutral-900/60">
            <h2 className="mb-3 text-lg font-semibold">{t("tunnel.local_ports")}</h2>
            <div className="min-h-0 flex-1 overflow-auto rounded-2xl border border-slate-200/80 dark:border-neutral-800/80">
              <table className="min-w-full text-sm">
                <thead className="sticky top-0 z-10 bg-slate-100 dark:bg-neutral-900">
                  <tr className="text-left text-slate-600 dark:text-neutral-300">
                    <th className="whitespace-nowrap px-3 py-2">{t("tunnel.protocol")}</th>
                    <th className="whitespace-nowrap px-3 py-2">{t("tunnel.address")}</th>
                    <th className="whitespace-nowrap px-3 py-2">{t("tunnel.port")}</th>
                    <th className="whitespace-nowrap px-3 py-2">{t("tunnel.process")}</th>
                    <th className="whitespace-nowrap px-3 py-2">{t("tunnel.pid")}</th>
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
                      <tr
                        key={`${item.protocol}-${item.address}-${item.port}-${item.pid}-${index}`}
                        className="border-t border-slate-200/80 dark:border-neutral-800/80"
                      >
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

      {proxyDialogOpen ? (
        <div
          className="fixed inset-0 z-[120] flex items-center justify-center bg-slate-900/45 p-4 backdrop-blur-[2px]"
          onClick={() => {
            if (!savingConfig) {
              setProxyDialogOpen(false);
            }
          }}
        >
          <section
            className="flex max-h-[92dvh] w-full max-w-2xl min-w-0 flex-col rounded-3xl bg-white/95 p-4 shadow-2xl ring-1 ring-slate-200/80 dark:bg-neutral-900/95 dark:ring-neutral-700"
            onClick={(event) => event.stopPropagation()}
          >
            <div className="flex items-center justify-between gap-3">
              <h3 className="text-sm font-semibold tracking-tight text-slate-900 dark:text-neutral-50">
                {t("tunnel.proxy_settings")}
              </h3>
              <button
                type="button"
                className={cn(
                  "inline-flex h-9 items-center gap-1.5 rounded-xl px-3 text-xs font-semibold transition-colors",
                  savingConfig
                    ? "cursor-not-allowed bg-slate-200 text-slate-500 dark:bg-neutral-800 dark:text-neutral-500"
                    : "bg-slate-100 text-slate-700 hover:bg-slate-200 dark:bg-neutral-800 dark:text-neutral-100 dark:hover:bg-neutral-700"
                )}
                disabled={savingConfig}
                onClick={() => setProxyDialogOpen(false)}
              >
                <FiX />
                {t("common.close")}
              </button>
            </div>

            <div className="mt-4 grid grid-cols-1 gap-2 md:grid-cols-[1fr_160px]">
              <input
                className="rounded-2xl border border-slate-200 bg-white px-3 py-2 text-sm dark:border-neutral-700 dark:bg-neutral-900"
                placeholder={t("tunnel.proxy_host")}
                value={proxyHost}
                onChange={(event) => setProxyHost(event.target.value)}
              />
              <input
                className="rounded-2xl border border-slate-200 bg-white px-3 py-2 text-sm dark:border-neutral-700 dark:bg-neutral-900"
                placeholder={t("tunnel.proxy_port")}
                value={proxyPort}
                onChange={(event) => setProxyPort(event.target.value)}
              />
              <input
                className="rounded-2xl border border-slate-200 bg-white px-3 py-2 text-sm md:col-span-2 dark:border-neutral-700 dark:bg-neutral-900"
                placeholder={t("tunnel.proxy_token")}
                value={proxyToken}
                onChange={(event) => setProxyToken(event.target.value)}
              />
            </div>

            <div className="mt-4 flex justify-end">
              <button
                className="inline-flex items-center justify-center gap-2 rounded-xl bg-slate-900 px-3 py-2 text-sm font-semibold text-white hover:bg-slate-800 disabled:cursor-not-allowed disabled:opacity-60 dark:bg-neutral-100 dark:text-neutral-900 dark:hover:bg-neutral-200"
                onClick={() => void onSaveProxy()}
                disabled={savingConfig}
              >
                <FiSave />
                {t("common.save")}
              </button>
            </div>
          </section>
        </div>
      ) : null}

      {mappingDialogOpen ? (
        <div
          className="fixed inset-0 z-[121] flex items-center justify-center bg-slate-900/45 p-4 backdrop-blur-[2px]"
          onClick={() => {
            if (!busy) {
              setMappingDialogOpen(false);
            }
          }}
        >
          <section
            className="flex max-h-[92dvh] w-full max-w-2xl min-w-0 flex-col rounded-3xl bg-white/95 p-4 shadow-2xl ring-1 ring-slate-200/80 dark:bg-neutral-900/95 dark:ring-neutral-700"
            onClick={(event) => event.stopPropagation()}
          >
            <div className="flex items-center justify-between gap-3">
              <h3 className="text-sm font-semibold tracking-tight text-slate-900 dark:text-neutral-50">
                {t("tunnel.add_mapping")}
              </h3>
              <button
                type="button"
                className={cn(
                  "inline-flex h-9 items-center gap-1.5 rounded-xl px-3 text-xs font-semibold transition-colors",
                  busy
                    ? "cursor-not-allowed bg-slate-200 text-slate-500 dark:bg-neutral-800 dark:text-neutral-500"
                    : "bg-slate-100 text-slate-700 hover:bg-slate-200 dark:bg-neutral-800 dark:text-neutral-100 dark:hover:bg-neutral-700"
                )}
                disabled={busy}
                onClick={() => setMappingDialogOpen(false)}
              >
                <FiX />
                {t("common.close")}
              </button>
            </div>

            <div className="mt-4 grid grid-cols-1 gap-2 md:grid-cols-2">
              <input
                className="rounded-2xl border border-slate-200 bg-white px-3 py-2 text-sm dark:border-neutral-700 dark:bg-neutral-900"
                placeholder={t("tunnel.name")}
                value={name}
                onChange={(event) => setName(event.target.value)}
              />
              <select
                className="rounded-2xl border border-slate-200 bg-white px-3 py-2 text-sm dark:border-neutral-700 dark:bg-neutral-900"
                value={protocol}
                onChange={(event) => setProtocol(event.target.value === "udp" ? "udp" : "tcp")}
              >
                <option value="tcp">TCP</option>
                <option value="udp">UDP</option>
              </select>
              <input
                className="rounded-2xl border border-slate-200 bg-white px-3 py-2 text-sm dark:border-neutral-700 dark:bg-neutral-900"
                placeholder={t("tunnel.local_host")}
                value={localHost}
                onChange={(event) => setLocalHost(event.target.value)}
              />
              <input
                className="rounded-2xl border border-slate-200 bg-white px-3 py-2 text-sm dark:border-neutral-700 dark:bg-neutral-900"
                placeholder={t("tunnel.local_port")}
                value={localPort}
                onChange={(event) => setLocalPort(event.target.value)}
              />
              <input
                className="rounded-2xl border border-slate-200 bg-white px-3 py-2 text-sm dark:border-neutral-700 dark:bg-neutral-900"
                placeholder={t("tunnel.remote_port")}
                value={remotePort}
                onChange={(event) => setRemotePort(event.target.value)}
              />
              <label className="inline-flex items-center justify-between gap-3 rounded-2xl border border-slate-200 bg-white px-3 py-2 text-sm text-slate-700 dark:border-neutral-700 dark:bg-neutral-900 dark:text-neutral-300">
                <span className="font-semibold">{t("tunnel.enabled")}</span>
                <span className="inline-flex items-center gap-2">
                  <span
                    className={cn(
                      "rounded-full px-2 py-0.5 text-[11px] font-semibold",
                      enabled
                        ? "bg-emerald-100 text-emerald-700 dark:bg-emerald-900/35 dark:text-emerald-300"
                        : "bg-slate-200 text-slate-600 dark:bg-neutral-800 dark:text-neutral-300"
                    )}
                  >
                    {enabled ? t("common.on") : t("common.off")}
                  </span>
                  <span className="relative inline-flex h-5 w-9 shrink-0">
                    <input
                      type="checkbox"
                      checked={enabled}
                      onChange={(event) => setEnabled(event.target.checked)}
                      className="peer sr-only"
                    />
                    <span className="absolute inset-0 rounded-full bg-slate-300 transition-colors peer-checked:bg-slate-900 dark:bg-neutral-700 dark:peer-checked:bg-neutral-100" />
                    <span className="absolute left-0.5 top-0.5 h-4 w-4 rounded-full bg-white shadow-sm transition-transform peer-checked:translate-x-4 dark:bg-neutral-900 dark:peer-checked:bg-neutral-900" />
                  </span>
                </span>
              </label>
            </div>

            <div className="mt-4 flex justify-end">
              <button
                className="inline-flex items-center gap-2 rounded-xl bg-slate-900 px-3 py-2 text-sm font-semibold text-white hover:bg-slate-800 disabled:cursor-not-allowed disabled:opacity-60 dark:bg-neutral-100 dark:text-neutral-900 dark:hover:bg-neutral-200"
                onClick={() => void onUpsertMapping()}
                disabled={busy}
              >
                <FiPlus />
                {t("tunnel.add_mapping")}
              </button>
            </div>
          </section>
        </div>
      ) : null}
    </div>
  );
}
