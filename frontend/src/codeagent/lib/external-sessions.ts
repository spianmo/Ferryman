import type { SessionSummary } from '@/types/api'

export const READ_EXTERNAL_SESSIONS_STORAGE_KEY = 'hapi-codeagent-read-external-sessions'

export function isExternalSessionId(sessionId: string): boolean {
    return sessionId.startsWith('external-')
}

export function isExternalSessionSummary(session: Pick<SessionSummary, 'id'>): boolean {
    return isExternalSessionId(session.id)
}

export function parseReadExternalSessions(raw: string | null): boolean {
    return raw !== 'false'
}

export function getStoredReadExternalSessions(): boolean {
    if (typeof window === 'undefined') {
        return true
    }
    try {
        return parseReadExternalSessions(localStorage.getItem(READ_EXTERNAL_SESSIONS_STORAGE_KEY))
    } catch {
        return true
    }
}

export function setStoredReadExternalSessions(enabled: boolean): void {
    if (typeof window === 'undefined') {
        return
    }
    try {
        if (enabled) {
            localStorage.removeItem(READ_EXTERNAL_SESSIONS_STORAGE_KEY)
        } else {
            localStorage.setItem(READ_EXTERNAL_SESSIONS_STORAGE_KEY, 'false')
        }
    } catch {
        // Ignore storage errors
    }
}

export function filterReadableSessions(
    sessions: readonly SessionSummary[],
    readExternalSessions: boolean
): SessionSummary[] {
    if (readExternalSessions) {
        return [...sessions]
    }
    return sessions.filter((session) => !isExternalSessionSummary(session))
}
