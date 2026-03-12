import { useEffect, useMemo, useState } from 'react'
import { useQuery } from '@tanstack/react-query'
import type { ApiClient } from '@/api/client'

const DIRECTORY_CHECK_DEBOUNCE_MS = 500

export function useMachinePathExists(
    api: ApiClient,
    machineId: string | null,
    path: string
): {
    exists: boolean | null
    isChecking: boolean
    error: string | null
} {
    const normalizedPath = path.trim()
    const [debouncedPath, setDebouncedPath] = useState(normalizedPath)

    useEffect(() => {
        if (!normalizedPath) {
            setDebouncedPath('')
            return
        }

        const timer = window.setTimeout(() => {
            setDebouncedPath(normalizedPath)
        }, DIRECTORY_CHECK_DEBOUNCE_MS)

        return () => {
            window.clearTimeout(timer)
        }
    }, [normalizedPath])

    const query = useQuery({
        queryKey: ['machine-path-exists', machineId ?? '', debouncedPath] as const,
        queryFn: async () => {
            if (!machineId || !debouncedPath) {
                return { exists: {} as Record<string, boolean> }
            }
            return await api.checkMachinePathsExists(machineId, [debouncedPath])
        },
        enabled: Boolean(machineId && debouncedPath),
        staleTime: 30_000,
        gcTime: 5 * 60_000,
        retry: false,
        refetchOnWindowFocus: false,
        refetchOnMount: false,
    })

    const exists = useMemo(() => {
        if (!machineId || !normalizedPath || normalizedPath !== debouncedPath) {
            return null
        }
        if (!query.data) {
            return null
        }
        return Boolean(query.data.exists?.[debouncedPath])
    }, [debouncedPath, machineId, normalizedPath, query.data])

    const isChecking = Boolean(machineId && normalizedPath)
        && (normalizedPath !== debouncedPath || query.isFetching)

    return {
        exists,
        isChecking,
        error: normalizedPath === debouncedPath
            ? (query.error instanceof Error ? query.error.message : query.error ? 'Failed to validate directory' : null)
            : null,
    }
}
