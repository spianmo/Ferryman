import { describe, expect, it } from 'vitest'
import type { SessionSummary } from '@/types/api'
import {
    filterReadableSessions,
    isExternalSessionId,
    parseReadExternalSessions,
} from './external-sessions'

function makeSession(id: string): SessionSummary {
    return {
        id,
        active: true,
        thinking: false,
        activeAt: 0,
        updatedAt: 0,
        pendingRequestsCount: 0,
        modelMode: 'default',
        metadata: {
            path: '/tmp/project',
            host: 'localhost',
        },
        todoProgress: null,
    } as SessionSummary
}

describe('external session helpers', () => {
    it('detects external session ids', () => {
        expect(isExternalSessionId('external-claude-123')).toBe(true)
        expect(isExternalSessionId('session-123')).toBe(false)
    })

    it('defaults to reading external sessions', () => {
        expect(parseReadExternalSessions(null)).toBe(true)
        expect(parseReadExternalSessions('true')).toBe(true)
        expect(parseReadExternalSessions('false')).toBe(false)
    })

    it('filters external sessions when disabled', () => {
        const sessions = [makeSession('session-1'), makeSession('external-claude-1')]
        expect(filterReadableSessions(sessions, true)).toHaveLength(2)
        expect(filterReadableSessions(sessions, false).map((session) => session.id)).toEqual(['session-1'])
    })
})
