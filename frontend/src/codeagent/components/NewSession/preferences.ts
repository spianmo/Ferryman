import type { AgentType } from './types'
import {
    loadPreferredPermissionChoice,
    savePreferredPermissionChoice,
} from './permissionChoices'

const AGENT_STORAGE_KEY = 'hapi:newSession:agent'

const VALID_AGENTS: AgentType[] = ['claude', 'codex', 'cursor', 'gemini', 'opencode']

export function loadPreferredAgent(): AgentType {
    try {
        const stored = localStorage.getItem(AGENT_STORAGE_KEY)
        if (stored && VALID_AGENTS.includes(stored as AgentType)) {
            return stored as AgentType
        }
    } catch {
        // Ignore storage errors
    }
    return 'claude'
}

export function savePreferredAgent(agent: AgentType): void {
    try {
        localStorage.setItem(AGENT_STORAGE_KEY, agent)
    } catch {
        // Ignore storage errors
    }
}

export { loadPreferredPermissionChoice, savePreferredPermissionChoice }
