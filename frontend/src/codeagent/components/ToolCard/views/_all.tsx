import type { ComponentType } from 'react'
import type { ToolCallBlock } from '@/chat/types'
import type { SessionMetadataSummary } from '@/types/api'
import { CodexDiffCompactView, CodexDiffFullView } from '@/components/ToolCard/views/CodexDiffView'
import { CodexPatchView } from '@/components/ToolCard/views/CodexPatchView'
import { EditView } from '@/components/ToolCard/views/EditView'
import { AskUserQuestionView } from '@/components/ToolCard/views/AskUserQuestionView'
import { RequestUserInputView } from '@/components/ToolCard/views/RequestUserInputView'
import { ExitPlanModeView } from '@/components/ToolCard/views/ExitPlanModeView'
import { MultiEditFullView, MultiEditView } from '@/components/ToolCard/views/MultiEditView'
import { TodoWriteView } from '@/components/ToolCard/views/TodoWriteView'
import { WriteView } from '@/components/ToolCard/views/WriteView'

export type ToolViewProps = {
    block: ToolCallBlock
    metadata: SessionMetadataSummary | null
}

export type ToolViewComponent = ComponentType<ToolViewProps>

export const toolViewRegistry: Record<string, ToolViewComponent> = {
    Edit: EditView,
    MultiEdit: MultiEditView,
    Write: WriteView,
    TodoWrite: TodoWriteView,
    CodexDiff: CodexDiffCompactView,
    AskUserQuestion: AskUserQuestionView,
    ExitPlanMode: ExitPlanModeView,
    ask_user_question: AskUserQuestionView,
    exit_plan_mode: ExitPlanModeView,
    request_user_input: RequestUserInputView
}

export const toolFullViewRegistry: Record<string, ToolViewComponent> = {
    Edit: EditView,
    MultiEdit: MultiEditFullView,
    Write: WriteView,
    CodexDiff: CodexDiffFullView,
    CodexPatch: CodexPatchView,
    AskUserQuestion: AskUserQuestionView,
    ExitPlanMode: ExitPlanModeView,
    ask_user_question: AskUserQuestionView,
    exit_plan_mode: ExitPlanModeView,
    request_user_input: RequestUserInputView
}

function resolveToolAlias(toolName: string): string {
    const normalized = toolName.trim().toLowerCase()
    if (
        normalized === 'askuserquestion'
        || normalized === 'ask_user_question'
        || normalized.endsWith('__ask_user_question')
        || normalized.endsWith('_ask_user_question')
    ) {
        return 'ask_user_question'
    }
    if (
        normalized === 'request_user_input'
        || normalized.endsWith('__request_user_input')
        || normalized.endsWith('_request_user_input')
    ) {
        return 'request_user_input'
    }
    return toolName
}

export function getToolViewComponent(toolName: string): ToolViewComponent | null {
    const resolved = resolveToolAlias(toolName)
    return toolViewRegistry[toolName] ?? toolViewRegistry[resolved] ?? null
}

export function getToolFullViewComponent(toolName: string): ToolViewComponent | null {
    const resolved = resolveToolAlias(toolName)
    return toolFullViewRegistry[toolName] ?? toolFullViewRegistry[resolved] ?? null
}
