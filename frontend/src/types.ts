export type SessionInfo = {
  token: string;
  host: string;
  httpPort: number;
  wsPort: number;
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
