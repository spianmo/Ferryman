import { emitUnauthorized } from '../../api/client'

type UnauthorizedPayload = {
    ok?: unknown
    code?: unknown
    error?: unknown
}

const DEFAULT_UNAUTHORIZED_REASON = 'Session expired. Please sign in again.'

export function normalizeUnauthorizedReason(reason: unknown): string {
    if (typeof reason !== 'string') {
        return DEFAULT_UNAUTHORIZED_REASON
    }
    const trimmed = reason.trim()
    return trimmed || DEFAULT_UNAUTHORIZED_REASON
}

export function emitCodeAgentUnauthorized(detail?: {
    reason?: unknown
    status?: number
    path?: string
}): void {
    emitUnauthorized({
        reason: normalizeUnauthorizedReason(detail?.reason),
        status: detail?.status,
        path: detail?.path
    })
}

export function isUnauthorizedPayload(value: unknown): value is UnauthorizedPayload {
    if (typeof value !== 'object' || value === null) {
        return false
    }
    return (value as UnauthorizedPayload).code === 'unauthorized'
}
