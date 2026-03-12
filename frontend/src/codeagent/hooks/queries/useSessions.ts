import { useEffect, useMemo, useRef } from 'react'
import { useQuery } from '@tanstack/react-query'
import type { ApiClient } from '@/api/client'
import type { SessionSummary } from '@/types/api'
import { queryKeys } from '@/lib/query-keys'
import { filterReadableSessions } from '@/lib/external-sessions'
import { useReadExternalSessions } from '@/hooks/useReadExternalSessions'

const EMPTY_SESSIONS: SessionSummary[] = []

export function useSessions(api: ApiClient | null): {
    sessions: SessionSummary[]
    isLoading: boolean
    error: string | null
    refetch: () => Promise<unknown>
} {
    const { readExternalSessions } = useReadExternalSessions()
    const didMountRef = useRef(false)
    const refetchRef = useRef<() => Promise<unknown>>(async () => undefined)
    const query = useQuery({
        queryKey: queryKeys.sessions,
        queryFn: async () => {
            if (!api) {
                throw new Error('API unavailable')
            }
            return await api.getSessions({ includeExternal: readExternalSessions })
        },
        enabled: Boolean(api),
    })
    const refetch = query.refetch

    useEffect(() => {
        refetchRef.current = query.refetch
    }, [query.refetch])

    const sessions = useMemo(
        () => filterReadableSessions(query.data?.sessions ?? EMPTY_SESSIONS, readExternalSessions),
        [query.data?.sessions, readExternalSessions]
    )

    useEffect(() => {
        if (!didMountRef.current) {
            didMountRef.current = true
            return
        }
        void refetchRef.current()
    }, [readExternalSessions])

    return {
        sessions,
        isLoading: query.isLoading,
        error: query.error instanceof Error ? query.error.message : query.error ? 'Failed to load sessions' : null,
        refetch,
    }
}
