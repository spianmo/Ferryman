import { useCallback, useEffect, useState } from 'react'
import type { AuthSource } from './useAuth'

export function useAuthSource(_baseUrl: string): {
    authSource: AuthSource | null
    isLoading: boolean
    isTelegram: boolean
    setAccessToken: (token: string) => void
    clearAuth: () => void
} {
    const [authSource, setAuthSource] = useState<AuthSource | null>(null)
    const [isLoading, setIsLoading] = useState(true)

    useEffect(() => {
        setAuthSource({ type: 'session' })
        setIsLoading(false)
    }, [])

    const setAccessToken = useCallback((_token: string) => {
        setAuthSource({ type: 'session' })
    }, [])

    const clearAuth = useCallback(() => {
        setAuthSource({ type: 'session' })
    }, [])

    return {
        authSource,
        isLoading,
        isTelegram: false,
        setAccessToken,
        clearAuth
    }
}
