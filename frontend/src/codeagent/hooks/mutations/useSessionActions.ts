import { useMutation, useQueryClient } from '@tanstack/react-query'
import { isPermissionModeAllowedForFlavor, isReasoningEffortAllowedForFlavor } from '@hapi/protocol'
import type { ApiClient } from '@/api/client'
import type { ModelMode, PermissionMode, ReasoningEffort, Session, SessionResponse, SessionsResponse } from '@/types/api'
import { queryKeys } from '@/lib/query-keys'
import { clearMessageWindow } from '@/lib/message-window-store'
import { isKnownFlavor } from '@/lib/agentFlavorUtils'

export function useSessionActions(
    api: ApiClient | null,
    sessionId: string | null,
    agentFlavor?: string | null
): {
    abortSession: () => Promise<void>
    archiveSession: () => Promise<void>
    setPermissionMode: (mode: PermissionMode) => Promise<void>
    setModelMode: (mode: ModelMode) => Promise<void>
    setReasoningEffort: (effort: ReasoningEffort) => Promise<void>
    renameSession: (name: string) => Promise<void>
    deleteSession: () => Promise<void>
    isPending: boolean
} {
    const queryClient = useQueryClient()

    const invalidateSession = async () => {
        if (!sessionId) return
        await queryClient.invalidateQueries({ queryKey: queryKeys.session(sessionId) })
        await queryClient.invalidateQueries({ queryKey: queryKeys.sessions })
    }

    const applySessionPatch = (patch: Partial<Pick<Session, 'permissionMode' | 'modelMode' | 'reasoningEffort'>>) => {
        if (!sessionId) return
        if (Object.keys(patch).length === 0) return

        queryClient.setQueryData<SessionResponse | undefined>(queryKeys.session(sessionId), (previous) => {
            if (!previous?.session) {
                return previous
            }
            return {
                ...previous,
                session: {
                    ...previous.session,
                    ...patch
                }
            }
        })

        queryClient.setQueryData<SessionsResponse | undefined>(queryKeys.sessions, (previous) => {
            if (!previous) {
                return previous
            }

            const nextSessions = previous.sessions.slice()
            const index = nextSessions.findIndex((session) => session.id === sessionId)
            if (index < 0) {
                return previous
            }

            const current = nextSessions[index]
            if (!current) {
                return previous
            }

            nextSessions[index] = {
                ...current,
                modelMode: patch.modelMode ?? current.modelMode,
                reasoningEffort: patch.reasoningEffort ?? current.reasoningEffort
            }
            return { ...previous, sessions: nextSessions }
        })
    }

    const abortMutation = useMutation({
        mutationFn: async () => {
            if (!api || !sessionId) {
                throw new Error('Session unavailable')
            }
            await api.abortSession(sessionId)
        },
        onSuccess: () => void invalidateSession(),
    })

    const archiveMutation = useMutation({
        mutationFn: async () => {
            if (!api || !sessionId) {
                throw new Error('Session unavailable')
            }
            await api.archiveSession(sessionId)
        },
        onSuccess: () => void invalidateSession(),
    })

    const permissionMutation = useMutation({
        mutationFn: async (mode: PermissionMode) => {
            if (!api || !sessionId) {
                throw new Error('Session unavailable')
            }
            if (isKnownFlavor(agentFlavor) && !isPermissionModeAllowedForFlavor(mode, agentFlavor)) {
                throw new Error('Invalid permission mode for session flavor')
            }
            return await api.setPermissionMode(sessionId, mode)
        },
        onSuccess: (applied, mode) => {
            applySessionPatch({ permissionMode: applied.permissionMode ?? mode })
            void invalidateSession()
        },
    })

    const modelMutation = useMutation({
        mutationFn: async (mode: ModelMode) => {
            if (!api || !sessionId) {
                throw new Error('Session unavailable')
            }
            return await api.setModelMode(sessionId, mode)
        },
        onSuccess: (applied, mode) => {
            applySessionPatch({ modelMode: applied.modelMode ?? mode })
            void invalidateSession()
        },
    })

    const reasoningEffortMutation = useMutation({
        mutationFn: async (effort: ReasoningEffort) => {
            if (!api || !sessionId) {
                throw new Error('Session unavailable')
            }
            if (!isReasoningEffortAllowedForFlavor(effort, agentFlavor)) {
                throw new Error('Invalid reasoning effort for session flavor')
            }
            await api.setReasoningEffort(sessionId, effort)
            return effort
        },
        onSuccess: (effort) => {
            applySessionPatch({ reasoningEffort: effort })
            void invalidateSession()
        },
    })

    const renameMutation = useMutation({
        mutationFn: async (name: string) => {
            if (!api || !sessionId) {
                throw new Error('Session unavailable')
            }
            await api.renameSession(sessionId, name)
        },
        onSuccess: () => void invalidateSession(),
    })

    const deleteMutation = useMutation({
        mutationFn: async () => {
            if (!api || !sessionId) {
                throw new Error('Session unavailable')
            }
            await api.deleteSession(sessionId)
        },
        onSuccess: async () => {
            if (!sessionId) return
            queryClient.removeQueries({ queryKey: queryKeys.session(sessionId) })
            clearMessageWindow(sessionId)
            await queryClient.invalidateQueries({ queryKey: queryKeys.sessions })
        },
    })

    return {
        abortSession: abortMutation.mutateAsync,
        archiveSession: archiveMutation.mutateAsync,
        setPermissionMode: async (mode: PermissionMode) => {
            await permissionMutation.mutateAsync(mode)
        },
        setModelMode: async (mode: ModelMode) => {
            await modelMutation.mutateAsync(mode)
        },
        setReasoningEffort: async (effort: ReasoningEffort) => {
            await reasoningEffortMutation.mutateAsync(effort)
        },
        renameSession: renameMutation.mutateAsync,
        deleteSession: deleteMutation.mutateAsync,
        isPending: abortMutation.isPending
            || archiveMutation.isPending
            || permissionMutation.isPending
            || modelMutation.isPending
            || reasoningEffortMutation.isPending
            || renameMutation.isPending
            || deleteMutation.isPending,
    }
}
