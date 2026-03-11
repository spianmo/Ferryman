import { useCallback, useEffect, useState } from 'react'
import {
    READ_EXTERNAL_SESSIONS_STORAGE_KEY,
    getStoredReadExternalSessions,
    setStoredReadExternalSessions,
} from '@/lib/external-sessions'

export function useReadExternalSessions(): {
    readExternalSessions: boolean
    setReadExternalSessions: (enabled: boolean) => void
} {
    const [readExternalSessions, setReadExternalSessionsState] = useState<boolean>(getStoredReadExternalSessions)

    useEffect(() => {
        if (typeof window === 'undefined') {
            return
        }

        const onStorage = (event: StorageEvent) => {
            if (event.key !== READ_EXTERNAL_SESSIONS_STORAGE_KEY) {
                return
            }
            setReadExternalSessionsState(getStoredReadExternalSessions())
        }

        window.addEventListener('storage', onStorage)
        return () => window.removeEventListener('storage', onStorage)
    }, [])

    const setReadExternalSessions = useCallback((enabled: boolean) => {
        setReadExternalSessionsState(enabled)
        setStoredReadExternalSessions(enabled)
    }, [])

    return { readExternalSessions, setReadExternalSessions }
}
