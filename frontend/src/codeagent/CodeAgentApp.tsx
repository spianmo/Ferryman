import { useCallback, useEffect, useMemo, useRef, useState } from 'react'
import { Outlet, useLocation, useMatchRoute, useRouter } from '@tanstack/react-router'
import { useQueryClient } from '@tanstack/react-query'
import { getTelegramWebApp, isTelegramApp } from '@/hooks/useTelegram'
import { initializeTheme } from '@/hooks/useTheme'
import { useAuth } from '@/hooks/useAuth'
import { useAuthSource } from '@/hooks/useAuthSource'
import { useServerUrl } from '@/hooks/useServerUrl'
import { useEventsSocket } from '@/hooks/useEventsSocket'
import { useSyncingState } from '@/hooks/useSyncingState'
import { usePushNotifications } from '@/hooks/usePushNotifications'
import { useVisibilityReporter } from '@/hooks/useVisibilityReporter'
import { queryKeys } from '@/lib/query-keys'
import { AppContextProvider } from '@/lib/app-context'
import { fetchLatestMessages } from '@/lib/message-window-store'
import { useAppGoBack } from '@/hooks/useAppGoBack'
import { useTranslation } from '@/lib/use-translation'
import { VoiceProvider } from '@/lib/voice-context'
import { InstallPrompt } from '@/components/InstallPrompt'
import { OfflineBanner } from '@/components/OfflineBanner'
import { SyncingBanner } from '@/components/SyncingBanner'
import { ReconnectingBanner } from '@/components/ReconnectingBanner'
import { VoiceErrorBanner } from '@/components/VoiceErrorBanner'
import { LoadingState } from '@/components/LoadingState'
import { ToastContainer } from '@/components/ToastContainer'
import { ToastProvider, useToast } from '@/lib/toast-context'
import type { SyncEvent } from '@/types/api'

type ToastEvent = Extract<SyncEvent, { type: 'toast' }>

export function CodeAgentApp() {
    return (
        <ToastProvider>
            <CodeAgentAppInner />
        </ToastProvider>
    )
}

function CodeAgentAppInner() {
    const { t } = useTranslation()
    const { baseUrl } = useServerUrl()
    const { authSource, isLoading: isAuthSourceLoading } = useAuthSource(baseUrl)
    const { token, api, isLoading: isAuthLoading, error: authError } = useAuth(authSource, baseUrl)
    const goBack = useAppGoBack()
    const pathname = useLocation({ select: (location) => location.pathname })
    const matchRoute = useMatchRoute()
    const router = useRouter()
    const { addToast } = useToast()

    useEffect(() => {
        const tg = getTelegramWebApp()
        tg?.ready()
        tg?.expand()
        initializeTheme()
    }, [])

    useEffect(() => {
        const preventDefault = (event: Event) => {
            event.preventDefault()
        }

        const onWheel = (event: WheelEvent) => {
            if (event.ctrlKey) {
                event.preventDefault()
            }
        }

        const onKeyDown = (event: KeyboardEvent) => {
            const modifier = event.ctrlKey || event.metaKey
            if (!modifier) return
            if (event.key === '+' || event.key === '-' || event.key === '=' || event.key === '0') {
                event.preventDefault()
            }
        }

        document.addEventListener('gesturestart', preventDefault as EventListener, { passive: false })
        document.addEventListener('gesturechange', preventDefault as EventListener, { passive: false })
        document.addEventListener('gestureend', preventDefault as EventListener, { passive: false })

        window.addEventListener('wheel', onWheel, { passive: false })
        window.addEventListener('keydown', onKeyDown)

        return () => {
            document.removeEventListener('gesturestart', preventDefault as EventListener)
            document.removeEventListener('gesturechange', preventDefault as EventListener)
            document.removeEventListener('gestureend', preventDefault as EventListener)

            window.removeEventListener('wheel', onWheel)
            window.removeEventListener('keydown', onKeyDown)
        }
    }, [])

    useEffect(() => {
        const tg = getTelegramWebApp()
        const backButton = tg?.BackButton
        if (!backButton) return

        if (pathname === '/' || pathname === '/sessions') {
            backButton.offClick(goBack)
            backButton.hide()
            return
        }

        backButton.show()
        backButton.onClick(goBack)
        return () => {
            backButton.offClick(goBack)
            backButton.hide()
        }
    }, [goBack, pathname])
    const queryClient = useQueryClient()
    const sessionMatch = matchRoute({ to: '/sessions/$sessionId' })
    const selectedSessionId = sessionMatch && sessionMatch.sessionId !== 'new' ? sessionMatch.sessionId : null
    const { isSyncing, startSync, endSync } = useSyncingState()
    const [eventsSocketDisconnected, setEventsSocketDisconnected] = useState(false)
    const [eventsSocketDisconnectReason, setEventsSocketDisconnectReason] = useState<string | null>(null)
    const syncTokenRef = useRef(0)
    const isFirstConnectRef = useRef(true)
    const baseUrlRef = useRef(baseUrl)
    const pushPromptedRef = useRef(false)
    const { isSupported: isPushSupported, permission: pushPermission, requestPermission, subscribe } = usePushNotifications(api)

    useEffect(() => {
        if (baseUrlRef.current === baseUrl) {
            return
        }
        baseUrlRef.current = baseUrl
        isFirstConnectRef.current = true
        syncTokenRef.current = 0
        queryClient.clear()
    }, [baseUrl, queryClient])

    // Clean up URL params after successful auth (for direct access links)
    useEffect(() => {
        if (!token || !api) return
        const { pathname, search, hash, state } = router.history.location
        const searchParams = new URLSearchParams(search)
        if (!searchParams.has('server') && !searchParams.has('hub') && !searchParams.has('token')) {
            return
        }
        searchParams.delete('server')
        searchParams.delete('hub')
        searchParams.delete('token')
        const nextSearch = searchParams.toString()
        const nextHref = `${pathname}${nextSearch ? `?${nextSearch}` : ''}${hash}`
        router.history.replace(nextHref, state)
    }, [token, api, router])

    useEffect(() => {
        if (!api || !token) {
            pushPromptedRef.current = false
            return
        }
        if (isTelegramApp() || !isPushSupported) {
            return
        }
        if (pushPromptedRef.current) {
            return
        }
        pushPromptedRef.current = true

        const run = async () => {
            if (pushPermission === 'granted') {
                await subscribe()
                return
            }
            if (pushPermission === 'default') {
                const granted = await requestPermission()
                if (granted) {
                    await subscribe()
                }
            }
        }

        void run()
    }, [api, isPushSupported, pushPermission, requestPermission, subscribe, token])

    const handleEventsSocketConnect = useCallback(() => {
        // Clear disconnected state on successful connection
        setEventsSocketDisconnected(false)
        setEventsSocketDisconnectReason(null)

        // Increment token to track this specific connection
        const token = ++syncTokenRef.current

        // Only force show banner on first connect (page load)
        // Subsequent connects (session switches) use non-forced mode
        // which only shows banner when returning from background
        if (isFirstConnectRef.current) {
            isFirstConnectRef.current = false
            startSync({ force: true })
        } else {
            startSync()
        }
        const invalidations = [
            queryClient.invalidateQueries({ queryKey: queryKeys.sessions }),
            ...(selectedSessionId ? [
                queryClient.invalidateQueries({ queryKey: queryKeys.session(selectedSessionId) })
            ] : [])
        ]
        const refreshMessages = (selectedSessionId && api)
            ? fetchLatestMessages(api, selectedSessionId)
            : Promise.resolve()
        Promise.all([...invalidations, refreshMessages])
            .catch((error) => {
                console.error('Failed to invalidate queries on events socket connect:', error)
            })
            .finally(() => {
                // Only end sync if this is still the latest connection
                if (syncTokenRef.current === token) {
                    endSync()
                }
            })
    }, [api, queryClient, selectedSessionId, startSync, endSync])

    const handleEventsSocketDisconnect = useCallback((reason: string) => {
        // Only show reconnecting banner if we've already connected once
        if (!isFirstConnectRef.current) {
            setEventsSocketDisconnected(true)
            setEventsSocketDisconnectReason(reason)
        }
    }, [])

    const handleEventsSocketEvent = useCallback(() => {}, [])
    const handleToast = useCallback((event: ToastEvent) => {
        addToast({
            title: event.data.title,
            body: event.data.body,
            sessionId: event.data.sessionId,
            url: event.data.url
        })

        const title = event.data.title ?? ''
        const isPermissionToast = /permission required|input needed/i.test(title)

        if (
            typeof window !== 'undefined'
            && 'Notification' in window
            && Notification.permission === 'granted'
            && typeof document !== 'undefined'
            && (document.visibilityState !== 'visible' || isPermissionToast)
        ) {
            try {
                const notification = new Notification(event.data.title, {
                    body: event.data.body,
                    tag: event.data.sessionId ? `permission:${event.data.sessionId}` : undefined
                })
                notification.onclick = () => {
                    window.focus()
                    if (event.data.sessionId) {
                        void router.navigate({
                            to: '/sessions/$sessionId',
                            params: { sessionId: event.data.sessionId }
                        })
                    }
                }
            } catch (error) {
                console.error('Failed to show browser notification:', error)
            }
        }
    }, [addToast, router])

    const eventSubscription = useMemo(() => ({ all: true }), [])

    const { subscriptionId } = useEventsSocket({
        enabled: Boolean(api && token),
        baseUrl,
        token: token ?? undefined,
        subscription: eventSubscription,
        onConnect: handleEventsSocketConnect,
        onDisconnect: handleEventsSocketDisconnect,
        onEvent: handleEventsSocketEvent,
        onToast: handleToast
    })

    useVisibilityReporter({
        api,
        subscriptionId,
        enabled: Boolean(api && token)
    })

    // Loading auth source
    if (isAuthSourceLoading) {
        return (
            <div className="h-full flex items-center justify-center p-4">
                <LoadingState label={t('loading')} className="text-sm" />
            </div>
        )
    }

    // Waiting for auth source resolution
    if (!authSource) {
        return (
            <div className="h-full flex items-center justify-center p-4">
                <LoadingState label={t('authorizing')} className="text-sm" />
            </div>
        )
    }

    // Authenticating (also covers the gap before useAuth effect starts)
    if (isAuthLoading || (authSource && !token && !authError)) {
        return (
            <div className="h-full flex items-center justify-center p-4">
                <LoadingState label={t('authorizing')} className="text-sm" />
            </div>
        )
    }

    // Auth error
    if (authError || !token || !api) {
        return (
            <div className="p-4 space-y-3">
                <div className="text-base font-semibold">CodeAgent</div>
                <div className="text-sm text-red-600">
                    {authError ?? 'Ferryman session invalid or expired.'}
                </div>
                <div className="text-xs text-[var(--app-hint)]">
                    Please login in Ferryman panel and reopen the CodeAgent tab.
                </div>
            </div>
        )
    }

    return (
        <AppContextProvider value={{ api, token, baseUrl }}>
            <VoiceProvider>
                <SyncingBanner isSyncing={isSyncing} />
                <ReconnectingBanner
                    isReconnecting={eventsSocketDisconnected && !isSyncing}
                    reason={eventsSocketDisconnectReason}
                />
                <VoiceErrorBanner />
                <OfflineBanner />
                <div className="h-full flex flex-col">
                    <Outlet />
                </div>
                <ToastContainer />
                <InstallPrompt />
            </VoiceProvider>
        </AppContextProvider>
    )
}
