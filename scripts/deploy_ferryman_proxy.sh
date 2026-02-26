#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'EOF'
FerrymanProxy Linux one-click deploy script

Usage:
  ./scripts/deploy_ferryman_proxy.sh [options]

Options:
  --bin <path>                 FerrymanProxy binary path (default: ./build/FerrymanProxy)
  --install-dir <dir>          Install directory for binary (default: /usr/local/bin)
  --service-name <name>        systemd service name (default: ferryman-proxy)
  --service-user <user>        systemd service user (default: root)
  --service-group <group>      systemd service group (default: root)
  --state-dir <dir>            Working directory for service (default: /var/lib/ferryman-proxy)
  --bind <host>                Proxy bind host (default: 0.0.0.0)
  --control-port <port>        Proxy control port (default: 17000)
  --admin-host <host>          Admin listener host (default: 127.0.0.1)
  --admin-port <port>          Admin listener port (default: 17001)
  --log-file <path>            Proxy log file path (default: /var/log/ferryman-proxy.log)
  --extra-tcp-ports <list>     Comma-separated extra TCP ports to open in firewall
  --extra-udp-ports <list>     Comma-separated extra UDP ports to open in firewall
  --firewall-backend <name>    auto|ufw|firewalld|iptables|none (default: auto)
  --no-firewall                Skip firewall changes
  --no-enable                  Do not enable/start service after install
  -h, --help                   Show help

Examples:
  ./scripts/deploy_ferryman_proxy.sh \
    --bin ./build/FerrymanProxy \
    --bind 0.0.0.0 \
    --control-port 17000 \
    --admin-host 127.0.0.1 \
    --admin-port 17001 \
    --extra-tcp-ports 18080,18443 \
    --extra-udp-ports 20000
EOF
}

if [[ "$(uname -s)" != "Linux" ]]; then
  echo "This deploy script only supports Linux."
  exit 1
fi

BIN_PATH="./build/FerrymanProxy"
INSTALL_DIR="/usr/local/bin"
SERVICE_NAME="ferryman-proxy"
SERVICE_USER="root"
SERVICE_GROUP="root"
STATE_DIR="/var/lib/ferryman-proxy"
BIND_HOST="0.0.0.0"
CONTROL_PORT="17000"
ADMIN_HOST="127.0.0.1"
ADMIN_PORT="17001"
LOG_FILE="/var/log/ferryman-proxy.log"
FIREWALL_BACKEND="auto"
OPEN_FIREWALL=1
ENABLE_SERVICE=1
EXTRA_TCP_PORTS=""
EXTRA_UDP_PORTS=""

while [[ $# -gt 0 ]]; do
  case "$1" in
    --bin)
      BIN_PATH="$2"
      shift 2
      ;;
    --install-dir)
      INSTALL_DIR="$2"
      shift 2
      ;;
    --service-name)
      SERVICE_NAME="$2"
      shift 2
      ;;
    --service-user)
      SERVICE_USER="$2"
      shift 2
      ;;
    --service-group)
      SERVICE_GROUP="$2"
      shift 2
      ;;
    --state-dir)
      STATE_DIR="$2"
      shift 2
      ;;
    --bind)
      BIND_HOST="$2"
      shift 2
      ;;
    --control-port)
      CONTROL_PORT="$2"
      shift 2
      ;;
    --admin-host)
      ADMIN_HOST="$2"
      shift 2
      ;;
    --admin-port)
      ADMIN_PORT="$2"
      shift 2
      ;;
    --log-file)
      LOG_FILE="$2"
      shift 2
      ;;
    --extra-tcp-ports)
      EXTRA_TCP_PORTS="$2"
      shift 2
      ;;
    --extra-udp-ports)
      EXTRA_UDP_PORTS="$2"
      shift 2
      ;;
    --firewall-backend)
      FIREWALL_BACKEND="$2"
      shift 2
      ;;
    --no-firewall)
      OPEN_FIREWALL=0
      shift
      ;;
    --no-enable)
      ENABLE_SERVICE=0
      shift
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "Unknown option: $1"
      usage
      exit 1
      ;;
  esac
done

validate_port() {
  local value="$1"
  if [[ ! "$value" =~ ^[0-9]+$ ]]; then
    return 1
  fi
  if (( value < 1 || value > 65535 )); then
    return 1
  fi
  return 0
}

if ! validate_port "$CONTROL_PORT"; then
  echo "Invalid --control-port: $CONTROL_PORT"
  exit 1
fi
if ! validate_port "$ADMIN_PORT"; then
  echo "Invalid --admin-port: $ADMIN_PORT"
  exit 1
fi

IFS=',' read -r -a EXTRA_TCP_ARRAY <<< "$EXTRA_TCP_PORTS"
for port in "${EXTRA_TCP_ARRAY[@]}"; do
  [[ -z "$port" ]] && continue
  if ! validate_port "$port"; then
    echo "Invalid TCP port in --extra-tcp-ports: $port"
    exit 1
  fi
done

IFS=',' read -r -a EXTRA_UDP_ARRAY <<< "$EXTRA_UDP_PORTS"
for port in "${EXTRA_UDP_ARRAY[@]}"; do
  [[ -z "$port" ]] && continue
  if ! validate_port "$port"; then
    echo "Invalid UDP port in --extra-udp-ports: $port"
    exit 1
  fi
done

if [[ ! -f "$BIN_PATH" ]]; then
  echo "FerrymanProxy binary not found: $BIN_PATH"
  exit 1
fi
if [[ ! -x "$BIN_PATH" ]]; then
  chmod +x "$BIN_PATH"
fi

SUDO=""
if [[ $EUID -ne 0 ]]; then
  if command -v sudo >/dev/null 2>&1; then
    SUDO="sudo"
  else
    echo "Root privileges are required. Re-run as root or install sudo."
    exit 1
  fi
fi

run_root() {
  if [[ -n "$SUDO" ]]; then
    "$SUDO" "$@"
  else
    "$@"
  fi
}

detect_firewall_backend() {
  local backend="$1"
  if [[ "$backend" != "auto" ]]; then
    echo "$backend"
    return
  fi
  if command -v ufw >/dev/null 2>&1; then
    if run_root ufw status 2>/dev/null | grep -q "Status: active"; then
      echo "ufw"
      return
    fi
  fi
  if command -v firewall-cmd >/dev/null 2>&1; then
    if run_root firewall-cmd --state >/dev/null 2>&1; then
      echo "firewalld"
      return
    fi
  fi
  if command -v iptables >/dev/null 2>&1; then
    echo "iptables"
    return
  fi
  echo "none"
}

open_firewall_port() {
  local backend="$1"
  local proto="$2"
  local port="$3"
  case "$backend" in
    ufw)
      run_root ufw allow "${port}/${proto}" >/dev/null
      ;;
    firewalld)
      run_root firewall-cmd --permanent --add-port="${port}/${proto}" >/dev/null
      ;;
    iptables)
      if ! run_root iptables -C INPUT -p "$proto" --dport "$port" -j ACCEPT >/dev/null 2>&1; then
        run_root iptables -I INPUT -p "$proto" --dport "$port" -j ACCEPT
      fi
      ;;
    none)
      ;;
    *)
      echo "Unsupported firewall backend: $backend"
      exit 1
      ;;
  esac
}

echo "Installing FerrymanProxy..."
run_root install -d "$INSTALL_DIR"
run_root install -m 0755 "$BIN_PATH" "$INSTALL_DIR/FerrymanProxy"
run_root install -d "$STATE_DIR"
run_root chown "$SERVICE_USER:$SERVICE_GROUP" "$STATE_DIR"
run_root install -d "$(dirname "$LOG_FILE")"
run_root touch "$LOG_FILE"
run_root chown "$SERVICE_USER:$SERVICE_GROUP" "$LOG_FILE"
run_root chmod 0644 "$LOG_FILE"

SERVICE_PATH="/etc/systemd/system/${SERVICE_NAME}.service"
TMP_SERVICE="$(mktemp)"
cat > "$TMP_SERVICE" <<EOF
[Unit]
Description=FerrymanProxy Reverse Port Mapping Service
After=network-online.target
Wants=network-online.target

[Service]
Type=simple
User=${SERVICE_USER}
Group=${SERVICE_GROUP}
WorkingDirectory=${STATE_DIR}
ExecStart=${INSTALL_DIR}/FerrymanProxy --bind ${BIND_HOST} --control-port ${CONTROL_PORT} --admin-host ${ADMIN_HOST} --admin-port ${ADMIN_PORT} --log-file ${LOG_FILE}
Restart=always
RestartSec=2
LimitNOFILE=1048576

[Install]
WantedBy=multi-user.target
EOF

run_root install -m 0644 "$TMP_SERVICE" "$SERVICE_PATH"
rm -f "$TMP_SERVICE"

echo "Reloading systemd and applying service..."
run_root systemctl daemon-reload
if [[ $ENABLE_SERVICE -eq 1 ]]; then
  run_root systemctl enable --now "$SERVICE_NAME"
else
  echo "Service installed but not started (--no-enable)."
fi

if [[ $OPEN_FIREWALL -eq 1 ]]; then
  FIREWALL_BACKEND="$(detect_firewall_backend "$FIREWALL_BACKEND")"
  echo "Configuring firewall backend: $FIREWALL_BACKEND"

  if [[ "$FIREWALL_BACKEND" != "none" ]]; then
    open_firewall_port "$FIREWALL_BACKEND" tcp "$CONTROL_PORT"
    if [[ "$ADMIN_HOST" == "0.0.0.0" || "$ADMIN_HOST" == "::" || "$ADMIN_HOST" == "*" ]]; then
      open_firewall_port "$FIREWALL_BACKEND" tcp "$ADMIN_PORT"
    fi
    for port in "${EXTRA_TCP_ARRAY[@]}"; do
      [[ -z "$port" ]] && continue
      open_firewall_port "$FIREWALL_BACKEND" tcp "$port"
    done
    for port in "${EXTRA_UDP_ARRAY[@]}"; do
      [[ -z "$port" ]] && continue
      open_firewall_port "$FIREWALL_BACKEND" udp "$port"
    done

    if [[ "$FIREWALL_BACKEND" == "firewalld" ]]; then
      run_root firewall-cmd --reload >/dev/null
    fi
  else
    echo "No supported firewall backend detected; skipped firewall changes."
  fi
fi

echo
echo "FerrymanProxy deployment completed."
echo "Binary:   ${INSTALL_DIR}/FerrymanProxy"
echo "Service:  ${SERVICE_NAME} (${SERVICE_PATH})"
echo "Control:  ${BIND_HOST}:${CONTROL_PORT}"
echo "Admin:    ${ADMIN_HOST}:${ADMIN_PORT}"
echo "Log file: ${LOG_FILE}"
echo
echo "Useful commands:"
echo "  systemctl status ${SERVICE_NAME}"
echo "  journalctl -u ${SERVICE_NAME} -f"
echo "  ${INSTALL_DIR}/FerrymanProxy --status --admin-host ${ADMIN_HOST} --admin-port ${ADMIN_PORT}"
echo "  ${INSTALL_DIR}/FerrymanProxy --list --admin-host ${ADMIN_HOST} --admin-port ${ADMIN_PORT}"
