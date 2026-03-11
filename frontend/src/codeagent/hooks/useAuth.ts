import { useCallback, useEffect, useMemo, useRef, useState } from 'react'
import { ApiClient } from '@/api/client'
import { UNAUTHORIZED_EVENT } from '../../api/client'
import type { AuthResponse } from '@/types/api'

const FERRYMAN_SESSION_KEY = 'ferryman.session'
const FERRYMAN_SESSION_COOKIE = 'ferryman_session_token'

export type AuthSource =
    | { type: 'session' }
    | { type: 'telegram'; initData: string }
    | { type: 'accessToken'; token: string }

function decodeJwtExpMs(token: string): number | null {
    const parts = token.split('.')
    if (parts.length < 2) return null

    const payloadBase64Url = parts[1] ?? ''
    const payloadBase64 = payloadBase64Url
        .replace(/-/g, '+')
        .replace(/_/g, '/')
        .padEnd(Math.ceil(payloadBase64Url.length / 4) * 4, '=')

    try {
        const decoded = globalThis.atob(payloadBase64)
        const payload = JSON.parse(decoded) as { exp?: unknown }
        if (typeof payload.exp !== 'number') return null
        return payload.exp * 1000
    } catch {
        return null
    }
}

function getEmbeddedUser(): AuthResponse['user'] {
    return {
        id: 1,
        username: 'ferryman',
        firstName: 'Ferryman',
        lastName: 'User'
    }
}

function readFerrymanSessionTokenFromCookie(): string | null {
    if (typeof document === 'undefined') {
        return null
    }
    const parts = document.cookie.split(';')
    for (const part of parts) {
        const trimmed = part.trim()
        if (!trimmed.startsWith(`${FERRYMAN_SESSION_COOKIE}=`)) {
            continue
        }
        const value = trimmed.slice(`${FERRYMAN_SESSION_COOKIE}=`.length).trim()
        if (!value) {
            return null
        }
        try {
            return decodeURIComponent(value)
        } catch {
            return value
        }
    }
    return null
}

function readFerrymanSessionTokenFromWindow(): string | null {
    if (typeof window === 'undefined') {
        return null
    }
    const token = window.__FERRYMAN_SESSION_TOKEN__
    if (typeof token !== 'string') {
        return null
    }
    const trimmed = token.trim()
    return trimmed ? trimmed : null
}

function readFerrymanSessionTokenFromStorage(): string | null {
    if (typeof window === 'undefined') {
        return null
    }
    try {
        const raw = window.localStorage.getItem(FERRYMAN_SESSION_KEY)
        if (!raw) {
            return null
        }
        const parsed = JSON.parse(raw) as { token?: unknown }
        return typeof parsed.token === 'string' && parsed.token.trim() ? parsed.token.trim() : null
    } catch {
        return null
    }
}

function resolveFerrymanSessionToken(): string | null {
    return readFerrymanSessionTokenFromWindow()
        ?? readFerrymanSessionTokenFromCookie()
        ?? readFerrymanSessionTokenFromStorage()
}

export function useAuth(authSource: AuthSource | null, baseUrl: string): {
    token: string | null
    user: AuthResponse['user'] | null
    api: ApiClient | null
    isLoading: boolean
    error: string | null
    needsBinding: boolean
    bind: (accessToken: string) => Promise<void>
} {
    const [token, setToken] = useState<string | null>(null)
    const [user, setUser] = useState<AuthResponse['user'] | null>(null)
    const [isLoading, setIsLoading] = useState<boolean>(false)
    const [error, setError] = useState<string | null>(null)
    const [needsBinding, setNeedsBinding] = useState<boolean>(false)
    const tokenRef = useRef<string | null>(null)
    const authSourceRef = useRef<AuthSource | null>(authSource)
    authSourceRef.current = authSource

    const applyAuthSource = useCallback((source: AuthSource | null): string | null => {
        if (!source) {
            tokenRef.current = null
            setToken(null)
            setUser(null)
            setError(null)
            setNeedsBinding(false)
            return null
        }

        if (source.type === 'session') {
            const ferrymanToken = resolveFerrymanSessionToken()
            tokenRef.current = ferrymanToken
            setToken(ferrymanToken)
            setUser(ferrymanToken ? getEmbeddedUser() : null)
            setError(ferrymanToken ? null : 'Ferryman session invalid or expired.')
            setNeedsBinding(false)
            return ferrymanToken
        }

        if (source.type === 'accessToken') {
            const directToken = source.token.trim()
            tokenRef.current = directToken || null
            setToken(directToken || null)
            setUser(directToken ? getEmbeddedUser() : null)
            setError(directToken ? null : 'Missing Ferryman session token.')
            setNeedsBinding(false)
            return directToken || null
        }

        tokenRef.current = null
        setToken(null)
        setUser(null)
        setNeedsBinding(false)
        setError('Telegram auth is not supported in Ferryman CodeAgent.')
        return null
    }, [])

    const refreshAuth = useCallback(async (): Promise<string | null> => {
        return applyAuthSource(authSourceRef.current)
    }, [applyAuthSource])

    const bind = useCallback(async (_accessToken: string) => {
        setError('Binding is not supported in Ferryman CodeAgent.')
        setNeedsBinding(false)
    }, [])

    const api = useMemo(() => {
        if (!token || !authSource) {
            return null
        }

        const sessionMode = authSource.type === 'session'
        return new ApiClient(token, {
            baseUrl,
            getToken: () => {
                if (!sessionMode) {
                    return tokenRef.current
                }
                const ferrymanToken = resolveFerrymanSessionToken()
                if (ferrymanToken) {
                    tokenRef.current = ferrymanToken
                }
                return ferrymanToken
            },
            onUnauthorized: () => refreshAuth(),
            authMode: 'session'
        })
    }, [authSource, baseUrl, refreshAuth, token])

    useEffect(() => {
        setIsLoading(true)
        setError(null)
        setNeedsBinding(false)
        applyAuthSource(authSource)
        setIsLoading(false)
    }, [applyAuthSource, authSource, baseUrl])

    useEffect(() => {
        const onUnauthorized = () => {
            tokenRef.current = null
            setToken(null)
            setUser(null)
            setNeedsBinding(false)
            setIsLoading(false)
            setError('Ferryman session invalid or expired.')
        }

        window.addEventListener(UNAUTHORIZED_EVENT, onUnauthorized as EventListener)
        return () => window.removeEventListener(UNAUTHORIZED_EVENT, onUnauthorized as EventListener)
    }, [])


    useEffect(() => {
        if (!token || !authSource || authSource.type !== 'accessToken') {
            return
        }

        const expMs = decodeJwtExpMs(token)
        if (!expMs) {
            return
        }

        let timeout: ReturnType<typeof setTimeout> | null = null
        let cancelled = false
        const delayMs = Math.max(0, expMs - 60_000 - Date.now())
        timeout = setTimeout(() => {
            if (!cancelled) {
                void refreshAuth()
            }
        }, delayMs)

        return () => {
            cancelled = true
            if (timeout) {
                clearTimeout(timeout)
            }
        }
    }, [authSource, refreshAuth, token])

    return { token, user, api, isLoading, error, needsBinding, bind }
}
