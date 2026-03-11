import type { AgentEvent, NormalizedAgentContent, NormalizedMessage, ToolResultPermission, UsageData } from '@/chat/types'
import { asNumber, asString, isObject } from '@hapi/protocol'

function normalizeToolResultPermissions(value: unknown): ToolResultPermission | undefined {
    if (!isObject(value)) return undefined
    const date = asNumber(value.date)
    const result = value.result
    if (date === null) return undefined
    if (result !== 'approved' && result !== 'denied') return undefined

    const mode = asString(value.mode) ?? undefined
    const allowedTools = Array.isArray(value.allowedTools)
        ? value.allowedTools.filter((tool) => typeof tool === 'string')
        : undefined
    const decision = value.decision
    const normalizedDecision = decision === 'approved' || decision === 'approved_for_session' || decision === 'denied' || decision === 'abort'
        ? decision
        : undefined

    return {
        date,
        result,
        mode,
        allowedTools,
        decision: normalizedDecision
    }
}

function normalizeAgentEvent(value: unknown): AgentEvent | null {
    if (!isObject(value) || typeof value.type !== 'string') return null
    return value as AgentEvent
}

function normalizeUsageData(value: unknown): UsageData | null {
    if (!isObject(value)) return null

    const usageSource: Record<string, unknown> = (() => {
        const last = value.last
        if (isObject(last)) return last

        const lastSnake = value.last_token_usage
        if (isObject(lastSnake)) return lastSnake

        const lastCamel = value.lastTokenUsage
        if (isObject(lastCamel)) return lastCamel

        return value
    })()
    const totalSource: Record<string, unknown> | null = (() => {
        const total = value.total
        if (isObject(total)) return total

        const totalSnake = value.total_token_usage
        if (isObject(totalSnake)) return totalSnake

        const totalCamel = value.totalTokenUsage
        if (isObject(totalCamel)) return totalCamel

        return null
    })()

    const readNumberFrom = (source: Record<string, unknown>, ...keys: string[]): number | null => {
        for (const key of keys) {
            const parsed = asNumber(source[key])
            if (parsed !== null) return parsed
        }
        return null
    }

    const readNumber = (...keys: string[]): number | null => {
        const parsed = readNumberFrom(usageSource, ...keys)
        if (parsed !== null) return parsed
        if (usageSource === value) return null
        return readNumberFrom(value, ...keys)
    }

    let inputTokens = readNumber('input_tokens', 'inputTokens', 'prompt_tokens', 'promptTokens')
    let outputTokens = readNumber('output_tokens', 'outputTokens', 'completion_tokens', 'completionTokens')
    const cacheCreation = readNumber('cache_creation_input_tokens', 'cacheCreationInputTokens')
    const cacheRead = readNumber('cache_read_input_tokens', 'cacheReadInputTokens')
    const cachedInput = readNumber('cached_input_tokens', 'cachedInputTokens')
    const reasoningOutput = readNumber('reasoning_output_tokens', 'reasoningOutputTokens')
    let totalTokens = readNumber('total_tokens', 'totalTokens')
    if (totalTokens === null && totalSource) {
        totalTokens = readNumberFrom(totalSource, 'total_tokens', 'totalTokens')
        if (totalTokens === null) {
            const totalInput = readNumberFrom(totalSource, 'input_tokens', 'inputTokens', 'prompt_tokens', 'promptTokens')
            const totalOutput = readNumberFrom(totalSource, 'output_tokens', 'outputTokens', 'completion_tokens', 'completionTokens')
            if (totalInput !== null && totalOutput !== null) {
                totalTokens = Math.max(0, totalInput + totalOutput)
            }
        }
    }

    let modelContextWindow = readNumber('model_context_window', 'modelContextWindow')
    if (modelContextWindow === null && totalSource) {
        modelContextWindow = readNumberFrom(totalSource, 'model_context_window', 'modelContextWindow')
    }

    if (inputTokens === null && totalTokens !== null && outputTokens !== null) {
        inputTokens = Math.max(0, totalTokens - outputTokens)
    }
    if (outputTokens === null && totalTokens !== null && inputTokens !== null) {
        outputTokens = Math.max(0, totalTokens - inputTokens)
    }

    if (inputTokens === null && outputTokens === null && totalTokens === null) {
        return null
    }

    const serviceTier = asString(usageSource.service_tier)
        ?? asString(usageSource.serviceTier)
        ?? asString(value.service_tier)
        ?? asString(value.serviceTier)
        ?? undefined

    return {
        input_tokens: inputTokens ?? 0,
        output_tokens: outputTokens ?? 0,
        cache_creation_input_tokens: cacheCreation ?? undefined,
        cache_read_input_tokens: cacheRead ?? undefined,
        cached_input_tokens: cachedInput ?? undefined,
        reasoning_output_tokens: reasoningOutput ?? undefined,
        total_tokens: totalTokens ?? undefined,
        model_context_window: modelContextWindow ?? undefined,
        service_tier: serviceTier
    }
}

function normalizeAssistantOutput(
    messageId: string,
    localId: string | null,
    createdAt: number,
    data: Record<string, unknown>,
    meta?: unknown
): NormalizedMessage | null {
    const uuid = asString(data.uuid) ?? messageId
    const parentUUID = asString(data.parentUuid) ?? null
    const isSidechain = Boolean(data.isSidechain)

    const message = isObject(data.message) ? data.message : null
    if (!message) return null

    const modelContent = message.content
    const blocks: NormalizedAgentContent[] = []

    if (typeof modelContent === 'string') {
        blocks.push({ type: 'text', text: modelContent, uuid, parentUUID })
    } else if (Array.isArray(modelContent)) {
        for (const block of modelContent) {
            if (!isObject(block) || typeof block.type !== 'string') continue
            if (block.type === 'text' && typeof block.text === 'string') {
                blocks.push({ type: 'text', text: block.text, uuid, parentUUID })
                continue
            }
            if (block.type === 'thinking' && typeof block.thinking === 'string') {
                blocks.push({ type: 'reasoning', text: block.thinking, uuid, parentUUID })
                continue
            }
            if (block.type === 'tool_use' && typeof block.id === 'string') {
                const name = asString(block.name) ?? 'Tool'
                const input = 'input' in block ? (block as Record<string, unknown>).input : undefined
                const description = isObject(input) && typeof input.description === 'string' ? input.description : null
                blocks.push({ type: 'tool-call', id: block.id, name, input, description, uuid, parentUUID })
            }
        }
    }

    const usage = normalizeUsageData(message.usage)

    return {
        id: messageId,
        localId,
        createdAt,
        role: 'agent',
        isSidechain,
        content: blocks,
        meta,
        usage: usage ?? undefined
    }
}

function normalizeUserOutput(
    messageId: string,
    localId: string | null,
    createdAt: number,
    data: Record<string, unknown>,
    meta?: unknown
): NormalizedMessage | null {
    const uuid = asString(data.uuid) ?? messageId
    const parentUUID = asString(data.parentUuid) ?? null
    const isSidechain = Boolean(data.isSidechain)

    const message = isObject(data.message) ? data.message : null
    if (!message) return null

    const messageContent = message.content

    if (isSidechain && typeof messageContent === 'string') {
        return {
            id: messageId,
            localId,
            createdAt,
            role: 'agent',
            isSidechain: true,
            content: [{ type: 'sidechain', uuid, prompt: messageContent }]
        }
    }

    if (typeof messageContent === 'string') {
        return {
            id: messageId,
            localId,
            createdAt,
            role: 'user',
            isSidechain: false,
            content: { type: 'text', text: messageContent },
            meta
        }
    }

    const blocks: NormalizedAgentContent[] = []

    if (Array.isArray(messageContent)) {
        for (const block of messageContent) {
            if (!isObject(block) || typeof block.type !== 'string') continue
            if (block.type === 'text' && typeof block.text === 'string') {
                blocks.push({ type: 'text', text: block.text, uuid, parentUUID })
                continue
            }
            if (block.type === 'tool_result' && typeof block.tool_use_id === 'string') {
                const isError = Boolean(block.is_error)
                const rawContent = 'content' in block ? (block as Record<string, unknown>).content : undefined
                const embeddedToolUseResult = 'toolUseResult' in data ? (data as Record<string, unknown>).toolUseResult : null

                const permissions = normalizeToolResultPermissions(block.permissions)

                blocks.push({
                    type: 'tool-result',
                    tool_use_id: block.tool_use_id,
                    content: embeddedToolUseResult ?? rawContent,
                    is_error: isError,
                    uuid,
                    parentUUID,
                    permissions
                })
            }
        }
    }

    return {
        id: messageId,
        localId,
        createdAt,
        role: 'agent',
        isSidechain,
        content: blocks,
        meta
    }
}

export function isSkippableAgentContent(content: unknown): boolean {
    if (!isObject(content) || content.type !== 'output') return false
    const data = isObject(content.data) ? content.data : null
    if (!data) return false
    return Boolean(data.isMeta) || Boolean(data.isCompactSummary)
}

export function isCodexContent(content: unknown): boolean {
    return isObject(content) && content.type === 'codex'
}

export function normalizeAgentRecord(
    messageId: string,
    localId: string | null,
    createdAt: number,
    content: unknown,
    meta?: unknown,
    agentFlavor?: string | null
): NormalizedMessage | null {
    if (!isObject(content) || typeof content.type !== 'string') return null

    if (content.type === 'output') {
        const data = isObject(content.data) ? content.data : null
        if (!data || typeof data.type !== 'string') return null

        // Skip meta/compact-summary messages (parity with hapi-app)
        if (data.isMeta) return null
        if (data.isCompactSummary) return null

        if (data.type === 'assistant') {
            return normalizeAssistantOutput(messageId, localId, createdAt, data, meta)
        }
        if (data.type === 'user') {
            return normalizeUserOutput(messageId, localId, createdAt, data, meta)
        }
        if (data.type === 'summary' && typeof data.summary === 'string') {
            return {
                id: messageId,
                localId,
                createdAt,
                role: 'agent',
                isSidechain: false,
                content: [{ type: 'summary', summary: data.summary }],
                meta
            }
        }
        if (data.type === 'system' && data.subtype === 'api_error') {
            return {
                id: messageId,
                localId,
                createdAt,
                role: 'event',
                content: {
                    type: 'api-error',
                    retryAttempt: asNumber(data.retryAttempt) ?? 0,
                    maxRetries: asNumber(data.maxRetries) ?? 0,
                    error: data.error
                },
                isSidechain: false,
                meta
            }
        }
        if (data.type === 'system' && data.subtype === 'turn_duration') {
            return {
                id: messageId,
                localId,
                createdAt,
                role: 'event',
                content: {
                    type: 'turn-duration',
                    durationMs: asNumber(data.durationMs) ?? 0
                },
                isSidechain: false,
                meta
            }
        }
        if (data.type === 'system' && data.subtype === 'microcompact_boundary') {
            const metadata = isObject(data.microcompactMetadata) ? data.microcompactMetadata : null
            return {
                id: messageId,
                localId,
                createdAt,
                role: 'event',
                content: {
                    type: 'microcompact',
                    trigger: asString(metadata?.trigger) ?? 'auto',
                    preTokens: asNumber(metadata?.preTokens) ?? 0,
                    tokensSaved: asNumber(metadata?.tokensSaved) ?? 0
                },
                isSidechain: false,
                meta
            }
        }
        if (data.type === 'system' && data.subtype === 'compact_boundary') {
            const metadata = isObject(data.compactMetadata) ? data.compactMetadata : null
            return {
                id: messageId,
                localId,
                createdAt,
                role: 'event',
                content: {
                    type: 'compact',
                    trigger: asString(metadata?.trigger) ?? 'auto',
                    preTokens: asNumber(metadata?.preTokens) ?? 0
                },
                isSidechain: false,
                meta
            }
        }
        return null
    }

    if (content.type === 'event') {
        const event = normalizeAgentEvent(content.data)
        if (!event) return null
        return {
            id: messageId,
            localId,
            createdAt,
            role: 'event',
            content: event,
            isSidechain: false,
            meta
        }
    }

    if (content.type === 'codex') {
        const data = isObject(content.data) ? content.data : null
        if (!data || typeof data.type !== 'string') return null

        if (data.type === 'token_count') {
            if (agentFlavor === 'claude') return null
            const usageSource = isObject(data.info) ? data.info : data
            const usage = normalizeUsageData(usageSource)
            if (!usage) return null
            return {
                id: messageId,
                localId,
                createdAt,
                role: 'agent',
                isSidechain: false,
                content: [],
                meta,
                usage
            }
        }

        if (data.type === 'message' && typeof data.message === 'string') {
            return {
                id: messageId,
                localId,
                createdAt,
                role: 'agent',
                isSidechain: false,
                content: [{ type: 'text', text: data.message, uuid: messageId, parentUUID: null }],
                meta
            }
        }

        if (data.type === 'reasoning' && typeof data.message === 'string') {
            return {
                id: messageId,
                localId,
                createdAt,
                role: 'agent',
                isSidechain: false,
                content: [{ type: 'reasoning', text: data.message, uuid: messageId, parentUUID: null }],
                meta
            }
        }

        if (data.type === 'tool-call' && typeof data.callId === 'string') {
            const uuid = asString(data.id) ?? messageId
            return {
                id: messageId,
                localId,
                createdAt,
                role: 'agent',
                isSidechain: false,
                content: [{
                    type: 'tool-call',
                    id: data.callId,
                    name: asString(data.name) ?? 'unknown',
                    input: data.input,
                    description: null,
                    uuid,
                    parentUUID: null
                }],
                meta
            }
        }

        if (data.type === 'tool-call-result' && typeof data.callId === 'string') {
            const uuid = asString(data.id) ?? messageId
            const isError = Boolean(data.is_error ?? data.isError)
            return {
                id: messageId,
                localId,
                createdAt,
                role: 'agent',
                isSidechain: false,
                content: [{
                    type: 'tool-result',
                    tool_use_id: data.callId,
                    content: data.output,
                    is_error: isError,
                    uuid,
                    parentUUID: null
                }],
                meta
            }
        }

        if (data.type === 'event') {
            const event = normalizeAgentEvent(data.data)
            if (!event) return null
            return {
                id: messageId,
                localId,
                createdAt,
                role: 'event',
                content: event,
                isSidechain: false,
                meta
            }
        }
    }

    return null
}
