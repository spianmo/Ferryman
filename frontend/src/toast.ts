export type ToastKind = "success" | "error" | "info";

export type ToastEventDetail = {
  kind: ToastKind;
  message: string;
};

export const TOAST_EVENT = "ferryman:toast";

function emit(kind: ToastKind, message: string) {
  if (typeof window === "undefined") return;
  window.dispatchEvent(new CustomEvent<ToastEventDetail>(TOAST_EVENT, { detail: { kind, message } }));
}

export const toast = {
  success: (message: string) => emit("success", message),
  error: (message: string) => emit("error", message),
  info: (message: string) => emit("info", message),
};

