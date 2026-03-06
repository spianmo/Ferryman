export const CODEAGENT_ROOT_ID = 'ferryman-codeagent-root'
export const CODEAGENT_PORTAL_ROOT_ID = 'ferryman-codeagent-portal-root'

export function getCodeAgentRootElement(): HTMLElement | null {
    if (typeof document === 'undefined') {
        return null
    }
    return document.getElementById(CODEAGENT_ROOT_ID)
}

export function getCodeAgentPortalElement(): HTMLElement | null {
    if (typeof document === 'undefined') {
        return null
    }
    return document.getElementById(CODEAGENT_PORTAL_ROOT_ID)
}
