import { useCallback, useEffect, useMemo, useRef, useState } from "react";
import { FiCode, FiExternalLink, FiMaximize2, FiMinimize2, FiRefreshCw, FiX } from "react-icons/fi";

import { getTask, startTask, updateCodeServerConfig } from "../api/client";
import { useI18n } from "../i18n";
import { toast } from "../toast";
import type { SessionInfo } from "../types";
import { decodeBase64Utf8 } from "../util/codec";
import { cn } from "../util/cn";

type Props = {
  session: SessionInfo;
  hostOs: string;
  codeServerInstalled: boolean;
};

type FullscreenDocument = Document & {
  webkitFullscreenElement?: Element | null;
  webkitExitFullscreen?: () => Promise<void> | void;
};

type FullscreenPanel = HTMLDivElement & {
  webkitRequestFullscreen?: () => Promise<void> | void;
};

type CodeServerTlsMode = "ferryman" | "selfsigned" | "custom";

type InstallCodeServerOptions = {
  port: number;
  httpsEnabled: boolean;
  tlsMode: CodeServerTlsMode;
  customCertPath: string;
  customKeyPath: string;
};

type CodeServerConfigPayload = {
  port: number;
  https_enabled: boolean;
  tls_mode: CodeServerTlsMode;
  custom_cert_path: string;
  custom_key_path: string;
};

const DEFAULT_CODE_SERVER_PORT = 13337;
const CODE_SERVER_PORT_STORAGE_KEY = "ferryman.codeserver.port";
const CODE_SERVER_HTTPS_ENABLED_STORAGE_KEY = "ferryman.codeserver.https_enabled";
const CODE_SERVER_TLS_MODE_STORAGE_KEY = "ferryman.codeserver.tls_mode";
const CODE_SERVER_CUSTOM_CERT_PATH_STORAGE_KEY = "ferryman.codeserver.custom_cert_path";
const CODE_SERVER_CUSTOM_KEY_PATH_STORAGE_KEY = "ferryman.codeserver.custom_key_path";
const CODE_SERVER_CONFIG_SYNC_DEBOUNCE_MS = 400;

function parsePort(value: string): number | null {
  const trimmed = value.trim();
  if (!/^\d+$/.test(trimmed)) {
    return null;
  }
  const parsed = Number(trimmed);
  if (!Number.isInteger(parsed) || parsed < 1 || parsed > 65535) {
    return null;
  }
  return parsed;
}

function loadStoredPortInput() {
  if (typeof window === "undefined") {
    return String(DEFAULT_CODE_SERVER_PORT);
  }
  const raw = window.localStorage.getItem(CODE_SERVER_PORT_STORAGE_KEY);
  if (!raw) {
    return String(DEFAULT_CODE_SERVER_PORT);
  }
  const parsed = parsePort(raw);
  return parsed == null ? String(DEFAULT_CODE_SERVER_PORT) : String(parsed);
}

function parseStoredBool(value: string | null, fallback: boolean) {
  if (!value) {
    return fallback;
  }
  const normalized = value.trim().toLowerCase();
  if (normalized === "1" || normalized === "true" || normalized === "yes" || normalized === "on") {
    return true;
  }
  if (normalized === "0" || normalized === "false" || normalized === "no" || normalized === "off") {
    return false;
  }
  return fallback;
}

function normalizeTlsMode(value: string): CodeServerTlsMode {
  const normalized = value.trim().toLowerCase();
  if (normalized === "custom") {
    return "custom";
  }
  if (normalized === "selfsigned" || normalized === "self-signed" || normalized === "self_signed") {
    return "selfsigned";
  }
  return "ferryman";
}

function loadStoredHttpsEnabled() {
  if (typeof window === "undefined") {
    return true;
  }
  return parseStoredBool(window.localStorage.getItem(CODE_SERVER_HTTPS_ENABLED_STORAGE_KEY), true);
}

function loadStoredTlsMode() {
  if (typeof window === "undefined") {
    return "ferryman" as CodeServerTlsMode;
  }
  return normalizeTlsMode(window.localStorage.getItem(CODE_SERVER_TLS_MODE_STORAGE_KEY) ?? "ferryman");
}

function loadStoredCustomPath(storageKey: string) {
  if (typeof window === "undefined") {
    return "";
  }
  return window.localStorage.getItem(storageKey) ?? "";
}

function shellSingleQuote(value: string) {
  return `'${value.replace(/'/g, `'\\''`)}'`;
}

function powerShellSingleQuote(value: string) {
  return value.replace(/'/g, "''");
}

function buildInstallCodeServerCommand(hostOs: string, options: InstallCodeServerOptions) {
  const port = options.port;
  const httpsEnabled = options.httpsEnabled;
  const tlsMode = options.tlsMode;
  const customCertPath = options.customCertPath.trim();
  const customKeyPath = options.customKeyPath.trim();
  const httpsEnabledFlag = httpsEnabled ? "1" : "0";
  const tlsModeLiteral = shellSingleQuote(tlsMode);
  const customCertLiteral = shellSingleQuote(customCertPath);
  const customKeyLiteral = shellSingleQuote(customKeyPath);
  const customCertPs = powerShellSingleQuote(customCertPath);
  const customKeyPs = powerShellSingleQuote(customKeyPath);

  if (hostOs === "linux" || hostOs === "macos" || hostOs === "freebsd") {
    return `
set -e
echo "[codeserver] checking current status..."
if ! command -v code-server >/dev/null 2>&1; then
  echo "[codeserver] installing via official script (prefer latest standalone)..."
  if curl -fsSL https://code-server.dev/install.sh | sh -s -- --method=standalone; then
    echo "[codeserver] standalone install completed."
  else
    echo "[codeserver] standalone install failed, falling back to default installer..."
    curl -fsSL https://code-server.dev/install.sh | sh
  fi
else
  echo "[codeserver] code-server already installed."
fi

if ! command -v code-server >/dev/null 2>&1; then
  echo "[codeserver] code-server not found after installation."
  exit 1
fi

echo "[codeserver] starting code-server on 0.0.0.0:${port} ..."
if command -v pkill >/dev/null 2>&1; then
  pkill -f "code-server.*--bind-addr 0.0.0.0:${port}" || true
fi
mkdir -p "\${HOME}/.ferryman"
printf "%s\n" "${port}" > "\${HOME}/.ferryman/codeserver_port"
printf "%s\n" "${httpsEnabledFlag}" > "\${HOME}/.ferryman/codeserver_https_enabled"
printf "%s\n" ${tlsModeLiteral} > "\${HOME}/.ferryman/codeserver_https_mode"
printf "%s\n" ${customCertLiteral} > "\${HOME}/.ferryman/codeserver_https_cert_file"
printf "%s\n" ${customKeyLiteral} > "\${HOME}/.ferryman/codeserver_https_key_file"

https_enabled="${httpsEnabledFlag}"
tls_mode=${tlsModeLiteral}
custom_cert_path=${customCertLiteral}
custom_key_path=${customKeyLiteral}
config_file="\${HOME}/.ferryman/config.ini"
if [ -f "$config_file" ]; then
  upsert_ini_key() {
    ini_key="$1"
    ini_value="$2"
    tmp_file="\${config_file}.tmp.$$"
    awk -v key="$ini_key" -v value="$ini_value" '
      BEGIN { updated = 0 }
      {
        line = $0
        trimmed = line
        sub(/^[ \t]+/, "", trimmed)
        if (trimmed ~ ("^" key "=")) {
          print key "=" value
          updated = 1
          next
        }
        print line
      }
      END {
        if (updated == 0) {
          print key "=" value
        }
      }
    ' "$config_file" > "$tmp_file" && mv "$tmp_file" "$config_file"
  }
  upsert_ini_key "codeserver_port" "${port}"
  upsert_ini_key "codeserver_https_enabled" "$https_enabled"
  upsert_ini_key "codeserver_https_mode" "$tls_mode"
  upsert_ini_key "codeserver_https_cert_file" "$custom_cert_path"
  upsert_ini_key "codeserver_https_key_file" "$custom_key_path"
fi
mkdir -p "\${HOME}/.ferryman/logs"
scheme="http"
if [ "$https_enabled" = "1" ]; then
  scheme="https"
  case "$tls_mode" in
    custom)
      if [ -z "$custom_cert_path" ] || [ -z "$custom_key_path" ]; then
        echo "[codeserver] custom cert mode requires cert and key paths."
        exit 1
      fi
      if [ ! -s "$custom_cert_path" ] || [ ! -s "$custom_key_path" ]; then
        echo "[codeserver] custom cert/key not found: cert=$custom_cert_path key=$custom_key_path"
        exit 1
      fi
      echo "[codeserver] using custom https certificate."
      nohup code-server --bind-addr 0.0.0.0:${port} --auth none --cert "$custom_cert_path" --cert-key "$custom_key_path" > "\${HOME}/.ferryman/logs/codeserver.log" 2>&1 &
      ;;
    selfsigned)
      echo "[codeserver] using code-server self-signed https certificate."
      nohup code-server --bind-addr 0.0.0.0:${port} --auth none --cert > "\${HOME}/.ferryman/logs/codeserver.log" 2>&1 &
      ;;
    *)
      ferryman_cert="\${HOME}/.ferryman/cert/server.crt"
      ferryman_key="\${HOME}/.ferryman/cert/server.key"
      if [ -s "$ferryman_cert" ] && [ -s "$ferryman_key" ]; then
        echo "[codeserver] using ferryman https certificate."
        nohup code-server --bind-addr 0.0.0.0:${port} --auth none --cert "$ferryman_cert" --cert-key "$ferryman_key" > "\${HOME}/.ferryman/logs/codeserver.log" 2>&1 &
      else
        echo "[codeserver] ferryman cert not found; fallback to self-signed https certificate."
        nohup code-server --bind-addr 0.0.0.0:${port} --auth none --cert > "\${HOME}/.ferryman/logs/codeserver.log" 2>&1 &
      fi
      ;;
  esac
else
  nohup code-server --bind-addr 0.0.0.0:${port} --auth none > "\${HOME}/.ferryman/logs/codeserver.log" 2>&1 &
fi
sleep 2

if command -v curl >/dev/null 2>&1; then
  if [ "$scheme" = "https" ]; then
    if curl -kfsS "$scheme://127.0.0.1:${port}/healthz" >/dev/null 2>&1; then
      echo "[codeserver] health check passed."
    else
      echo "[codeserver] health check failed. check \${HOME}/.ferryman/logs/codeserver.log"
    fi
  elif curl -fsS "$scheme://127.0.0.1:${port}/healthz" >/dev/null 2>&1; then
    echo "[codeserver] health check passed."
  else
    echo "[codeserver] health check failed. check \${HOME}/.ferryman/logs/codeserver.log"
  fi
fi

code-server --version || true
echo "[codeserver] done."
`.trim();
  }

  if (hostOs === "windows") {
    return [
      "powershell -NoProfile -NonInteractive -ExecutionPolicy Bypass -Command",
      `"`,
      "$ErrorActionPreference='Stop';",
      "Write-Host '[codeserver] checking current status...';",
      "if (-not (Get-Command code-server -ErrorAction SilentlyContinue)) {",
      "  if (-not (Get-Command npm -ErrorAction SilentlyContinue)) {",
      "    Write-Host '[codeserver] npm not found.';",
      "    exit 1",
      "  };",
      "  npm install -g code-server",
      "} else {",
      "  Write-Host '[codeserver] code-server already installed.'",
      "};",
      "$exe = (Get-Command code-server -ErrorAction Stop).Source;",
      "$ferrymanDir = Join-Path $Env:USERPROFILE '.ferryman';",
      "New-Item -Path $ferrymanDir -ItemType Directory -Force | Out-Null;",
      `Set-Content -Path (Join-Path $ferrymanDir 'codeserver_port') -Value '${port}' -Encoding ASCII;`,
      `Set-Content -Path (Join-Path $ferrymanDir 'codeserver_https_enabled') -Value '${httpsEnabledFlag}' -Encoding ASCII;`,
      `Set-Content -Path (Join-Path $ferrymanDir 'codeserver_https_mode') -Value '${powerShellSingleQuote(tlsMode)}' -Encoding ASCII;`,
      `Set-Content -Path (Join-Path $ferrymanDir 'codeserver_https_cert_file') -Value '${customCertPs}' -Encoding ASCII;`,
      `Set-Content -Path (Join-Path $ferrymanDir 'codeserver_https_key_file') -Value '${customKeyPs}' -Encoding ASCII;`,
      `$httpsEnabled = '${httpsEnabledFlag}' -eq '1';`,
      `$tlsMode = '${powerShellSingleQuote(tlsMode)}';`,
      `$customCert = '${customCertPs}';`,
      `$customKey = '${customKeyPs}';`,
      "$configPath = Join-Path $ferrymanDir 'config.ini';",
      "function Upsert-IniKey {",
      "  param([string]$Path, [string]$Key, [string]$Value)",
      "  if (-not (Test-Path -LiteralPath $Path)) {",
      "    return",
      "  }",
      "  $lines = Get-Content -LiteralPath $Path;",
      "  if ($null -eq $lines) { $lines = @() }",
      "  if ($lines -isnot [System.Array]) { $lines = @($lines) }",
      "  $pattern = '^\\s*' + [Regex]::Escape($Key) + '=';",
      "  $updated = $false;",
      "  for ($i = 0; $i -lt $lines.Count; $i++) {",
      "    if ($lines[$i] -match $pattern) {",
      "      $lines[$i] = ($Key + '=' + $Value);",
      "      $updated = $true;",
      "      break",
      "    }",
      "  }",
      "  if (-not $updated) {",
      "    $lines += ($Key + '=' + $Value)",
      "  }",
      "  Set-Content -LiteralPath $Path -Value $lines -Encoding ASCII;",
      "}",
      `Upsert-IniKey -Path $configPath -Key 'codeserver_port' -Value '${port}';`,
      "Upsert-IniKey -Path $configPath -Key 'codeserver_https_enabled' -Value ($(if ($httpsEnabled) { 'true' } else { 'false' }));",
      "Upsert-IniKey -Path $configPath -Key 'codeserver_https_mode' -Value $tlsMode;",
      "Upsert-IniKey -Path $configPath -Key 'codeserver_https_cert_file' -Value $customCert;",
      "Upsert-IniKey -Path $configPath -Key 'codeserver_https_key_file' -Value $customKey;",
      `$args = @('--bind-addr','0.0.0.0:${port}','--auth','none');`,
      `$healthScheme = 'http';`,
      "if ($httpsEnabled) {",
      "  $healthScheme = 'https';",
      "  if ($tlsMode -eq 'custom') {",
      "    if ([string]::IsNullOrWhiteSpace($customCert) -or [string]::IsNullOrWhiteSpace($customKey)) {",
      "      Write-Host '[codeserver] custom cert mode requires cert and key paths.';",
      "      exit 1",
      "    }",
      "    if (-not (Test-Path -LiteralPath $customCert) -or -not (Test-Path -LiteralPath $customKey)) {",
      "      Write-Host ('[codeserver] custom cert/key not found: cert=' + $customCert + ' key=' + $customKey);",
      "      exit 1",
      "    }",
      "    Write-Host '[codeserver] using custom https certificate.';",
      "    $args += @('--cert', $customCert, '--cert-key', $customKey);",
      "  } elseif ($tlsMode -eq 'selfsigned') {",
      "    Write-Host '[codeserver] using code-server self-signed https certificate.';",
      "    $args += '--cert';",
      "  } else {",
      "    $ferrymanCert = Join-Path (Join-Path $ferrymanDir 'cert') 'server.crt';",
      "    $ferrymanKey = Join-Path (Join-Path $ferrymanDir 'cert') 'server.key';",
      "    if ((Test-Path -LiteralPath $ferrymanCert) -and (Test-Path -LiteralPath $ferrymanKey)) {",
      "      Write-Host '[codeserver] using ferryman https certificate.';",
      "      $args += @('--cert', $ferrymanCert, '--cert-key', $ferrymanKey);",
      "    } else {",
      "      Write-Host '[codeserver] ferryman cert not found; fallback to self-signed https certificate.';",
      "      $args += '--cert';",
      "    }",
      "  }",
      "}",
      "Start-Process -FilePath $exe -ArgumentList $args -WindowStyle Hidden;",
      "Start-Sleep -Seconds 2;",
      "if ($healthScheme -eq 'https') {",
      "  [System.Net.ServicePointManager]::ServerCertificateValidationCallback = { $true };",
      "}",
      `try {`,
      `  $r = Invoke-WebRequest -UseBasicParsing -Uri ($healthScheme + '://127.0.0.1:${port}/healthz') -TimeoutSec 2;`,
      `  if ($r.StatusCode -ge 200 -and $r.StatusCode -lt 400) {`,
      `    Write-Host '[codeserver] health check passed.'`,
      `  } else {`,
      `    Write-Host '[codeserver] health check failed.'`,
      "  }",
      "} catch {",
      "  Write-Host '[codeserver] health check failed.'",
      "}",
      "code-server --version;",
      "Write-Host '[codeserver] done.'",
      `"`,
    ].join(" ");
  }

  return "";
}

function panelUrlForCurrentHost(port: number, httpsEnabled: boolean) {
  const protocol = httpsEnabled ? "https" : "http";
  if (typeof window === "undefined") {
    return `${protocol}://127.0.0.1:${port}/`;
  }
  const hostname = window.location.hostname;
  const wrappedHost = hostname.includes(":") ? `[${hostname}]` : hostname;
  return `${protocol}://${wrappedHost}:${port}/`;
}

function buildCodeServerConfigSignature(payload: CodeServerConfigPayload) {
  return [
    String(payload.port),
    payload.https_enabled ? "1" : "0",
    payload.tls_mode,
    payload.custom_cert_path,
    payload.custom_key_path,
  ].join("|");
}

function getFullscreenElement() {
  const doc = document as FullscreenDocument;
  return document.fullscreenElement ?? doc.webkitFullscreenElement ?? null;
}

export default function CodeServerPage({ session, hostOs, codeServerInstalled }: Props) {
  const { t } = useI18n();
  const panelRef = useRef<HTMLDivElement | null>(null);
  const installViewportRef = useRef<HTMLPreElement | null>(null);
  const installNotifiedRef = useRef(false);

  const [installedState, setInstalledState] = useState(codeServerInstalled);
  const [installDialogOpen, setInstallDialogOpen] = useState(false);
  const [installTaskId, setInstallTaskId] = useState("");
  const [installStatus, setInstallStatus] = useState("");
  const [installLogs, setInstallLogs] = useState("");
  const [installRunning, setInstallRunning] = useState(false);
  const [iframeKey, setIframeKey] = useState(0);
  const [panelFullscreen, setPanelFullscreen] = useState(false);
  const [portEditing, setPortEditing] = useState(false);
  const [customCertEditing, setCustomCertEditing] = useState(false);
  const [customKeyEditing, setCustomKeyEditing] = useState(false);
  const [portInput, setPortInput] = useState(() => loadStoredPortInput());
  const [httpsEnabled, setHttpsEnabled] = useState(() => loadStoredHttpsEnabled());
  const [tlsMode, setTlsMode] = useState<CodeServerTlsMode>(() => loadStoredTlsMode());
  const [customCertPath, setCustomCertPath] = useState(() =>
    loadStoredCustomPath(CODE_SERVER_CUSTOM_CERT_PATH_STORAGE_KEY)
  );
  const [customKeyPath, setCustomKeyPath] = useState(() =>
    loadStoredCustomPath(CODE_SERVER_CUSTOM_KEY_PATH_STORAGE_KEY)
  );
  const configSyncInitializedRef = useRef(false);
  const lastSyncedConfigSignatureRef = useRef("");
  const configSyncRequestIdRef = useRef(0);

  const parsedPort = useMemo(() => parsePort(portInput), [portInput]);
  const effectivePort = parsedPort ?? DEFAULT_CODE_SERVER_PORT;
  const panelUrl = useMemo(() => panelUrlForCurrentHost(effectivePort, httpsEnabled), [effectivePort, httpsEnabled]);
  const trimmedCustomCertPath = customCertPath.trim();
  const trimmedCustomKeyPath = customKeyPath.trim();
  const configSyncPaused = portEditing || customCertEditing || customKeyEditing;
  const configPayload = useMemo<CodeServerConfigPayload | null>(() => {
    if (parsedPort == null) {
      return null;
    }
    if (httpsEnabled && tlsMode === "custom" && (!trimmedCustomCertPath || !trimmedCustomKeyPath)) {
      return null;
    }
    return {
      port: parsedPort,
      https_enabled: httpsEnabled,
      tls_mode: tlsMode,
      custom_cert_path: trimmedCustomCertPath,
      custom_key_path: trimmedCustomKeyPath,
    };
  }, [httpsEnabled, parsedPort, tlsMode, trimmedCustomCertPath, trimmedCustomKeyPath]);
  const configPayloadSignature = useMemo(
    () => (configPayload == null ? "" : buildCodeServerConfigSignature(configPayload)),
    [configPayload]
  );

  const installSupportedHost = hostOs === "linux" || hostOs === "macos" || hostOs === "windows" || hostOs === "freebsd";
  const canInstallCodeServer = installSupportedHost && !installedState;

  useEffect(() => {
    setInstalledState(codeServerInstalled);
  }, [codeServerInstalled]);

  useEffect(() => {
    const viewport = installViewportRef.current;
    if (!viewport) {
      return;
    }
    viewport.scrollTop = viewport.scrollHeight;
  }, [installLogs]);

  useEffect(() => {
    if (typeof window === "undefined") {
      return;
    }
    if (parsedPort == null) {
      window.localStorage.removeItem(CODE_SERVER_PORT_STORAGE_KEY);
      return;
    }
    window.localStorage.setItem(CODE_SERVER_PORT_STORAGE_KEY, String(parsedPort));
  }, [parsedPort]);

  useEffect(() => {
    if (typeof window === "undefined") {
      return;
    }
    window.localStorage.setItem(CODE_SERVER_HTTPS_ENABLED_STORAGE_KEY, httpsEnabled ? "1" : "0");
  }, [httpsEnabled]);

  useEffect(() => {
    if (typeof window === "undefined") {
      return;
    }
    window.localStorage.setItem(CODE_SERVER_TLS_MODE_STORAGE_KEY, tlsMode);
  }, [tlsMode]);

  useEffect(() => {
    if (typeof window === "undefined") {
      return;
    }
    window.localStorage.setItem(CODE_SERVER_CUSTOM_CERT_PATH_STORAGE_KEY, customCertPath);
  }, [customCertPath]);

  useEffect(() => {
    if (typeof window === "undefined") {
      return;
    }
    window.localStorage.setItem(CODE_SERVER_CUSTOM_KEY_PATH_STORAGE_KEY, customKeyPath);
  }, [customKeyPath]);

  useEffect(() => {
    if (!httpsEnabled || tlsMode !== "custom") {
      setCustomCertEditing(false);
      setCustomKeyEditing(false);
    }
  }, [httpsEnabled, tlsMode]);

  useEffect(() => {
    if (!installedState) {
      configSyncInitializedRef.current = false;
      lastSyncedConfigSignatureRef.current = "";
      configSyncRequestIdRef.current += 1;
      return;
    }
    if (configPayload == null || !configPayloadSignature) {
      configSyncRequestIdRef.current += 1;
      return;
    }
    if (!configSyncInitializedRef.current) {
      configSyncInitializedRef.current = true;
      lastSyncedConfigSignatureRef.current = configPayloadSignature;
      return;
    }
    if (configSyncPaused) {
      configSyncRequestIdRef.current += 1;
      return;
    }
    if (configPayloadSignature === lastSyncedConfigSignatureRef.current) {
      return;
    }

    const requestId = ++configSyncRequestIdRef.current;
    const timer = window.setTimeout(() => {
      void (async () => {
        const result = await updateCodeServerConfig(session.token, configPayload);
        if (requestId !== configSyncRequestIdRef.current) {
          return;
        }
        if (!result.ok) {
          toast.error(result.error ?? t("toast.request_failed"));
          return;
        }

        const normalizedPayload: CodeServerConfigPayload = {
          port: Number.isFinite(result.codeserver_port) ? result.codeserver_port : configPayload.port,
          https_enabled: result.https_enabled,
          tls_mode: normalizeTlsMode(result.tls_mode),
          custom_cert_path: (result.custom_cert_path ?? "").trim(),
          custom_key_path: (result.custom_key_path ?? "").trim(),
        };
        lastSyncedConfigSignatureRef.current = buildCodeServerConfigSignature(normalizedPayload);
        setPortInput(String(normalizedPayload.port));
        setHttpsEnabled(normalizedPayload.https_enabled);
        setTlsMode(normalizedPayload.tls_mode);
        setCustomCertPath(normalizedPayload.custom_cert_path);
        setCustomKeyPath(normalizedPayload.custom_key_path);
        setIframeKey((prev) => prev + 1);
      })();
    }, CODE_SERVER_CONFIG_SYNC_DEBOUNCE_MS);

    return () => {
      window.clearTimeout(timer);
    };
  }, [configPayload, configPayloadSignature, configSyncPaused, installedState, session.token, t]);

  useEffect(() => {
    const onFullscreenChange = () => {
      const panel = panelRef.current;
      setPanelFullscreen(panel != null && getFullscreenElement() === panel);
    };

    onFullscreenChange();
    document.addEventListener("fullscreenchange", onFullscreenChange);
    document.addEventListener("webkitfullscreenchange", onFullscreenChange);
    return () => {
      document.removeEventListener("fullscreenchange", onFullscreenChange);
      document.removeEventListener("webkitfullscreenchange", onFullscreenChange);
    };
  }, []);

  const openInNewTab = useCallback(() => {
    window.open(panelUrl, "_blank", "noopener,noreferrer");
  }, [panelUrl]);

  const togglePanelFullscreen = useCallback(async () => {
    const panel = panelRef.current as FullscreenPanel | null;
    const doc = document as FullscreenDocument;
    if (!panel) {
      return;
    }

    try {
      const current = getFullscreenElement();
      if (current === panel) {
        if (typeof document.exitFullscreen === "function") {
          await document.exitFullscreen();
          return;
        }
        if (typeof doc.webkitExitFullscreen === "function") {
          await doc.webkitExitFullscreen();
          return;
        }
        toast.error(t("codeserver.fullscreen_unsupported"));
        return;
      }

      if (current) {
        if (typeof document.exitFullscreen === "function") {
          await document.exitFullscreen();
        } else if (typeof doc.webkitExitFullscreen === "function") {
          await doc.webkitExitFullscreen();
        }
      }

      if (typeof panel.requestFullscreen === "function") {
        await panel.requestFullscreen();
        return;
      }
      if (typeof panel.webkitRequestFullscreen === "function") {
        await panel.webkitRequestFullscreen();
        return;
      }

      toast.error(t("codeserver.fullscreen_unsupported"));
    } catch {
      toast.error(t("codeserver.fullscreen_failed"));
    }
  }, [t]);

  const runInstallCodeServer = useCallback(async () => {
    if (installRunning) {
      return;
    }
    if (parsedPort == null) {
      toast.error(t("codeserver.invalid_port"));
      return;
    }
    if (httpsEnabled && tlsMode === "custom" && (!trimmedCustomCertPath || !trimmedCustomKeyPath)) {
      toast.error(t("codeserver.invalid_custom_cert"));
      return;
    }
    const command = buildInstallCodeServerCommand(hostOs, {
      port: parsedPort,
      httpsEnabled,
      tlsMode,
      customCertPath: trimmedCustomCertPath,
      customKeyPath: trimmedCustomKeyPath,
    });
    if (!command) {
      toast.error(t("codeserver.install_unsupported"));
      return;
    }

    setInstallDialogOpen(true);
    setInstallTaskId("");
    setInstallStatus("queued");
    setInstallLogs("");
    setInstallRunning(true);
    installNotifiedRef.current = false;

    const res = await startTask(session.token, command);
    if (!res.ok || !res.task_id) {
      setInstallStatus("failed");
      setInstallRunning(false);
      setInstallLogs(`[error] ${res.error ?? t("toast.request_failed")}`);
      toast.error(res.error ?? t("toast.request_failed"));
      return;
    }
    setInstallTaskId(res.task_id);
  }, [hostOs, httpsEnabled, installRunning, parsedPort, session.token, t, tlsMode, trimmedCustomCertPath, trimmedCustomKeyPath]);

  useEffect(() => {
    if (!installDialogOpen || !installTaskId) {
      return;
    }
    let stopped = false;
    const poll = async () => {
      const res = await getTask(session.token, installTaskId);
      if (stopped) {
        return;
      }
      if (!res.ok) {
        setInstallStatus("failed");
        setInstallRunning(false);
        setInstallLogs((prev) => {
          if (prev.includes("[error]")) {
            return prev;
          }
          const suffix = `[error] ${res.error ?? t("toast.request_failed")}`;
          return prev ? `${prev}\n${suffix}` : suffix;
        });
        if (!installNotifiedRef.current) {
          installNotifiedRef.current = true;
          toast.error(res.error ?? t("toast.request_failed"));
        }
        return;
      }

      const output = decodeBase64Utf8(res.output_base64 ?? "");
      setInstallLogs(output);
      const nextStatus = typeof res.status === "string" ? res.status : "";
      setInstallStatus(nextStatus);
      const finished = nextStatus === "succeeded" || nextStatus === "failed";
      if (!finished) {
        return;
      }

      setInstallRunning(false);
      if (installNotifiedRef.current) {
        return;
      }
      installNotifiedRef.current = true;
      if (nextStatus === "succeeded") {
        setInstalledState(true);
        setIframeKey((prev) => prev + 1);
        toast.success(t("codeserver.install_success"));
      } else {
        toast.error(t("codeserver.install_failed"));
      }
    };

    void poll();
    const timer = window.setInterval(() => {
      void poll();
    }, 1000);
    return () => {
      stopped = true;
      window.clearInterval(timer);
    };
  }, [installDialogOpen, installTaskId, session.token, t]);

  return (
    <>
      <div className="grid h-full min-h-[380px] grid-cols-1">
        <section className="flex min-h-0 min-w-0 flex-col overflow-hidden rounded-3xl bg-white/75 p-4 shadow-soft ring-1 ring-slate-200/70 backdrop-blur dark:bg-neutral-900/60 dark:ring-neutral-800/80">
          <div className="flex flex-wrap items-center justify-between gap-2">
            <div>
              <h3 className="inline-flex items-center gap-2 text-sm font-semibold text-slate-900 dark:text-neutral-50">
                <FiCode />
                {t("codeserver.title")}
              </h3>
              <div className="mt-1 text-xs text-slate-500 dark:text-neutral-400">{t("codeserver.subtitle")}</div>
            </div>

            <div className="flex flex-wrap items-center gap-2">
              <div className="inline-flex items-center gap-2">
                <span className="text-xs text-slate-500 dark:text-neutral-400">{t("codeserver.port_label")}</span>
                <input
                  type="text"
                  inputMode="numeric"
                  value={portInput}
                  onChange={(event) => setPortInput(event.target.value.replace(/[^\d]/g, "").slice(0, 5))}
                  onFocus={() => setPortEditing(true)}
                  onBlur={() => setPortEditing(false)}
                  onKeyDown={(event) => {
                    if (event.key === "Enter") {
                      event.currentTarget.blur();
                    }
                  }}
                  className={cn(
                    "h-8 w-24 rounded-xl border bg-white px-2.5 text-xs shadow-sm outline-none dark:bg-neutral-950/40",
                    parsedPort == null
                      ? "border-rose-300 text-rose-700 focus:border-rose-400 dark:border-rose-700 dark:text-rose-300"
                      : "border-slate-200 text-slate-700 focus:border-slate-300 dark:border-neutral-800 dark:text-neutral-100 dark:focus:border-neutral-700"
                  )}
                />
              </div>
              <div className="inline-flex items-center gap-1 rounded-xl bg-slate-100 p-1 dark:bg-neutral-800">
                <button
                  type="button"
                  className={cn(
                    "rounded-lg px-2 py-1 text-[11px] font-semibold transition-colors",
                    !httpsEnabled
                      ? "bg-white text-slate-800 shadow-sm dark:bg-neutral-700 dark:text-neutral-50"
                      : "text-slate-500 hover:text-slate-700 dark:text-neutral-400 dark:hover:text-neutral-200"
                  )}
                  onClick={() => setHttpsEnabled(false)}
                >
                  {t("codeserver.protocol_http")}
                </button>
                <button
                  type="button"
                  className={cn(
                    "rounded-lg px-2 py-1 text-[11px] font-semibold transition-colors",
                    httpsEnabled
                      ? "bg-white text-slate-800 shadow-sm dark:bg-neutral-700 dark:text-neutral-50"
                      : "text-slate-500 hover:text-slate-700 dark:text-neutral-400 dark:hover:text-neutral-200"
                  )}
                  onClick={() => setHttpsEnabled(true)}
                >
                  {t("codeserver.protocol_https")}
                </button>
              </div>
              {httpsEnabled ? (
                <select
                  value={tlsMode}
                  onChange={(event) => setTlsMode(normalizeTlsMode(event.target.value))}
                  className="h-8 rounded-xl border border-slate-200 bg-white px-2.5 text-xs text-slate-700 shadow-sm outline-none focus:border-slate-300 dark:border-neutral-800 dark:bg-neutral-950/40 dark:text-neutral-100 dark:focus:border-neutral-700"
                >
                  <option value="ferryman">{t("codeserver.tls_mode_ferryman")}</option>
                  <option value="selfsigned">{t("codeserver.tls_mode_selfsigned")}</option>
                  <option value="custom">{t("codeserver.tls_mode_custom")}</option>
                </select>
              ) : null}
              {canInstallCodeServer ? (
                <button
                  className={cn(
                    "inline-flex h-8 items-center rounded-xl px-3 text-xs font-semibold transition-colors",
                    installRunning
                      ? "cursor-wait bg-slate-200 text-slate-500 dark:bg-neutral-800 dark:text-neutral-500"
                      : "bg-amber-100 text-amber-900 hover:bg-amber-200 dark:bg-amber-900/40 dark:text-amber-300 dark:hover:bg-amber-900/60"
                  )}
                  onClick={() => void runInstallCodeServer()}
                  disabled={installRunning}
                >
                  {installRunning ? t("codeserver.installing") : t("codeserver.install")}
                </button>
              ) : null}

              {installedState ? (
                <>
                  <button
                    className="grid h-8 w-8 place-items-center rounded-xl bg-slate-100 text-slate-700 transition-colors hover:bg-slate-200 dark:bg-neutral-800 dark:text-neutral-100 dark:hover:bg-neutral-700"
                    onClick={() => setIframeKey((prev) => prev + 1)}
                    title={t("codeserver.reload")}
                  >
                    <FiRefreshCw />
                  </button>
                  <button
                    className="inline-flex h-8 items-center gap-1 rounded-xl bg-slate-100 px-3 text-xs font-semibold text-slate-700 transition-colors hover:bg-slate-200 dark:bg-neutral-800 dark:text-neutral-100 dark:hover:bg-neutral-700"
                    onClick={() => void togglePanelFullscreen()}
                  >
                    {panelFullscreen ? <FiMinimize2 /> : <FiMaximize2 />}
                    {panelFullscreen ? t("codeserver.exit_fullscreen") : t("codeserver.fullscreen")}
                  </button>
                  <button
                    className="inline-flex h-8 items-center gap-1 rounded-xl bg-slate-900 px-3 text-xs font-semibold text-white transition-colors hover:bg-slate-800 dark:bg-neutral-50 dark:text-neutral-900 dark:hover:bg-white"
                    onClick={openInNewTab}
                  >
                    <FiExternalLink />
                    {t("codeserver.open")}
                  </button>
                </>
              ) : null}
            </div>
          </div>

          {httpsEnabled && tlsMode === "custom" ? (
            <div className="mt-3 grid grid-cols-1 gap-2 md:grid-cols-2">
              <input
                type="text"
                value={customCertPath}
                onChange={(event) => setCustomCertPath(event.target.value)}
                onFocus={() => setCustomCertEditing(true)}
                onBlur={() => setCustomCertEditing(false)}
                placeholder={t("codeserver.custom_cert_path")}
                className="h-9 rounded-xl border border-slate-200 bg-white px-3 text-xs text-slate-700 shadow-sm outline-none focus:border-slate-300 dark:border-neutral-800 dark:bg-neutral-950/40 dark:text-neutral-100 dark:focus:border-neutral-700"
              />
              <input
                type="text"
                value={customKeyPath}
                onChange={(event) => setCustomKeyPath(event.target.value)}
                onFocus={() => setCustomKeyEditing(true)}
                onBlur={() => setCustomKeyEditing(false)}
                placeholder={t("codeserver.custom_key_path")}
                className="h-9 rounded-xl border border-slate-200 bg-white px-3 text-xs text-slate-700 shadow-sm outline-none focus:border-slate-300 dark:border-neutral-800 dark:bg-neutral-950/40 dark:text-neutral-100 dark:focus:border-neutral-700"
              />
            </div>
          ) : null}
          {httpsEnabled && tlsMode === "ferryman" ? (
            <div className="mt-2 text-xs text-slate-500 dark:text-neutral-400">{t("codeserver.ferryman_tls_hint")}</div>
          ) : null}

          {parsedPort == null ? (
            <div className="mt-2 text-xs text-rose-600 dark:text-rose-300">{t("codeserver.invalid_port")}</div>
          ) : null}
          {httpsEnabled && tlsMode === "custom" && (!trimmedCustomCertPath || !trimmedCustomKeyPath) ? (
            <div className="mt-2 text-xs text-rose-600 dark:text-rose-300">{t("codeserver.invalid_custom_cert")}</div>
          ) : null}

          {!installedState ? (
            <div className="mt-4 rounded-2xl border border-dashed border-slate-200 p-6 text-sm text-slate-600 dark:border-neutral-800 dark:text-neutral-300">
              <p>{installSupportedHost ? t("codeserver.not_installed") : t("codeserver.install_unsupported")}</p>
            </div>
          ) : (
            <>
              <div className="mt-3 truncate font-mono text-[11px] text-slate-500 dark:text-neutral-400" title={panelUrl}>
                {t("codeserver.panel_url")}: {panelUrl}
              </div>
              <div
                ref={panelRef}
                className="mt-3 min-h-0 flex-1 overflow-hidden rounded-2xl border border-slate-200 bg-white dark:border-neutral-800 dark:bg-neutral-950/30"
              >
                <iframe
                  key={iframeKey}
                  src={panelUrl}
                  title="code-server"
                  className="h-full w-full border-0"
                  loading="lazy"
                />
              </div>
            </>
          )}
        </section>
      </div>

      {installDialogOpen ? (
        <div
          className="fixed inset-0 z-[130] flex items-center justify-center bg-slate-900/45 p-4 backdrop-blur-[2px]"
          onClick={() => {
            if (!installRunning) {
              setInstallDialogOpen(false);
            }
          }}
        >
          <section
            className="flex max-h-[92dvh] w-full max-w-3xl min-w-0 flex-col rounded-3xl bg-white/95 p-4 shadow-2xl ring-1 ring-slate-200/80 dark:bg-neutral-900/95 dark:ring-neutral-700"
            onClick={(event) => event.stopPropagation()}
          >
            <div className="flex items-center justify-between gap-3">
              <h3 className="inline-flex items-center gap-2 text-sm font-semibold tracking-tight text-slate-900 dark:text-neutral-50">
                <FiCode className="text-[14px]" />
                {t("codeserver.install_dialog_title")}
              </h3>
              <div className="flex items-center gap-2">
                <span className="rounded-full bg-slate-100 px-2 py-0.5 text-[11px] font-semibold text-slate-600 dark:bg-neutral-800 dark:text-neutral-300">
                  {installStatus || "queued"}
                </span>
                <button
                  type="button"
                  className={cn(
                    "inline-flex h-9 items-center gap-1.5 rounded-xl px-3 text-xs font-semibold transition-colors",
                    installRunning
                      ? "cursor-not-allowed bg-slate-200 text-slate-500 dark:bg-neutral-800 dark:text-neutral-500"
                      : "bg-slate-100 text-slate-700 hover:bg-slate-200 dark:bg-neutral-800 dark:text-neutral-100 dark:hover:bg-neutral-700"
                  )}
                  disabled={installRunning}
                  onClick={() => setInstallDialogOpen(false)}
                >
                  <FiX />
                  {t("common.close")}
                </button>
              </div>
            </div>

            <pre
              ref={installViewportRef}
              className="mt-4 min-h-0 flex-1 overflow-auto whitespace-pre-wrap break-all rounded-2xl bg-neutral-950 p-3 font-mono text-[12px] leading-relaxed text-neutral-50"
            >
              {installLogs || t("codeserver.install_log_empty")}
            </pre>
          </section>
        </div>
      ) : null}
    </>
  );
}
