import { useCallback, useEffect, useRef, useState } from 'react'
import { emitCodeAgentUnauthorized, normalizeUnauthorizedReason } from '@/lib/unauthorized'

type TerminalConnectionState =
    | { status: 'idle' }
    | { status: 'connecting' }
    | { status: 'connected' }
    | { status: 'error'; error: string }

type UseTerminalSocketOptions = {
    baseUrl: string
    token: string
    sessionId: string
    terminalId: string
}

type TerminalWsPayload = {
    ok?: boolean
    event?: string
    terminal_id?: string
    data?: string
    error?: string
    code?: string
}

const textEncoder = new TextEncoder()
const textDecoder = new TextDecoder()

function buildTerminalWsUrl(baseUrl: string, token: string): string {
    const fallbackBase = typeof window !== 'undefined' ? window.location.origin : 'http://127.0.0.1'
    const url = new URL('/ws/codeagent/terminal', baseUrl || fallbackBase)
    if (url.protocol === 'https:') {
        url.protocol = 'wss:'
    } else if (url.protocol === 'http:') {
        url.protocol = 'ws:'
    }
    const trimmedToken = token.trim()
    if (trimmedToken) {
        url.searchParams.set('token', trimmedToken)
    }
    return url.toString()
}

function encodeBase64Utf8(value: string): string {
    const bytes = textEncoder.encode(value)
    let binary = ''
    for (const byte of bytes) {
        binary += String.fromCharCode(byte)
    }
    return btoa(binary)
}

function decodeBase64Utf8(value: string): string {
    try {
        const binary = atob(value)
        const bytes = new Uint8Array(binary.length)
        for (let i = 0; i < binary.length; i += 1) {
            bytes[i] = binary.charCodeAt(i)
        }
        return textDecoder.decode(bytes)
    } catch {
        return ''
    }
}

export function useTerminalSocket(options: UseTerminalSocketOptions): {
    state: TerminalConnectionState
    connect: (cols: number, rows: number) => void
    write: (data: string) => void
    resize: (cols: number, rows: number) => void
    disconnect: () => void
    onOutput: (handler: (data: string) => void) => void
    onExit: (handler: (code: number | null, signal: string | null) => void) => void
} {
    const [state, setState] = useState<TerminalConnectionState>({ status: 'idle' })
    const socketRef = useRef<WebSocket | null>(null)
    const outputHandlerRef = useRef<(data: string) => void>(() => {})
    const exitHandlerRef = useRef<(code: number | null, signal: string | null) => void>(() => {})
    const sessionIdRef = useRef(options.sessionId)
    const terminalIdRef = useRef(options.terminalId)
    const baseUrlRef = useRef(options.baseUrl)
    const tokenRef = useRef(options.token)
    const connectedTerminalIdRef = useRef<string>('')
    const pendingWritesRef = useRef<string[]>([])
    const pendingResizeRef = useRef<{ cols: number; rows: number } | null>(null)
    const manualDisconnectRef = useRef(false)
    const lastSizeRef = useRef<{ cols: number; rows: number } | null>(null)

    useEffect(() => {
        sessionIdRef.current = options.sessionId
        terminalIdRef.current = options.terminalId
        baseUrlRef.current = options.baseUrl
        tokenRef.current = options.token
    }, [options.sessionId, options.terminalId, options.baseUrl, options.token])

    const setErrorState = useCallback((message: string) => {
        setState({ status: 'error', error: message })
    }, [])

    const sendJson = useCallback((payload: Record<string, unknown>) => {
        const socket = socketRef.current
        if (!socket || socket.readyState !== WebSocket.OPEN) {
            return false
        }
        socket.send(JSON.stringify(payload))
        return true
    }, [])

    const flushPendingWrites = useCallback(() => {
        const terminalId = connectedTerminalIdRef.current
        if (!terminalId || pendingWritesRef.current.length === 0) {
            return
        }
        const queued = pendingWritesRef.current
        pendingWritesRef.current = []
        for (const chunk of queued) {
            sendJson({
                action: 'input',
                terminal_id: terminalId,
                data: encodeBase64Utf8(chunk)
            })
        }
    }, [sendJson])

    const flushPendingResize = useCallback(() => {
        const terminalId = connectedTerminalIdRef.current
        const size = pendingResizeRef.current
        if (!terminalId || !size) {
            return
        }
        pendingResizeRef.current = null
        sendJson({
            action: 'resize',
            terminal_id: terminalId,
            cols: size.cols,
            rows: size.rows
        })
    }, [sendJson])

    const handleMessage = useCallback((event: MessageEvent<string>) => {
        let payload: TerminalWsPayload
        try {
            payload = JSON.parse(event.data) as TerminalWsPayload
        } catch {
            return
        }

        if (payload.ok === false) {
            if (payload.code === 'unauthorized') {
                emitCodeAgentUnauthorized({
                    reason: normalizeUnauthorizedReason(payload.error),
                    path: '/ws/codeagent/terminal'
                })
                const socket = socketRef.current
                if (socket) {
                    socket.onclose = null
                    socket.onerror = null
                    socket.close()
                    socketRef.current = null
                }
            }
            setErrorState(payload.error || 'Terminal request failed.')
            return
        }

        if (payload.event === 'terminal_open') {
            const terminalId = payload.terminal_id || ''
            if (!terminalId) {
                setErrorState('Terminal did not return terminal id.')
                return
            }
            connectedTerminalIdRef.current = terminalId
            setState({ status: 'connected' })
            flushPendingResize()
            flushPendingWrites()
            return
        }

        if (payload.event === 'terminal_output') {
            if (payload.terminal_id && connectedTerminalIdRef.current && payload.terminal_id !== connectedTerminalIdRef.current) {
                return
            }
            const output = typeof payload.data === 'string' ? decodeBase64Utf8(payload.data) : ''
            if (output) {
                outputHandlerRef.current(output)
            }
            return
        }

        if (payload.event === 'terminal_closed') {
            if (payload.terminal_id && connectedTerminalIdRef.current && payload.terminal_id !== connectedTerminalIdRef.current) {
                return
            }
            connectedTerminalIdRef.current = ''
            exitHandlerRef.current(null, null)
            setErrorState('Terminal exited.')
        }
    }, [flushPendingResize, flushPendingWrites, setErrorState])

    const connect = useCallback((cols: number, rows: number) => {
        lastSizeRef.current = { cols, rows }
        pendingResizeRef.current = { cols, rows }
        const sessionId = sessionIdRef.current
        const terminalId = terminalIdRef.current
        if (!sessionId || !terminalId) {
            setErrorState('Missing terminal credentials.')
            return
        }

        const openPayload = {
            action: 'open',
            session_id: sessionId,
            terminal_id: terminalId,
            cols,
            rows
        }

        const socket = socketRef.current
        if (socket && socket.readyState === WebSocket.OPEN) {
            connectedTerminalIdRef.current = ''
            sendJson(openPayload)
            setState({ status: 'connecting' })
            return
        }

        if (socket && socket.readyState === WebSocket.CONNECTING) {
            setState({ status: 'connecting' })
            return
        }

        manualDisconnectRef.current = false
        connectedTerminalIdRef.current = ''
        const ws = new WebSocket(buildTerminalWsUrl(baseUrlRef.current, tokenRef.current))
        socketRef.current = ws
        setState({ status: 'connecting' })

        ws.onopen = () => {
            const size = lastSizeRef.current ?? { cols, rows }
            ws.send(JSON.stringify({ ...openPayload, cols: size.cols, rows: size.rows }))
        }

        ws.onmessage = (evt) => {
            if (typeof evt.data !== 'string') {
                return
            }
            handleMessage(evt as MessageEvent<string>)
        }

        ws.onerror = () => {
            setErrorState('Connection error')
        }

        ws.onclose = (evt) => {
            socketRef.current = null
            connectedTerminalIdRef.current = ''
            if (manualDisconnectRef.current) {
                setState({ status: 'idle' })
                return
            }
            const reason = evt.reason || `code ${evt.code}`
            setErrorState(`Disconnected: ${reason}`)
        }
    }, [handleMessage, sendJson, setErrorState])

    const write = useCallback((data: string) => {
        if (!data) {
            return
        }
        const terminalId = connectedTerminalIdRef.current
        if (terminalId) {
            const sent = sendJson({
                action: 'input',
                terminal_id: terminalId,
                data: encodeBase64Utf8(data)
            })
            if (sent) {
                return
            }
        }
        pendingWritesRef.current.push(data)
    }, [sendJson])

    const resize = useCallback((cols: number, rows: number) => {
        lastSizeRef.current = { cols, rows }
        pendingResizeRef.current = { cols, rows }
        const terminalId = connectedTerminalIdRef.current
        if (!terminalId) {
            return
        }
        const sent = sendJson({
            action: 'resize',
            terminal_id: terminalId,
            cols,
            rows
        })
        if (sent) {
            pendingResizeRef.current = null
        }
    }, [sendJson])

    const disconnect = useCallback(() => {
        const socket = socketRef.current
        manualDisconnectRef.current = true
        pendingWritesRef.current = []
        pendingResizeRef.current = null

        if (socket && socket.readyState === WebSocket.OPEN) {
            const terminalId = connectedTerminalIdRef.current
            if (terminalId) {
                socket.send(JSON.stringify({
                    action: 'close',
                    terminal_id: terminalId
                }))
            }
        }

        connectedTerminalIdRef.current = ''
        if (socket) {
            socket.onopen = null
            socket.onmessage = null
            socket.onerror = null
            socket.onclose = null
            socket.close()
        }
        socketRef.current = null
        setState({ status: 'idle' })
    }, [])

    useEffect(() => {
        const socket = socketRef.current
        if (!socket) {
            return
        }
        if (socket.readyState === WebSocket.OPEN || socket.readyState === WebSocket.CONNECTING) {
            const size = lastSizeRef.current
            disconnect()
            if (size) {
                connect(size.cols, size.rows)
            }
        }
    }, [connect, disconnect])

    useEffect(() => {
        return () => {
            disconnect()
        }
    }, [disconnect])

    const onOutput = useCallback((handler: (data: string) => void) => {
        outputHandlerRef.current = handler
    }, [])

    const onExit = useCallback((handler: (code: number | null, signal: string | null) => void) => {
        exitHandlerRef.current = handler
    }, [])

    return {
        state,
        connect,
        write,
        resize,
        disconnect,
        onOutput,
        onExit
    }
}
