import {
    getPermissionModeOptionsForFlavor,
    isPermissionModeAllowedForFlavor,
    type PermissionMode,
    type PermissionModeOption,
    type PermissionModeTone,
} from '@hapi/protocol'
import type { I18nContextValue } from '@/lib/use-translation'
import { getPermissionModeDescriptionText } from '@/lib/permissionModeDescriptions'
import type { AgentType } from './types'

const PERMISSION_MODE_STORAGE_KEY_PREFIX = 'hapi:newSession:permissionMode:'

export type NewSessionPermissionChoice = PermissionMode

export type NewSessionPermissionChoiceOption = {
    value: NewSessionPermissionChoice
    label: string
    tone: PermissionModeTone
    description?: string
}

export function getDefaultPermissionChoice(agent: AgentType): NewSessionPermissionChoice {
    switch (agent) {
        case 'codex':
            return 'auto'
        case 'cursor':
            return 'agent'
        case 'opencode':
            return 'ask'
        default:
            return 'default'
    }
}

export function getPermissionChoiceOptions(agent: AgentType, t: I18nContextValue['t']): NewSessionPermissionChoiceOption[] {
    return getPermissionModeOptionsForFlavor(agent)
        .map((option) => toChoiceOption(option, t))
}

export function loadPreferredPermissionChoice(agent: AgentType): NewSessionPermissionChoice {
    const fallback = getDefaultPermissionChoice(agent)

    try {
        const stored = localStorage.getItem(`${PERMISSION_MODE_STORAGE_KEY_PREFIX}${agent}`)
        if (stored && isPermissionChoiceAllowedForAgent(stored, agent)) {
            return stored as NewSessionPermissionChoice
        }
    } catch {
        return fallback
    }

    return fallback
}

export function savePreferredPermissionChoice(agent: AgentType, choice: NewSessionPermissionChoice): void {
    try {
        localStorage.setItem(`${PERMISSION_MODE_STORAGE_KEY_PREFIX}${agent}`, choice)
    } catch {
        // Ignore storage errors
    }
}

function isPermissionChoiceAllowedForAgent(choice: string, agent: AgentType): choice is NewSessionPermissionChoice {
    return isPermissionModeAllowedForFlavor(choice as PermissionMode, agent)
}

function toChoiceOption(option: PermissionModeOption, t: I18nContextValue['t']): NewSessionPermissionChoiceOption {
    return {
        value: option.mode,
        label: option.label,
        tone: option.tone,
        description: getPermissionModeDescriptionText(t, option.mode)
    }
}
