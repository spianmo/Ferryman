import type { ModelMode } from '@/types/api'

/**
 * Context windows vary by model/provider.
 * Prefer server-provided `model_context_window` when available.
 * These values are fallback defaults for legacy payloads.
 */

const CLAUDE_MODEL_CONTEXT_WINDOWS: Record<ModelMode, number> = {
    default: 200_000,
    sonnet: 200_000,
    opus: 200_000
}

const MODEL_CONTEXT_WINDOWS_BY_NAME: Record<string, number> = {
    // Sources:
    // - OpenAI Codex model pricing/docs: https://developers.openai.com/codex/model-pricing
    // - OpenAI model docs for GPT-5.4: https://platform.openai.com/docs/models/gpt-5.4
    'gpt-5.4': 1_050_000,
    'gpt-5.3-codex': 272_000,
    'gpt-5.2-codex': 272_000,
    'gpt-5.2': 400_000,
    'gpt-5.1-codex-max': 272_000,
    'gpt-5.1-codex-mini': 272_000,
    'gemini-3-pro-preview': 1_048_576,
    'gemini-2.5-pro': 1_048_576,
    'gemini-2.5-flash': 1_048_576
}

function normalizeModel(model: string | null | undefined): string {
    return (model ?? '').trim().toLowerCase()
}

export function getContextBudgetTokens(options: {
    modelMode?: ModelMode
    model?: string
    flavor?: string | null
}): number | null {
    const normalizedModel = normalizeModel(options.model)
    if (normalizedModel && MODEL_CONTEXT_WINDOWS_BY_NAME[normalizedModel]) {
        return MODEL_CONTEXT_WINDOWS_BY_NAME[normalizedModel]
    }

    if (options.flavor === 'codex') {
        return 272_000
    }
    if (options.flavor === 'gemini') {
        return 1_048_576
    }

    const mode: ModelMode = options.modelMode ?? 'default'
    const claudeWindow = CLAUDE_MODEL_CONTEXT_WINDOWS[mode]
    if (!claudeWindow) return null
    return Math.max(1, claudeWindow)
}
