import type { AgentState } from '@/types/api'
import type { ChatBlock, NormalizedMessage, UsageData } from '@/chat/types'
import { traceMessages, type TracedMessage } from '@/chat/tracer'
import { dedupeAgentEvents, foldApiErrorEvents } from '@/chat/reducerEvents'
import { collectTitleChanges, collectToolIdsFromMessages, ensureToolBlock, getPermissions } from '@/chat/reducerTools'
import { reduceTimeline } from '@/chat/reducerTimeline'

// Calculate context size from usage data
function calculateContextSize(usage: UsageData): number {
    if (typeof usage.total_tokens === 'number' && Number.isFinite(usage.total_tokens)) {
        return Math.max(0, usage.total_tokens)
    }

    const hasClaudeCacheFields = usage.cache_creation_input_tokens !== undefined
        || usage.cache_read_input_tokens !== undefined
    if (hasClaudeCacheFields) {
        return Math.max(
            0,
            usage.input_tokens
            + (usage.cache_creation_input_tokens || 0)
            + (usage.cache_read_input_tokens || 0)
        )
    }

    return Math.max(0, usage.input_tokens + usage.output_tokens)
}

export type LatestUsage = {
    inputTokens: number
    outputTokens: number
    cacheCreation: number
    cacheRead: number
    totalTokens?: number
    contextWindow?: number
    contextSize: number
    timestamp: number
}

export function reduceChatBlocks(
    normalized: NormalizedMessage[],
    agentState: AgentState | null | undefined
): { blocks: ChatBlock[]; hasReadyEvent: boolean; latestUsage: LatestUsage | null } {
    const permissionsById = getPermissions(agentState)
    const toolIdsInMessages = collectToolIdsFromMessages(normalized)
    const titleChangesByToolUseId = collectTitleChanges(normalized)

    const traced = traceMessages(normalized)
    const groups = new Map<string, TracedMessage[]>()
    const root: TracedMessage[] = []

    for (const msg of traced) {
        if (msg.sidechainId) {
            const existing = groups.get(msg.sidechainId) ?? []
            existing.push(msg)
            groups.set(msg.sidechainId, existing)
        } else {
            root.push(msg)
        }
    }

    const consumedGroupIds = new Set<string>()
    const emittedTitleChangeToolUseIds = new Set<string>()
    const reducerContext = { permissionsById, groups, consumedGroupIds, titleChangesByToolUseId, emittedTitleChangeToolUseIds }
    const rootResult = reduceTimeline(root, reducerContext)
    let hasReadyEvent = rootResult.hasReadyEvent

    // Only create permission-only tool cards when there is no tool call/result in the transcript.
    // Also skip if the permission is older than the oldest message in the current view,
    // to avoid mixing old tool cards with newer messages when paginating.
    const oldestMessageTime = normalized.length > 0
        ? Math.min(...normalized.map(m => m.createdAt))
        : null

    for (const [id, entry] of permissionsById) {
        if (toolIdsInMessages.has(id)) continue
        if (rootResult.toolBlocksById.has(id)) continue

        const createdAt = entry.permission.createdAt ?? Date.now()

        // Skip permissions that are older than the oldest message in the current view.
        // These will be shown when the user loads older messages.
        if (oldestMessageTime !== null && createdAt < oldestMessageTime) {
            continue
        }

        const block = ensureToolBlock(rootResult.blocks, rootResult.toolBlocksById, id, {
            createdAt,
            localId: null,
            name: entry.toolName,
            input: entry.input,
            description: null,
            permission: entry.permission
        })

        if (entry.permission.status === 'approved') {
            block.tool.state = 'completed'
            block.tool.completedAt = entry.permission.completedAt ?? createdAt
            if (block.tool.result === undefined) {
                block.tool.result = 'Approved'
            }
        } else if (entry.permission.status === 'denied' || entry.permission.status === 'canceled') {
            block.tool.state = 'error'
            block.tool.completedAt = entry.permission.completedAt ?? createdAt
            if (block.tool.result === undefined && entry.permission.reason) {
                block.tool.result = { error: entry.permission.reason }
            }
        }
    }

    // Calculate usage snapshot for status bar.
    // Prefer server-provided cumulative totals when available; otherwise
    // aggregate per-turn usage so the counter doesn't reset every turn.
    let latestUsage: LatestUsage | null = null
    let latestUsageMessage: NormalizedMessage | null = null
    let aggregatedInputTokens = 0
    let aggregatedOutputTokens = 0
    let aggregatedCacheCreationTokens = 0
    let aggregatedCacheReadTokens = 0
    let latestTotalTokens: number | undefined
    let latestContextWindow: number | undefined

    for (const msg of normalized) {
        const usage = msg.usage
        if (!usage) continue

        latestUsageMessage = msg
        aggregatedInputTokens += Math.max(0, usage.input_tokens)
        aggregatedOutputTokens += Math.max(0, usage.output_tokens)
        aggregatedCacheCreationTokens += Math.max(0, usage.cache_creation_input_tokens ?? 0)
        aggregatedCacheReadTokens += Math.max(0, usage.cache_read_input_tokens ?? 0)

        if (typeof usage.total_tokens === 'number' && Number.isFinite(usage.total_tokens)) {
            latestTotalTokens = Math.max(0, usage.total_tokens)
        }
        if (typeof usage.model_context_window === 'number' && Number.isFinite(usage.model_context_window)) {
            latestContextWindow = Math.max(0, usage.model_context_window)
        }
    }

    if (latestUsageMessage?.usage) {
        const latest = latestUsageMessage.usage
        const mergedUsage: UsageData = {
            input_tokens: latestTotalTokens !== undefined ? latest.input_tokens : aggregatedInputTokens,
            output_tokens: latestTotalTokens !== undefined ? latest.output_tokens : aggregatedOutputTokens,
            cache_creation_input_tokens: latestTotalTokens !== undefined
                ? latest.cache_creation_input_tokens
                : aggregatedCacheCreationTokens,
            cache_read_input_tokens: latestTotalTokens !== undefined
                ? latest.cache_read_input_tokens
                : aggregatedCacheReadTokens,
            total_tokens: latestTotalTokens,
            model_context_window: latestContextWindow
        }

        latestUsage = {
            inputTokens: mergedUsage.input_tokens,
            outputTokens: mergedUsage.output_tokens,
            cacheCreation: mergedUsage.cache_creation_input_tokens ?? 0,
            cacheRead: mergedUsage.cache_read_input_tokens ?? 0,
            totalTokens: mergedUsage.total_tokens ?? undefined,
            contextWindow: mergedUsage.model_context_window ?? undefined,
            contextSize: calculateContextSize(mergedUsage),
            timestamp: latestUsageMessage.createdAt
        }
    }

    return { blocks: dedupeAgentEvents(foldApiErrorEvents(rootResult.blocks)), hasReadyEvent, latestUsage }
}
