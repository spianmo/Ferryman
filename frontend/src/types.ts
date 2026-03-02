export type SessionInfo = {
  token: string;
  host: string;
  httpPort: number;
  wsPort: number;
};

export type SessionEnvironment = {
  host_os: "linux" | "macos" | "windows" | "freebsd" | string;
  docker_installed: boolean;
  codeserver_installed: boolean;
  kvm_installed: boolean;
};

export type FileEntry = {
  name: string;
  path: string;
  is_directory: boolean;
  size: number;
  modified_at: number;
  permissions: string;
};

export type TaskInfo = {
  task_id: string;
  command: string;
  status: string;
  exit_code: number;
  created_at: string;
  updated_at: string;
};

export type ScreenSource = {
  id: string;
  name: string;
  width: number;
  height: number;
  is_default: boolean;
};

export type DockurrVmInfo = {
  id: string;
  name: string;
  os: "windows" | "macos" | string;
  image: string;
  state: string;
  running: boolean;
  ports: string;
  running_for: string;
  persistent: boolean;
  novnc_port: string;
  desktop_port: string;
};

export type DockerContainerInfo = {
  id: string;
  name: string;
  image: string;
  state: string;
  status: string;
  running_for: string;
  ports: string;
  created_at: string;
};

export type DockerContainerStats = {
  name: string;
  cpu_percent: number;
  memory_usage_bytes: number;
  memory_limit_bytes: number;
  memory_percent: number;
  net_input_bytes: number;
  net_output_bytes: number;
  block_input_bytes: number;
  block_output_bytes: number;
  pids: number;
};

export type DockerContainerProcesses = {
  name: string;
  columns: string[];
  rows: string[][];
};

export type DockerContainerFileEntry = {
  name: string;
  path: string;
  is_directory: boolean;
  size: number;
  modified_at: number;
  permissions: string;
};

export type MonitorSnapshot = {
  ts_ms: number;
  device: {
    name: string;
    model: string;
    configuration: string;
    os: string;
    architecture: string;
    cpu_model: string;
    gpu_model: string;
    logical_cores: number;
    physical_cores: number;
  };
  cpu: {
    base_frequency_mhz: number;
    frequency_mhz: number;
    total_load_percent: number;
    per_core_load_percent: number[];
  };
  gpu: {
    model: string;
    load_percent: number | null;
  };
  boot: {
    uptime_seconds: number;
    started_at_ms: number;
  };
  memory: {
    total_bytes: number;
    used_bytes: number;
    free_bytes: number;
    used_percent: number;
  };
  disk: {
    total_bytes: number;
    used_bytes: number;
    free_bytes: number;
    used_percent: number;
    volumes: Array<{
      id: string;
      name: string;
      mount: string;
      total_bytes: number;
      used_bytes: number;
      free_bytes: number;
      used_percent: number;
    }>;
  };
};

export type TunnelMappingState = {
  id: string;
  name: string;
  protocol: "tcp" | "udp" | string;
  local_host: string;
  local_port: number;
  remote_port: number;
  enabled: boolean;
  active: boolean;
  status: string;
  detail: string;
  updated_at: string;
};

export type TunnelState = {
  proxy_host: string;
  proxy_port: number;
  mappings: TunnelMappingState[];
};

export type ListeningPortInfo = {
  protocol: "tcp" | "udp" | string;
  address: string;
  port: number;
  process: string;
  pid: number;
};
