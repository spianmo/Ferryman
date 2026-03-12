import { useCallback, useEffect, useMemo, useState, type KeyboardEvent as ReactKeyboardEvent } from 'react'
import type { ApiClient } from '@/api/client'
import type { Machine } from '@/types/api'
import { usePlatform } from '@/hooks/usePlatform'
import { useSpawnSession } from '@/hooks/mutations/useSpawnSession'
import { useMachinePathExists } from '@/hooks/queries/useMachinePathExists'
import { useSessions } from '@/hooks/queries/useSessions'
import { useMachinePathsExists } from '@/hooks/queries/useMachinePathsExists'
import type { Suggestion } from '@/hooks/useActiveSuggestions'
import { useDirectorySuggestions } from '@/hooks/useDirectorySuggestions'
import { useRecentPaths } from '@/hooks/useRecentPaths'
import { useTranslation } from '@/lib/use-translation'
import type { AgentType } from './types'
import { ActionButtons } from './ActionButtons'
import { AgentSelector } from './AgentSelector'
import { DirectorySection } from './DirectorySection'
import { MachineSelector } from './MachineSelector'
import { ModelSelector } from './ModelSelector'
import {
    loadPreferredAgent,
    loadPreferredPermissionChoice,
    savePreferredAgent,
    savePreferredPermissionChoice,
} from './preferences'
import {
    getPermissionChoiceOptions,
    type NewSessionPermissionChoice,
} from './permissionChoices'
import { DirectoryPickerDialog } from './DirectoryPickerDialog'
import { PermissionModeSelector } from './PermissionModeSelector'

export function NewSession(props: {
    api: ApiClient
    machines: Machine[]
    isLoading?: boolean
    onSuccess: (sessionId: string) => void
    onCancel: () => void
}) {
    const { t } = useTranslation()
    const { haptic } = usePlatform()
    const { spawnSession, isPending, error: spawnError } = useSpawnSession(props.api)
    const { sessions } = useSessions(props.api)
    const isFormDisabled = Boolean(isPending || props.isLoading)
    const { getRecentPaths, addRecentPath, getLastUsedMachineId, setLastUsedMachineId } = useRecentPaths()

    const [machineId, setMachineId] = useState<string | null>(null)
    const [directory, setDirectory] = useState('')
    const [suppressSuggestions, setSuppressSuggestions] = useState(false)
    const [isDirectoryFocused, setIsDirectoryFocused] = useState(false)
    const [showDirectorySuggestions, setShowDirectorySuggestions] = useState(false)
    const [selectedSuggestionIndex, setSelectedSuggestionIndex] = useState(-1)
    const [agent, setAgent] = useState<AgentType>(loadPreferredAgent)
    const [model, setModel] = useState('auto')
    const [permissionChoice, setPermissionChoice] = useState<NewSessionPermissionChoice>(() => loadPreferredPermissionChoice(loadPreferredAgent()))
    const [error, setError] = useState<string | null>(null)
    const [directoryDialogOpen, setDirectoryDialogOpen] = useState(false)

    useEffect(() => {
        setModel('auto')
        setPermissionChoice(loadPreferredPermissionChoice(agent))
    }, [agent])

    useEffect(() => {
        savePreferredAgent(agent)
    }, [agent])

    useEffect(() => {
        savePreferredPermissionChoice(agent, permissionChoice)
    }, [agent, permissionChoice])

    useEffect(() => {
        if (props.machines.length === 0) return
        if (machineId && props.machines.find((m) => m.id === machineId)) return

        const lastUsed = getLastUsedMachineId()
        const foundLast = lastUsed ? props.machines.find((m) => m.id === lastUsed) : null

        if (foundLast) {
            setMachineId(foundLast.id)
            const paths = getRecentPaths(foundLast.id)
            if (paths[0]) setDirectory(paths[0])
        } else if (props.machines[0]) {
            setMachineId(props.machines[0].id)
        }
    }, [props.machines, machineId, getLastUsedMachineId, getRecentPaths])

    const recentPaths = useMemo(
        () => getRecentPaths(machineId),
        [getRecentPaths, machineId]
    )

    const allPaths = useDirectorySuggestions(machineId, sessions, recentPaths)
    const pathExistence = useMachinePathsExists(props.api, machineId, allPaths)

    const verifiedPaths = useMemo(
        () => allPaths.filter((path) => pathExistence[path]),
        [allPaths, pathExistence]
    )

    const suggestions = useMemo<Suggestion[]>(() => {
        if (!isDirectoryFocused || suppressSuggestions || !showDirectorySuggestions) {
            return []
        }

        const lowered = directory.trim().toLowerCase()
        if (!lowered) {
            return []
        }

        return verifiedPaths
            .filter((path) => path.toLowerCase().includes(lowered))
            .slice(0, 8)
            .map((path) => ({
                key: path,
                text: path,
                label: path
            }))
    }, [directory, isDirectoryFocused, showDirectorySuggestions, suppressSuggestions, verifiedPaths])

    useEffect(() => {
        setSelectedSuggestionIndex((previous) => {
            if (suggestions.length === 0) {
                return -1
            }
            if (previous < 0) {
                return -1
            }
            if (previous >= suggestions.length) {
                return suggestions.length - 1
            }
            return previous
        })
    }, [suggestions])

    const moveSuggestionUp = useCallback(() => {
        setSelectedSuggestionIndex((previous) => {
            if (suggestions.length === 0) {
                return -1
            }
            if (previous <= 0) {
                return suggestions.length - 1
            }
            return previous - 1
        })
    }, [suggestions.length])

    const moveSuggestionDown = useCallback(() => {
        setSelectedSuggestionIndex((previous) => {
            if (suggestions.length === 0) {
                return -1
            }
            if (previous < 0 || previous >= suggestions.length - 1) {
                return 0
            }
            return previous + 1
        })
    }, [suggestions.length])

    const clearSuggestions = useCallback(() => {
        setSelectedSuggestionIndex(-1)
        setShowDirectorySuggestions(false)
    }, [])

    const permissionChoiceOptions = useMemo(
        () => getPermissionChoiceOptions(agent, t),
        [agent, t]
    )

    const directoryTrimmed = directory.trim()
    const {
        exists: directoryExists,
        isChecking: isCheckingDirectory,
        error: directoryCheckError,
    } = useMachinePathExists(props.api, machineId, directoryTrimmed)

    const directoryValidation = useMemo((): {
        message: string | null
        tone: 'muted' | 'success' | 'error'
    } => {
        if (!machineId || !directoryTrimmed) {
            return { message: null, tone: 'muted' }
        }
        if (isCheckingDirectory) {
            return { message: t('newSession.directoryChecking'), tone: 'muted' }
        }
        if (directoryCheckError) {
            return { message: t('newSession.directoryCheckFailed'), tone: 'error' }
        }
        if (directoryExists === false) {
            return { message: t('newSession.directoryNotFound'), tone: 'error' }
        }
        if (directoryExists === true) {
            return { message: t('newSession.directoryExists'), tone: 'success' }
        }
        return { message: null, tone: 'muted' }
    }, [directoryCheckError, directoryExists, directoryTrimmed, isCheckingDirectory, machineId, t])

    const handleMachineChange = useCallback((newMachineId: string) => {
        setMachineId(newMachineId)
        setSelectedSuggestionIndex(-1)
        setShowDirectorySuggestions(false)
        setError(null)
        const paths = getRecentPaths(newMachineId)
        if (paths[0]) {
            setDirectory(paths[0])
        } else {
            setDirectory('')
        }
    }, [getRecentPaths])

    const handlePathClick = useCallback((path: string) => {
        setDirectory(path)
        setError(null)
        setSelectedSuggestionIndex(-1)
        setShowDirectorySuggestions(false)
        setSuppressSuggestions(true)
    }, [])

    const handleBrowseDirectory = useCallback(() => {
        setDirectoryDialogOpen(true)
    }, [])

    const handleDirectoryPicked = useCallback((path: string) => {
        setDirectory(path)
        setError(null)
        setSelectedSuggestionIndex(-1)
        setShowDirectorySuggestions(false)
        setSuppressSuggestions(true)
    }, [])

    const handleSuggestionSelect = useCallback((index: number) => {
        const suggestion = suggestions[index]
        if (suggestion) {
            setDirectory(suggestion.text)
            setError(null)
            clearSuggestions()
            setSuppressSuggestions(true)
        }
    }, [suggestions, clearSuggestions])

    const handleDirectoryChange = useCallback((value: string) => {
        setSuppressSuggestions(false)
        setShowDirectorySuggestions(true)
        setSelectedSuggestionIndex(-1)
        setError(null)
        setDirectory(value)
    }, [])

    const handleDirectoryFocus = useCallback(() => {
        setSuppressSuggestions(false)
        setShowDirectorySuggestions(false)
        setSelectedSuggestionIndex(-1)
        setIsDirectoryFocused(true)
    }, [])

    const handleDirectoryBlur = useCallback(() => {
        setIsDirectoryFocused(false)
        setShowDirectorySuggestions(false)
        setSelectedSuggestionIndex(-1)
    }, [])

    const handleDirectoryKeyDown = useCallback((event: ReactKeyboardEvent<HTMLInputElement>) => {
        if (suggestions.length === 0) return

        if (event.key === 'ArrowUp') {
            event.preventDefault()
            moveSuggestionUp()
        }

        if (event.key === 'ArrowDown') {
            event.preventDefault()
            moveSuggestionDown()
        }

        if (event.key === 'Enter' || event.key === 'Tab') {
            if (selectedSuggestionIndex >= 0) {
                event.preventDefault()
                handleSuggestionSelect(selectedSuggestionIndex)
            }
        }

        if (event.key === 'Escape') {
            clearSuggestions()
        }
    }, [suggestions.length, selectedSuggestionIndex, moveSuggestionUp, moveSuggestionDown, clearSuggestions, handleSuggestionSelect])

    async function handleCreate() {
        if (!machineId || !directoryTrimmed) return
        if (isCheckingDirectory || directoryExists === false) return

        setError(null)
        try {
            const resolvedModel = model !== 'auto' && agent !== 'opencode' ? model : undefined
            const result = await spawnSession({
                machineId,
                directory: directoryTrimmed,
                agent,
                model: resolvedModel,
                permissionMode: permissionChoice
            })

            if (result.type === 'success') {
                haptic.notification('success')
                setLastUsedMachineId(machineId)
                addRecentPath(machineId, directoryTrimmed)
                props.onSuccess(result.sessionId)
                return
            }

            haptic.notification('error')
            setError(result.message)
        } catch (e) {
            haptic.notification('error')
            setError(e instanceof Error ? e.message : 'Failed to create session')
        }
    }

    const canCreate = Boolean(
        machineId
        && directoryTrimmed
        && !isFormDisabled
        && !isCheckingDirectory
        && directoryExists !== false
    )

    return (
        <div className="flex flex-col divide-y divide-[var(--app-divider)]">
            <MachineSelector
                machines={props.machines}
                machineId={machineId}
                isLoading={props.isLoading}
                isDisabled={isFormDisabled}
                onChange={handleMachineChange}
            />
            <DirectorySection
                directory={directory}
                suggestions={suggestions}
                selectedIndex={selectedSuggestionIndex}
                isDisabled={isFormDisabled}
                recentPaths={recentPaths}
                validationMessage={directoryValidation.message}
                validationTone={directoryValidation.tone}
                onDirectoryChange={handleDirectoryChange}
                onDirectoryFocus={handleDirectoryFocus}
                onDirectoryBlur={handleDirectoryBlur}
                onDirectoryKeyDown={handleDirectoryKeyDown}
                onSuggestionSelect={handleSuggestionSelect}
                onPathClick={handlePathClick}
                onBrowse={handleBrowseDirectory}
            />
            <AgentSelector
                agent={agent}
                isDisabled={isFormDisabled}
                onAgentChange={setAgent}
            />
            <ModelSelector
                agent={agent}
                model={model}
                isDisabled={isFormDisabled}
                onModelChange={setModel}
            />
            <PermissionModeSelector
                value={permissionChoice}
                options={permissionChoiceOptions}
                isDisabled={isFormDisabled}
                onChange={setPermissionChoice}
            />

            {(error ?? spawnError) ? (
                <div className="px-3 py-2 text-sm text-red-600">
                    {error ?? spawnError}
                </div>
            ) : null}

            <ActionButtons
                isPending={isPending}
                canCreate={canCreate}
                isDisabled={isFormDisabled}
                onCancel={props.onCancel}
                onCreate={handleCreate}
            />
            <DirectoryPickerDialog
                api={props.api}
                isOpen={directoryDialogOpen}
                machineId={machineId}
                initialPath={directory}
                onClose={() => setDirectoryDialogOpen(false)}
                onSelect={handleDirectoryPicked}
            />
        </div>
    )
}
