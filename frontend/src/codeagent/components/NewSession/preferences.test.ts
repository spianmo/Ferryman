import { beforeEach, describe, expect, it } from 'vitest'
import {
    loadPreferredAgent,
    loadPreferredPermissionChoice,
    savePreferredAgent,
    savePreferredPermissionChoice,
} from './preferences'

describe('NewSession preferences', () => {
    beforeEach(() => {
        localStorage.clear()
    })

    it('loads defaults when storage is empty', () => {
        expect(loadPreferredAgent()).toBe('claude')
        expect(loadPreferredPermissionChoice('claude')).toBe('default')
        expect(loadPreferredPermissionChoice('codex')).toBe('auto')
        expect(loadPreferredPermissionChoice('cursor')).toBe('agent')
        expect(loadPreferredPermissionChoice('opencode')).toBe('ask')
    })

    it('loads saved values from storage', () => {
        localStorage.setItem('hapi:newSession:agent', 'codex')
        localStorage.setItem('hapi:newSession:permissionMode:codex', 'full-access')
        localStorage.setItem('hapi:newSession:permissionMode:cursor', 'force')

        expect(loadPreferredAgent()).toBe('codex')
        expect(loadPreferredPermissionChoice('codex')).toBe('full-access')
        expect(loadPreferredPermissionChoice('cursor')).toBe('force')
    })

    it('falls back to default agent on invalid stored value', () => {
        localStorage.setItem('hapi:newSession:agent', 'unknown-agent')

        expect(loadPreferredAgent()).toBe('claude')
    })

    it('falls back to the agent default for invalid stored permission choices', () => {
        localStorage.setItem('hapi:newSession:permissionMode:codex', 'plan')
        localStorage.setItem('hapi:newSession:permissionMode:claude', 'full-access')
        localStorage.setItem('hapi:newSession:permissionMode:cursor', 'default')
        localStorage.setItem('hapi:newSession:permissionMode:opencode', 'yolo')

        expect(loadPreferredPermissionChoice('codex')).toBe('auto')
        expect(loadPreferredPermissionChoice('claude')).toBe('default')
        expect(loadPreferredPermissionChoice('cursor')).toBe('agent')
        expect(loadPreferredPermissionChoice('opencode')).toBe('ask')
    })

    it('persists new values to storage', () => {
        savePreferredAgent('gemini')
        savePreferredPermissionChoice('codex', 'auto')
        savePreferredPermissionChoice('cursor', 'force')

        expect(localStorage.getItem('hapi:newSession:agent')).toBe('gemini')
        expect(localStorage.getItem('hapi:newSession:permissionMode:codex')).toBe('auto')
        expect(localStorage.getItem('hapi:newSession:permissionMode:cursor')).toBe('force')
    })
})
