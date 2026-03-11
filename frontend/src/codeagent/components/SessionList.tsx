import { useEffect, useMemo, useState } from 'react'
import type { SessionSummary } from '@/types/api'
import type { ApiClient } from '@/api/client'
import { useLongPress } from '@/hooks/useLongPress'
import { usePlatform } from '@/hooks/usePlatform'
import { useSessionActions } from '@/hooks/mutations/useSessionActions'
import { SessionActionMenu } from '@/components/SessionActionMenu'
import { RenameSessionDialog } from '@/components/RenameSessionDialog'
import { ConfirmDialog } from '@/components/ui/ConfirmDialog'
import { useTranslation } from '@/lib/use-translation'
import { isExternalSessionSummary } from '@/lib/external-sessions'

type SessionGroup = {
    directory: string
    displayName: string
    sessions: SessionSummary[]
    latestUpdatedAt: number
    hasActiveSession: boolean
}

type SessionSourceGroup = {
    key: 'codeagent' | 'external'
    title: string
    sessions: SessionSummary[]
    groups: SessionGroup[]
}

function getGroupDisplayName(directory: string): string {
    if (directory === 'Other') return directory
    const parts = directory.split(/[\\/]+/).filter(Boolean)
    if (parts.length === 0) return directory
    if (parts.length === 1) return parts[0]
    return `${parts[parts.length - 2]}/${parts[parts.length - 1]}`
}

function groupSessionsByDirectory(sessions: SessionSummary[]): SessionGroup[] {
    const groups = new Map<string, SessionSummary[]>()

    sessions.forEach(session => {
        const path = session.metadata?.worktree?.basePath ?? session.metadata?.path ?? 'Other'
        if (!groups.has(path)) {
            groups.set(path, [])
        }
        groups.get(path)!.push(session)
    })

    return Array.from(groups.entries())
        .map(([directory, groupSessions]) => {
            const sortedSessions = [...groupSessions].sort((a, b) => {
                const rankA = a.active ? (a.pendingRequestsCount > 0 ? 0 : 1) : 2
                const rankB = b.active ? (b.pendingRequestsCount > 0 ? 0 : 1) : 2
                if (rankA !== rankB) return rankA - rankB
                return b.updatedAt - a.updatedAt
            })
            const latestUpdatedAt = groupSessions.reduce(
                (max, s) => (s.updatedAt > max ? s.updatedAt : max),
                -Infinity
            )
            const hasActiveSession = groupSessions.some(s => s.active)
            const displayName = getGroupDisplayName(directory)

            return { directory, displayName, sessions: sortedSessions, latestUpdatedAt, hasActiveSession }
        })
        .sort((a, b) => {
            if (a.hasActiveSession !== b.hasActiveSession) {
                return a.hasActiveSession ? -1 : 1
            }
            return b.latestUpdatedAt - a.latestUpdatedAt
        })
}


function groupSessionsBySource(
    sessions: SessionSummary[],
    t: (key: string, params?: Record<string, string | number>) => string
): SessionSourceGroup[] {
    const codeagentSessions = sessions.filter((session) => !isExternalSessionSummary(session))
    const externalSessions = sessions.filter((session) => isExternalSessionSummary(session))
    const groups: SessionSourceGroup[] = []

    if (codeagentSessions.length > 0) {
        groups.push({
            key: 'codeagent',
            title: t('sessions.source.codeagent'),
            sessions: codeagentSessions,
            groups: groupSessionsByDirectory(codeagentSessions),
        })
    }

    if (externalSessions.length > 0) {
        groups.push({
            key: 'external',
            title: t('sessions.source.external'),
            sessions: externalSessions,
            groups: groupSessionsByDirectory(externalSessions),
        })
    }

    return groups
}

function PlusIcon(props: { className?: string }) {
    return (
        <svg
            xmlns="http://www.w3.org/2000/svg"
            width="24"
            height="24"
            viewBox="0 0 24 24"
            fill="none"
            stroke="currentColor"
            strokeWidth="2"
            strokeLinecap="round"
            strokeLinejoin="round"
            className={props.className}
        >
            <line x1="12" y1="5" x2="12" y2="19" />
            <line x1="5" y1="12" x2="19" y2="12" />
        </svg>
    )
}

function BulbIcon(props: { className?: string }) {
    return (
        <svg
            xmlns="http://www.w3.org/2000/svg"
            width="24"
            height="24"
            viewBox="0 0 24 24"
            fill="none"
            stroke="currentColor"
            strokeWidth="2"
            strokeLinecap="round"
            strokeLinejoin="round"
            className={props.className}
        >
            <path d="M9 18h6" />
            <path d="M10 22h4" />
            <path d="M12 2a7 7 0 0 0-4 12c.6.6 1 1.2 1 2h6c0-.8.4-1.4 1-2a7 7 0 0 0-4-12Z" />
        </svg>
    )
}

function ChevronIcon(props: { className?: string; collapsed?: boolean }) {
    return (
        <svg
            xmlns="http://www.w3.org/2000/svg"
            width="16"
            height="16"
            viewBox="0 0 24 24"
            fill="none"
            stroke="currentColor"
            strokeWidth="2"
            strokeLinecap="round"
            strokeLinejoin="round"
            className={`${props.className ?? ''} transition-transform duration-200 ${props.collapsed ? '' : 'rotate-90'}`}
        >
            <polyline points="9 18 15 12 9 6" />
        </svg>
    )
}

function getSessionTitle(session: SessionSummary): string {
    if (session.metadata?.name) {
        return session.metadata.name
    }
    if (session.metadata?.summary?.text) {
        return session.metadata.summary.text
    }
    if (session.metadata?.path) {
        const parts = session.metadata.path.split('/').filter(Boolean)
        return parts.length > 0 ? parts[parts.length - 1] : session.id.slice(0, 8)
    }
    return session.id.slice(0, 8)
}

function getTodoProgress(session: SessionSummary): { completed: number; total: number } | null {
    if (!session.todoProgress) return null
    if (session.todoProgress.completed === session.todoProgress.total) return null
    return session.todoProgress
}

function getAgentLabel(session: SessionSummary): string {
    const flavor = session.metadata?.flavor?.trim()
    if (flavor) return flavor
    return 'unknown'
}

function isReadOnlyLocalCliSession(sessionId: string, active: boolean): boolean {
    return sessionId.startsWith('external-') && !active
}

function formatRelativeTime(value: number, t: (key: string, params?: Record<string, string | number>) => string): string | null {
    const ms = value < 1_000_000_000_000 ? value * 1000 : value
    if (!Number.isFinite(ms)) return null
    const delta = Date.now() - ms
    if (delta < 60_000) return t('session.time.justNow')
    const minutes = Math.floor(delta / 60_000)
    if (minutes < 60) return t('session.time.minutesAgo', { n: minutes })
    const hours = Math.floor(minutes / 60)
    if (hours < 24) return t('session.time.hoursAgo', { n: hours })
    const days = Math.floor(hours / 24)
    if (days < 7) return t('session.time.daysAgo', { n: days })
    return new Date(ms).toLocaleDateString()
}

function SessionItem(props: {
    session: SessionSummary
    onSelect: (sessionId: string) => void
    showPath?: boolean
    api: ApiClient | null
    selected?: boolean
}) {
    const { t } = useTranslation()
    const { session: s, onSelect, showPath = true, api, selected = false } = props
    const { haptic } = usePlatform()
    const canManageSession = !isReadOnlyLocalCliSession(s.id, s.active)
    const [menuOpen, setMenuOpen] = useState(false)
    const [menuAnchorPoint, setMenuAnchorPoint] = useState<{ x: number; y: number }>({ x: 0, y: 0 })
    const [renameOpen, setRenameOpen] = useState(false)
    const [archiveOpen, setArchiveOpen] = useState(false)
    const [deleteOpen, setDeleteOpen] = useState(false)

    const { archiveSession, renameSession, deleteSession, isPending } = useSessionActions(
        api,
        s.id,
        s.metadata?.flavor ?? null
    )

    const longPressHandlers = useLongPress({
        onLongPress: (point) => {
            if (!canManageSession) return
            haptic.impact('medium')
            setMenuAnchorPoint(point)
            setMenuOpen(true)
        },
        onClick: () => {
            if (!menuOpen) {
                onSelect(s.id)
            }
        },
        threshold: 500
    })

    const sessionName = getSessionTitle(s)
    const statusDotClass = s.active
        ? (s.thinking ? 'bg-[#007AFF]' : 'bg-[var(--app-badge-success-text)]')
        : 'bg-[var(--app-hint)]'
    return (
        <>
            <button
                type="button"
                {...longPressHandlers}
                className={`session-list-item flex w-full flex-col gap-1.5 px-3 py-3 text-left transition-colors focus-visible:outline-none focus-visible:ring-2 focus-visible:ring-[var(--app-link)] select-none ${selected ? 'bg-[var(--app-secondary-bg)]' : ''}`}
                style={{ WebkitTouchCallout: 'none' }}
                aria-current={selected ? 'page' : undefined}
            >
                <div className="flex items-center justify-between gap-3">
                    <div className="flex items-center gap-2 min-w-0">
                        <span className="flex h-4 w-4 items-center justify-center" aria-hidden="true">
                            <span
                                className={`h-2 w-2 rounded-full ${statusDotClass}`}
                            />
                        </span>
                        <div className="truncate text-base font-medium">
                            {sessionName}
                        </div>
                    </div>
                    <div className="flex items-center gap-2 shrink-0 text-xs">
                        {s.thinking ? (
                            <span className="text-[#007AFF] animate-pulse">
                                {t('session.item.thinking')}
                            </span>
                        ) : null}
                        {(() => {
                            const progress = getTodoProgress(s)
                            if (!progress) return null
                            return (
                                <span className="flex items-center gap-1 text-[var(--app-hint)]">
                                    <BulbIcon className="h-3 w-3" />
                                    {progress.completed}/{progress.total}
                                </span>
                            )
                        })()}
                        {s.pendingRequestsCount > 0 ? (
                            <span className="text-[var(--app-badge-warning-text)]">
                                {t('session.item.pending')} {s.pendingRequestsCount}
                            </span>
                        ) : null}
                        <span className="text-[var(--app-hint)]">
                            {formatRelativeTime(s.updatedAt, t)}
                        </span>
                    </div>
                </div>
                {showPath ? (
                    <div className="truncate text-xs text-[var(--app-hint)]">
                        {s.metadata?.path ?? s.id}
                    </div>
                ) : null}
                <div className="flex flex-wrap items-center gap-x-3 gap-y-1 text-xs text-[var(--app-hint)]">
                    <span className="inline-flex items-center gap-2">
                        <span className="flex h-4 w-4 items-center justify-center" aria-hidden="true">
                            ❖
                        </span>
                        {getAgentLabel(s)}
                    </span>
                    <span>{t('session.item.modelMode')}: {s.modelMode || 'default'}</span>
                </div>
            </button>

            {canManageSession ? (
                <>
                    <SessionActionMenu
                        isOpen={menuOpen}
                        onClose={() => setMenuOpen(false)}
                        sessionActive={s.active}
                        onRename={() => setRenameOpen(true)}
                        onArchive={() => setArchiveOpen(true)}
                        onDelete={() => setDeleteOpen(true)}
                        anchorPoint={menuAnchorPoint}
                    />

                    <RenameSessionDialog
                        isOpen={renameOpen}
                        onClose={() => setRenameOpen(false)}
                        currentName={sessionName}
                        onRename={renameSession}
                        isPending={isPending}
                    />

                    <ConfirmDialog
                        isOpen={archiveOpen}
                        onClose={() => setArchiveOpen(false)}
                        title={t('dialog.archive.title')}
                        description={t('dialog.archive.description', { name: sessionName })}
                        confirmLabel={t('dialog.archive.confirm')}
                        confirmingLabel={t('dialog.archive.confirming')}
                        onConfirm={archiveSession}
                        isPending={isPending}
                        destructive
                    />

                    <ConfirmDialog
                        isOpen={deleteOpen}
                        onClose={() => setDeleteOpen(false)}
                        title={t('dialog.delete.title')}
                        description={t('dialog.delete.description', { name: sessionName })}
                        confirmLabel={t('dialog.delete.confirm')}
                        confirmingLabel={t('dialog.delete.confirming')}
                        onConfirm={deleteSession}
                        isPending={isPending}
                        destructive
                    />
                </>
            ) : null}
        </>
    )
}

export function SessionList(props: {
    sessions: SessionSummary[]
    onSelect: (sessionId: string) => void
    onNewSession: () => void
    onRefresh: () => void
    isLoading: boolean
    renderHeader?: boolean
    api: ApiClient | null
    selectedSessionId?: string | null
}) {
    const { t } = useTranslation()
    const { renderHeader = true, api, selectedSessionId } = props
    const sourceGroups = useMemo(
        () => groupSessionsBySource(props.sessions, t),
        [props.sessions, t]
    )
    const totalProjectGroups = useMemo(
        () => sourceGroups.reduce((count, group) => count + group.groups.length, 0),
        [sourceGroups]
    )
    const [collapseOverrides, setCollapseOverrides] = useState<Map<string, boolean>>(
        () => new Map()
    )
    const isGroupCollapsed = (groupKey: string, hasActiveSession: boolean): boolean => {
        const override = collapseOverrides.get(groupKey)
        if (override !== undefined) return override
        return !hasActiveSession
    }

    const toggleGroup = (groupKey: string, isCollapsed: boolean) => {
        setCollapseOverrides(prev => {
            const next = new Map(prev)
            next.set(groupKey, !isCollapsed)
            return next
        })
    }

    useEffect(() => {
        setCollapseOverrides(prev => {
            if (prev.size === 0) return prev
            const next = new Map(prev)
            const knownGroups = new Set(
                sourceGroups.flatMap((group) => group.groups.map((directoryGroup) => `${group.key}:${directoryGroup.directory}`))
            )
            let changed = false
            for (const groupKey of next.keys()) {
                if (!knownGroups.has(groupKey)) {
                    next.delete(groupKey)
                    changed = true
                }
            }
            return changed ? next : prev
        })
    }, [sourceGroups])

    return (
        <div className="mx-auto w-full max-w-content flex flex-col">
            {renderHeader ? (
                <div className="flex items-center justify-between px-3 py-1">
                    <div className="text-xs text-[var(--app-hint)]">
                        {t('sessions.count', { n: props.sessions.length, m: totalProjectGroups })}
                    </div>
                    <button
                        type="button"
                        onClick={props.onNewSession}
                        className="session-list-new-button p-1.5 rounded-full text-[var(--app-link)] transition-colors"
                        title={t('sessions.new')}
                    >
                        <PlusIcon className="h-5 w-5" />
                    </button>
                </div>
            ) : null}

            <div className="flex flex-col">
                {sourceGroups.map((sourceGroup) => (
                    <section key={sourceGroup.key} className="flex flex-col">
                        <div className="px-3 py-2 text-xs font-semibold text-[var(--app-hint)] uppercase tracking-wide border-b border-[var(--app-divider)]">
                            {sourceGroup.title}
                            <span className="ml-2 text-[var(--app-hint)] normal-case">({sourceGroup.sessions.length})</span>
                        </div>
                        {sourceGroup.groups.map((group) => {
                            const groupKey = `${sourceGroup.key}:${group.directory}`
                            const isCollapsed = isGroupCollapsed(groupKey, group.hasActiveSession)
                            return (
                                <div key={groupKey}>
                                    <button
                                        type="button"
                                        onClick={() => toggleGroup(groupKey, isCollapsed)}
                                        className="sticky top-0 z-10 flex w-full items-center gap-2 px-3 py-2 text-left bg-[var(--app-bg)] border-b border-[var(--app-divider)] transition-colors hover:bg-[var(--app-secondary-bg)]"
                                    >
                                        <ChevronIcon
                                            className="h-4 w-4 text-[var(--app-hint)]"
                                            collapsed={isCollapsed}
                                        />
                                        <div className="flex items-center gap-2 min-w-0 flex-1">
                                            <span className="font-medium text-base break-words" title={group.directory}>
                                                {group.displayName}
                                            </span>
                                            <span className="shrink-0 text-xs text-[var(--app-hint)]">
                                                ({group.sessions.length})
                                            </span>
                                        </div>
                                    </button>
                                    {!isCollapsed ? (
                                        <div className="flex flex-col divide-y divide-[var(--app-divider)] border-b border-[var(--app-divider)]">
                                            {group.sessions.map((session) => (
                                                <SessionItem
                                                    key={session.id}
                                                    session={session}
                                                    onSelect={props.onSelect}
                                                    showPath={false}
                                                    api={api}
                                                    selected={session.id === selectedSessionId}
                                                />
                                            ))}
                                        </div>
                                    ) : null}
                                </div>
                            )
                        })}
                    </section>
                ))}
            </div>
        </div>
    )
}
