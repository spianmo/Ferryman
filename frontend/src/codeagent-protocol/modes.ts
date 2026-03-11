export const CLAUDE_PERMISSION_MODES = ['default', 'acceptEdits', 'plan', 'bypassPermissions'] as const
export type ClaudePermissionMode = typeof CLAUDE_PERMISSION_MODES[number]

export const CODEX_PERMISSION_MODES = ['read-only', 'auto', 'full-access'] as const
export type CodexPermissionMode = typeof CODEX_PERMISSION_MODES[number]

export const GEMINI_PERMISSION_MODES = ['default', 'auto-edit', 'plan', 'yolo'] as const
export type GeminiPermissionMode = typeof GEMINI_PERMISSION_MODES[number]

export const OPENCODE_PERMISSION_MODES = ['ask', 'allow', 'deny'] as const
export type OpencodePermissionMode = typeof OPENCODE_PERMISSION_MODES[number]

export const CURSOR_PERMISSION_MODES = ['agent', 'plan', 'ask', 'force'] as const
export type CursorPermissionMode = typeof CURSOR_PERMISSION_MODES[number]

export const PERMISSION_MODES = [
    'default',
    'acceptEdits',
    'plan',
    'bypassPermissions',
    'ask',
    'read-only',
    'auto',
    'full-access',
    'auto-edit',
    'yolo',
    'allow',
    'deny',
    'agent',
    'force'
] as const
export type PermissionMode = typeof PERMISSION_MODES[number]

export const MODEL_MODES = ['default', 'sonnet', 'opus'] as const
export type ModelMode = typeof MODEL_MODES[number]

export const REASONING_EFFORTS = ['low', 'medium', 'high', 'xhigh'] as const
export type ReasoningEffort = typeof REASONING_EFFORTS[number]

export type AgentFlavor = 'claude' | 'codex' | 'gemini' | 'opencode' | 'cursor'

export const PERMISSION_MODE_LABELS: Record<PermissionMode, string> = {
    default: 'Default',
    acceptEdits: 'Accept Edits',
    plan: 'Plan',
    ask: 'Ask',
    bypassPermissions: 'Bypass Permissions',
    'read-only': 'Read Only',
    auto: 'Auto',
    'full-access': 'Full Access',
    'auto-edit': 'Auto Edit',
    yolo: 'YOLO',
    allow: 'Allow',
    deny: 'Deny',
    agent: 'Agent',
    force: 'Force'
}

export const PERMISSION_MODE_DESCRIPTIONS: Partial<Record<PermissionMode, string>> = {
    default: 'Claude asks when it needs elevated actions.',
    acceptEdits: 'Auto-apply file edits while still asking before broader actions.',
    plan: 'Inspect and plan without making changes.',
    bypassPermissions: 'Skip permission prompts. Use only in trusted environments.',
    ask: 'Ask before taking actions that need approval.',
    'read-only': 'Inspect the codebase without changing files.',
    auto: 'Work inside the workspace and ask before sensitive actions.',
    'full-access': 'Skip approvals and sandboxing. Use only in trusted environments.',
    'auto-edit': 'Auto-approve edit tools while prompting for everything else.',
    yolo: 'Auto-approve all tools. Use only in trusted environments.',
    allow: 'Allow requested tools without prompting.',
    deny: 'Deny requested tools by default.',
    agent: 'Run in standard agent mode.',
    force: 'Skip confirmation prompts and run with force enabled.'
}

export type PermissionModeTone = 'neutral' | 'info' | 'warning' | 'danger'

export const PERMISSION_MODE_TONES: Record<PermissionMode, PermissionModeTone> = {
    default: 'neutral',
    acceptEdits: 'warning',
    plan: 'info',
    ask: 'neutral',
    bypassPermissions: 'danger',
    'read-only': 'neutral',
    auto: 'info',
    'full-access': 'danger',
    'auto-edit': 'warning',
    yolo: 'danger',
    allow: 'warning',
    deny: 'info',
    agent: 'info',
    force: 'danger'
}

export type PermissionModeOption = {
    mode: PermissionMode
    label: string
    tone: PermissionModeTone
    description?: string
}

export const MODEL_MODE_LABELS: Record<ModelMode, string> = {
    default: 'Default',
    sonnet: 'Sonnet',
    opus: 'Opus'
}

export const REASONING_EFFORT_LABELS: Record<ReasoningEffort, string> = {
    low: 'Low',
    medium: 'Medium',
    high: 'High',
    xhigh: 'X-High'
}

export function getPermissionModeLabel(mode: PermissionMode): string {
    return PERMISSION_MODE_LABELS[mode]
}

export function getPermissionModeTone(mode: PermissionMode): PermissionModeTone {
    return PERMISSION_MODE_TONES[mode]
}

export function getPermissionModeDescription(mode: PermissionMode): string | undefined {
    return PERMISSION_MODE_DESCRIPTIONS[mode]
}

export function getPermissionModesForFlavor(flavor?: string | null): readonly PermissionMode[] {
    if (flavor === 'codex') {
        return CODEX_PERMISSION_MODES
    }
    if (flavor === 'gemini') {
        return GEMINI_PERMISSION_MODES
    }
    if (flavor === 'opencode') {
        return OPENCODE_PERMISSION_MODES
    }
    if (flavor === 'cursor') {
        return CURSOR_PERMISSION_MODES
    }
    return CLAUDE_PERMISSION_MODES
}

export function getPermissionModeOptionsForFlavor(flavor?: string | null): PermissionModeOption[] {
    return getPermissionModesForFlavor(flavor).map((mode) => ({
        mode,
        label: getPermissionModeLabel(mode),
        tone: getPermissionModeTone(mode),
        description: getPermissionModeDescription(mode)
    }))
}

export function isPermissionModeAllowedForFlavor(mode: PermissionMode, flavor?: string | null): boolean {
    return getPermissionModesForFlavor(flavor).includes(mode)
}

export function getModelModesForFlavor(flavor?: string | null): readonly ModelMode[] {
    if (flavor === 'codex' || flavor === 'gemini' || flavor === 'opencode' || flavor === 'cursor') {
        return []
    }
    return MODEL_MODES
}

export function isModelModeAllowedForFlavor(mode: ModelMode, flavor?: string | null): boolean {
    return getModelModesForFlavor(flavor).includes(mode)
}

export function getReasoningEffortsForFlavor(flavor?: string | null): readonly ReasoningEffort[] {
    if (flavor === 'codex') {
        return REASONING_EFFORTS
    }
    return []
}

export function isReasoningEffortAllowedForFlavor(effort: ReasoningEffort, flavor?: string | null): boolean {
    return getReasoningEffortsForFlavor(flavor).includes(effort)
}
