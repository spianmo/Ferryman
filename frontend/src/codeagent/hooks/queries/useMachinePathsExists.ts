import { useMemo } from 'react'
import { useQuery } from '@tanstack/react-query'
import type { ApiClient } from '@/api/client'

const EMPTY_EXISTS: Record<string, boolean> = {}

export function useMachinePathsExists(
    api: ApiClient,
    machineId: string | null,
    paths: string[]
): Record<string, boolean> {
    const normalizedPaths = useMemo(
        () => Array.from(new Set(paths))
            .sort((left, right) => left.localeCompare(right))
            .slice(0, 1000),
        [paths]
    )

    const query = useQuery({
        queryKey: ['machine-paths-exists', machineId ?? '', normalizedPaths] as const,
        queryFn: async () => {
            if (!machineId || normalizedPaths.length === 0) {
                return { exists: EMPTY_EXISTS }
            }
            return await api.checkMachinePathsExists(machineId, normalizedPaths)
        },
        enabled: Boolean(machineId && normalizedPaths.length > 0),
        staleTime: 60_000,
        gcTime: 5 * 60_000,
        retry: false,
        refetchOnWindowFocus: false,
        refetchOnMount: false,
    })

    return useMemo(() => {
        if (!machineId || normalizedPaths.length === 0) {
            return EMPTY_EXISTS
        }
        return query.data?.exists ?? EMPTY_EXISTS
    }, [machineId, normalizedPaths.length, query.data?.exists])
}
