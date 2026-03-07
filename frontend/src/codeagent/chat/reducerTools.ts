import type { AgentState } from '@/types/api'
import type { ChatBlock, ChatToolCall, NormalizedMessage, ToolCallBlock, ToolPermission } from '@/chat/types'

export type PermissionEntry = {
    toolName: string
    input: unknown
    permission: ToolPermission
}

function normalizeToolName(value: string): string {
    return value.trim().toLowerCase()
}

function isQuestionToolName(value: string): boolean {
    const normalized = normalizeToolName(value)
    return normalized === 'askuserquestion'
        || normalized === 'ask_user_question'
        || normalized === 'request_user_input'
        || normalized.endsWith('__ask_user_question')
        || normalized.endsWith('_ask_user_question')
        || normalized.endsWith('__request_user_input')
        || normalized.endsWith('_request_user_input')
}

function stableInputSignature(input: unknown): string {
    try {
        return JSON.stringify(input) ?? ''
    } catch {
        return ''
    }
}

export function getPermissions(agentState: AgentState | null | undefined): Map<string, PermissionEntry> {
    const map = new Map<string, PermissionEntry>()
    const dedupeBySignature = new Map<string, string>()
    const answeredQuestionCompletedAtBySignature = new Map<string, number | null>()

    const completed = agentState?.completedRequests ?? null
    if (completed) {
        for (const [id, entry] of Object.entries(completed)) {
            const answers = entry.answers
            const hasAnswers = Boolean(answers && Object.keys(answers).length > 0)
            const questionSignature = isQuestionToolName(entry.tool)
                ? `${normalizeToolName(entry.tool)}::${stableInputSignature(entry.arguments)}`
                : null

            if (questionSignature && entry.status === 'approved' && hasAnswers) {
                answeredQuestionCompletedAtBySignature.set(questionSignature, entry.completedAt ?? entry.createdAt ?? null)
            }

            const shouldCollapse = isQuestionToolName(entry.tool)
                && !hasAnswers
                && (entry.status === 'denied' || entry.status === 'canceled')

            const signature = shouldCollapse
                ? `${normalizeToolName(entry.tool)}::${stableInputSignature(entry.arguments)}`
                : null
            if (signature) {
                const previousId = dedupeBySignature.get(signature)
                if (previousId) {
                    map.delete(previousId)
                }
                dedupeBySignature.set(signature, id)
            }

            map.set(id, {
                toolName: entry.tool,
                input: entry.arguments,
                permission: {
                    id,
                    status: entry.status,
                    reason: entry.reason ?? undefined,
                    mode: entry.mode ?? undefined,
                    decision: entry.decision ?? undefined,
                    allowedTools: entry.allowTools,
                    answers: entry.answers,
                    createdAt: entry.createdAt ?? null,
                    completedAt: entry.completedAt ?? null
                }
            })
        }
    }

    const requests = agentState?.requests ?? null
    if (requests) {
        for (const [id, request] of Object.entries(requests)) {
            if (map.has(id)) continue

            const shouldCollapse = isQuestionToolName(request.tool)
            const signature = shouldCollapse
                ? `${normalizeToolName(request.tool)}::${stableInputSignature(request.arguments)}`
                : null
            if (signature) {
                const answeredAt = answeredQuestionCompletedAtBySignature.get(signature)
                const requestCreatedAt = request.createdAt ?? null
                const isRecentReplayDuplicate = answeredAt === null
                    || requestCreatedAt === null
                    || (requestCreatedAt >= answeredAt && requestCreatedAt - answeredAt <= 60_000)

                if (answeredQuestionCompletedAtBySignature.has(signature) && isRecentReplayDuplicate) {
                    continue
                }
            }
            if (signature) {
                const previousId = dedupeBySignature.get(signature)
                if (previousId) {
                    map.delete(previousId)
                }
                dedupeBySignature.set(signature, id)
            }

            map.set(id, {
                toolName: request.tool,
                input: request.arguments,
                permission: {
                    id,
                    status: 'pending',
                    createdAt: request.createdAt ?? null
                }
            })
        }
    }

    return map
}

export function ensureToolBlock(
    blocks: ChatBlock[],
    toolBlocksById: Map<string, ToolCallBlock>,
    id: string,
    seed: {
        createdAt: number
        localId: string | null
        meta?: unknown
        name: string
        input: unknown
        description: string | null
        permission?: ToolPermission
    }
): ToolCallBlock {
    const existing = toolBlocksById.get(id)
    if (existing) {
        const isPlaceholderToolName = (name: string): boolean => {
            const normalized = name.trim().toLowerCase()
            return normalized === '' || normalized === 'tool' || normalized === 'unknown'
        }

        // Preserve earliest createdAt for stable ordering.
        if (seed.createdAt < existing.createdAt) {
            existing.createdAt = seed.createdAt
            existing.tool.createdAt = seed.createdAt
        }
        if (seed.permission) {
            existing.tool.permission = { ...existing.tool.permission, ...seed.permission }
            if (seed.permission.status === 'pending' && existing.tool.state === 'running') {
                existing.tool.state = 'pending'
            } else if (seed.permission.status === 'approved') {
                existing.tool.state = 'completed'
                existing.tool.completedAt = seed.permission.completedAt ?? existing.tool.completedAt ?? seed.createdAt
            } else if (seed.permission.status === 'denied' || seed.permission.status === 'canceled') {
                existing.tool.state = 'error'
                existing.tool.completedAt = seed.permission.completedAt ?? existing.tool.completedAt ?? seed.createdAt
            }
        }
        if (seed.name && (!isPlaceholderToolName(seed.name) || isPlaceholderToolName(existing.tool.name))) {
            existing.tool.name = seed.name
        }
        if (seed.input !== null && seed.input !== undefined) {
            existing.tool.input = seed.input
        }
        if (seed.description !== null) {
            existing.tool.description = seed.description
        }
        return existing
    }

    const initialState: ChatToolCall['state'] = seed.permission?.status === 'pending'
        ? 'pending'
        : seed.permission?.status === 'approved'
            ? 'completed'
            : seed.permission?.status === 'denied' || seed.permission?.status === 'canceled'
                ? 'error'
                : 'running'

    const tool: ChatToolCall = {
        id,
        name: seed.name,
        state: initialState,
        input: seed.input,
        createdAt: seed.createdAt,
        startedAt: initialState === 'running' ? seed.createdAt : null,
        completedAt: initialState === 'completed' || initialState === 'error'
            ? (seed.permission?.completedAt ?? seed.createdAt)
            : null,
        description: seed.description,
        permission: seed.permission
    }

    const block: ToolCallBlock = {
        kind: 'tool-call',
        id,
        localId: seed.localId,
        createdAt: seed.createdAt,
        tool,
        children: [],
        meta: seed.meta
    }

    toolBlocksById.set(id, block)
    blocks.push(block)
    return block
}

export function collectToolIdsFromMessages(messages: NormalizedMessage[]): Set<string> {
    const ids = new Set<string>()
    for (const msg of messages) {
        if (msg.role !== 'agent') continue
        for (const content of msg.content) {
            if (content.type === 'tool-call') {
                ids.add(content.id)
            } else if (content.type === 'tool-result') {
                ids.add(content.tool_use_id)
            }
        }
    }
    return ids
}

export function isChangeTitleToolName(name: string): boolean {
    const normalized = name.trim().toLowerCase()
    if (!normalized) return false

    const withoutFunctionsPrefix = normalized.startsWith('functions.')
        ? normalized.slice('functions.'.length)
        : normalized

    if (
        withoutFunctionsPrefix === 'mcp__hapi__change_title'
        || withoutFunctionsPrefix === 'hapi__change_title'
        || withoutFunctionsPrefix === 'hapi_change_title'
        || withoutFunctionsPrefix === 'happy__change_title'
        || withoutFunctionsPrefix === 'change_title'
    ) {
        return true
    }

    return (
        withoutFunctionsPrefix.endsWith('__change_title')
        || withoutFunctionsPrefix.endsWith('_change_title')
    )
}

export function extractTitleFromChangeTitleInput(input: unknown): string | null {
    const extractFromRecord = (record: Record<string, unknown> | null | undefined): string | null => {
        if (!record) return null
        const candidates = ['title', 'name', 'new_title', 'session_title']
        for (const key of candidates) {
            const value = record[key]
            if (typeof value === 'string' && value.trim().length > 0) {
                return value.trim()
            }
        }
        return null
    }

    if (typeof input === 'string') {
        try {
            const parsed = JSON.parse(input) as unknown
            if (parsed && typeof parsed === 'object') {
                const fromParsed = extractFromRecord(parsed as Record<string, unknown>)
                if (fromParsed) return fromParsed
            }
        } catch {
            return null
        }
        return null
    }

    if (!input || typeof input !== 'object') return null
    const inputRecord = input as Record<string, unknown>

    const direct = extractFromRecord(inputRecord)
    if (direct) return direct

    const argumentsValue = inputRecord.arguments
    if (argumentsValue && typeof argumentsValue === 'object') {
        const nested = extractFromRecord(argumentsValue as Record<string, unknown>)
        if (nested) return nested
    }

    return null
}

export function collectTitleChanges(messages: NormalizedMessage[]): Map<string, string> {
    const map = new Map<string, string>()
    for (const msg of messages) {
        if (msg.role !== 'agent') continue
        for (const content of msg.content) {
            if (content.type !== 'tool-call') continue
            if (!isChangeTitleToolName(content.name)) continue
            const title = extractTitleFromChangeTitleInput(content.input)
            if (!title) continue
            map.set(content.id, title)
        }
    }
    return map
}
