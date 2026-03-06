export {}

declare global {
  interface Window {
    __FERRYMAN_CODEAGENT_EMBEDDED__?: boolean
    __FERRYMAN_SESSION_TOKEN__?: string
  }
}
