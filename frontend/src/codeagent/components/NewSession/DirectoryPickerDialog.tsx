import { useCallback, useEffect, useMemo, useRef, useState } from 'react'
import { FiChevronLeft, FiFolder, FiRefreshCw } from 'react-icons/fi'
import type { ApiClient } from '@/api/client'
import type { DirectoryEntry } from '@/types/api'
import { Button } from '@/components/ui/button'
import {
    Dialog,
    DialogContent,
    DialogDescription,
    DialogHeader,
    DialogTitle,
} from '@/components/ui/dialog'
import { useTranslation } from '@/lib/use-translation'

function joinDirectoryPath(base: string, name: string): string {
    if (!base) return name
    if (base.endsWith('/') || base.endsWith('\\')) {
        return `${base}${name}`
    }
    const useWindowsSeparator = base.includes('\\') && !base.includes('/')
    if (useWindowsSeparator && /^[A-Za-z]:$/.test(base)) {
        return `${base}\\${name}`
    }
    return `${base}${useWindowsSeparator ? '\\' : '/'}${name}`
}

function parentDirectoryPath(path: string): string {
    const trimmed = path.replace(/[\\/]+$/, '')
    if (!trimmed) return path
    if (trimmed === '/' || trimmed === '\\' || /^[A-Za-z]:$/.test(trimmed)) {
        return trimmed
    }

    const slashPos = Math.max(trimmed.lastIndexOf('/'), trimmed.lastIndexOf('\\'))
    if (slashPos < 0) {
        return trimmed
    }
    if (slashPos === 0) {
        return trimmed.startsWith('\\') ? '\\' : '/'
    }

    return trimmed.slice(0, slashPos)
}

function trimPath(path: string): string {
    const value = path.trim()
    if (!value) {
        return ''
    }
    if (value === '/' || value === '\\') {
        return value
    }
    if (/^[A-Za-z]:[\\/]?$/.test(value)) {
        return value.slice(0, 2)
    }
    return value.replace(/[\\/]+$/, '')
}

function buildBreadcrumbs(path: string): Array<{ label: string; path: string }> {
    const normalized = trimPath(path)
    if (!normalized) {
        return []
    }
    if (normalized === '/' || normalized === '\\') {
        return [{ label: '/', path: '/' }]
    }

    const windowsDriveMatch = normalized.match(/^[A-Za-z]:/)
    if (windowsDriveMatch) {
        const drive = windowsDriveMatch[0]
        const rest = normalized.slice(drive.length).replace(/^[\\/]+/, '')
        const parts = rest ? rest.split(/[\\/]+/).filter(Boolean) : []
        const items: Array<{ label: string; path: string }> = [{ label: drive, path: drive }]
        let current = drive
        for (const part of parts) {
            current = `${current}\\${part}`
            items.push({ label: part, path: current })
        }
        return items
    }

    const unixPath = normalized.replace(/\\/g, '/')
    const isAbsolute = unixPath.startsWith('/')
    const parts = unixPath.split('/').filter(Boolean)
    if (parts.length === 0 && isAbsolute) {
        return [{ label: '/', path: '/' }]
    }

    const items: Array<{ label: string; path: string }> = []
    let current = isAbsolute ? '' : '.'
    if (isAbsolute) {
        items.push({ label: '/', path: '/' })
    }
    for (const part of parts) {
        current = isAbsolute ? `${current}/${part}` : `${current}/${part}`
        items.push({ label: part, path: current })
    }
    return items
}

export function DirectoryPickerDialog(props: {
    api: ApiClient
    isOpen: boolean
    machineId: string | null
    initialPath: string
    onClose: () => void
    onSelect: (path: string) => void
}) {
    const { t } = useTranslation()
    const [currentPath, setCurrentPath] = useState('')
    const [pathInput, setPathInput] = useState('')
    const [entries, setEntries] = useState<DirectoryEntry[]>([])
    const [loading, setLoading] = useState(false)
    const [error, setError] = useState<string | null>(null)
    const requestIdRef = useRef(0)

    const directories = useMemo(
        () => entries.filter((entry) => entry.type === 'directory'),
        [entries]
    )
    const breadcrumbs = useMemo(() => buildBreadcrumbs(currentPath), [currentPath])

    const loadDirectory = useCallback(async (targetPath?: string) => {
        if (!props.machineId) {
            setCurrentPath('')
            setPathInput('')
            setEntries([])
            setError(t('newSession.dirDialog.needMachine'))
            return
        }

        const requestId = ++requestIdRef.current
        setLoading(true)
        setError(null)
        try {
            const normalized = trimPath(targetPath ?? '')
            const result = await props.api.listMachineDirectory(
                props.machineId,
                normalized ? normalized : undefined
            )
            if (requestId !== requestIdRef.current) {
                return
            }
            if (!result.success) {
                setEntries([])
                setError(result.error ?? t('newSession.dirDialog.loadFailed'))
                return
            }

            const resolvedPath = result.path ?? normalized
            const nextPath = trimPath(resolvedPath)
            setCurrentPath(nextPath)
            setPathInput(nextPath)
            setEntries(result.entries ?? [])
        } catch (e) {
            if (requestId !== requestIdRef.current) {
                return
            }
            setEntries([])
            setError(e instanceof Error ? e.message : t('newSession.dirDialog.loadFailed'))
        } finally {
            if (requestId === requestIdRef.current) {
                setLoading(false)
            }
        }
    }, [props.api, props.machineId, t])

    useEffect(() => {
        if (!props.isOpen) {
            return
        }
        const preferredPath = trimPath(props.initialPath)
        void loadDirectory(preferredPath || undefined)
    }, [props.isOpen, props.machineId, props.initialPath, loadDirectory])

    const handleGo = useCallback(() => {
        void loadDirectory(pathInput || undefined)
    }, [loadDirectory, pathInput])

    const handleRefresh = useCallback(() => {
        void loadDirectory(currentPath || undefined)
    }, [currentPath, loadDirectory])

    const handleUp = useCallback(() => {
        const next = parentDirectoryPath(currentPath)
        if (!next || next === currentPath) {
            return
        }
        void loadDirectory(next)
    }, [currentPath, loadDirectory])

    const handleEnter = useCallback((entry: DirectoryEntry) => {
        if (entry.type !== 'directory') {
            return
        }
        const next = joinDirectoryPath(currentPath, entry.name)
        void loadDirectory(next)
    }, [currentPath, loadDirectory])

    const handleSelectCurrent = useCallback(() => {
        if (!currentPath) {
            return
        }
        props.onSelect(currentPath)
        props.onClose()
    }, [currentPath, props])

    return (
        <Dialog open={props.isOpen} onOpenChange={(open) => !open && props.onClose()}>
            <DialogContent className="max-w-3xl p-0">
                <div className="flex h-[560px] flex-col rounded-xl bg-white/95 p-4 shadow-soft ring-1 ring-slate-200/80 dark:bg-neutral-900/95 dark:ring-neutral-800/80">
                    <DialogHeader className="pb-3">
                        <DialogTitle>{t('newSession.dirDialog.title')}</DialogTitle>
                        <DialogDescription>{t('newSession.dirDialog.description')}</DialogDescription>
                        {breadcrumbs.length > 0 ? (
                            <div className="mt-1 flex flex-wrap items-center gap-1 font-mono text-xs text-slate-500 dark:text-neutral-400">
                                {breadcrumbs.map((item, index) => (
                                    <div key={`${item.path}-${index}`} className="flex items-center gap-1">
                                        {index > 0 ? <span>/</span> : null}
                                        <button
                                            type="button"
                                            className="rounded px-1 py-0.5 transition-colors hover:bg-slate-200/70 hover:text-slate-700 dark:hover:bg-neutral-800 dark:hover:text-neutral-100"
                                            onClick={() => void loadDirectory(item.path)}
                                            title={item.path}
                                        >
                                            {item.label}
                                        </button>
                                    </div>
                                ))}
                            </div>
                        ) : null}
                    </DialogHeader>

                    <div className="flex items-center gap-2">
                        <button
                            type="button"
                            className="grid h-10 w-10 place-items-center rounded-2xl bg-slate-100 text-slate-700 transition-colors hover:bg-slate-200 disabled:opacity-50 dark:bg-neutral-800 dark:text-neutral-100 dark:hover:bg-neutral-700"
                            onClick={handleUp}
                            disabled={loading || !currentPath}
                            title={t('newSession.remoteUp')}
                        >
                            <FiChevronLeft />
                        </button>
                        <button
                            type="button"
                            className="grid h-10 w-10 place-items-center rounded-2xl bg-slate-100 text-slate-700 transition-colors hover:bg-slate-200 disabled:opacity-50 dark:bg-neutral-800 dark:text-neutral-100 dark:hover:bg-neutral-700"
                            onClick={handleRefresh}
                            disabled={loading || !props.machineId}
                            title={t('newSession.remoteRefresh')}
                        >
                            <FiRefreshCw />
                        </button>
                        <input
                            value={pathInput}
                            onChange={(event) => setPathInput(event.target.value)}
                            onKeyDown={(event) => {
                                if (event.key === 'Enter') {
                                    event.preventDefault()
                                    handleGo()
                                }
                            }}
                            disabled={loading || !props.machineId}
                            className="h-10 w-full rounded-2xl border border-slate-200 bg-white px-3 py-2 text-sm text-slate-900 shadow-sm outline-none placeholder:text-slate-400 focus:border-slate-300 dark:border-neutral-700 dark:bg-neutral-950/40 dark:text-neutral-50 dark:placeholder:text-neutral-500 dark:focus:border-neutral-600"
                            placeholder={t('newSession.placeholder')}
                        />
                        <Button
                            type="button"
                            onClick={handleGo}
                            disabled={loading || !props.machineId}
                            className="h-10 rounded-2xl px-4"
                        >
                            {t('newSession.dirDialog.go')}
                        </Button>
                    </div>

                    <div
                        className="mt-2 truncate rounded-lg border border-slate-200 bg-white/80 px-2.5 py-1.5 font-mono text-xs text-slate-600 dark:border-neutral-700 dark:bg-neutral-950/40 dark:text-neutral-300"
                        title={currentPath || '-'}
                    >
                        {currentPath || '-'}
                    </div>

                    {error ? (
                        <div className="mt-2 rounded-md bg-red-500/10 px-3 py-2 text-xs text-red-600 dark:text-red-400">
                            {error}
                        </div>
                    ) : null}

                    <div className="mt-3 min-h-0 flex-1 overflow-auto space-y-1 p-1">
                        {loading ? (
                            Array.from({ length: 6 }).map((_, index) => (
                                <div
                                    key={`dir-skeleton-${index}`}
                                    className="h-11 animate-pulse rounded-2xl bg-slate-100 dark:bg-neutral-800/60"
                                />
                            ))
                        ) : directories.length === 0 ? (
                            <div className="rounded-2xl border border-dashed border-slate-200 p-5 text-center text-sm text-slate-500 dark:border-neutral-700 dark:text-neutral-400">
                                {t('newSession.dirDialog.empty')}
                            </div>
                        ) : (
                            directories.map((entry) => (
                                <button
                                    key={`dir:${entry.name}`}
                                    type="button"
                                    className="flex w-full items-center gap-3 rounded-2xl border border-transparent bg-white/70 px-3 py-2 text-left text-sm text-slate-800 shadow-sm ring-1 ring-slate-200/60 transition-colors hover:bg-slate-50 dark:bg-neutral-950/30 dark:text-neutral-50 dark:ring-neutral-800/70 dark:hover:bg-neutral-900/60"
                                    onClick={() => handleEnter(entry)}
                                >
                                    <span className="text-base text-slate-600 dark:text-neutral-300">
                                        <FiFolder />
                                    </span>
                                    <span className="min-w-0 flex-1 truncate font-semibold" title={entry.name}>
                                        {entry.name}
                                    </span>
                                    <span className="shrink-0 rounded-full bg-white/70 px-2 py-0.5 font-mono text-[11px] text-slate-500 dark:bg-neutral-900/60 dark:text-neutral-400">
                                        dir
                                    </span>
                                </button>
                            ))
                        )}
                    </div>

                    <div className="mt-3 flex items-center justify-end gap-2 border-t border-slate-200 pt-3 dark:border-neutral-800">
                        <Button
                            type="button"
                            variant="secondary"
                            onClick={props.onClose}
                        >
                            {t('button.cancel')}
                        </Button>
                        <Button
                            type="button"
                            onClick={handleSelectCurrent}
                            disabled={!currentPath || loading}
                        >
                            {t('newSession.dirDialog.selectCurrent')}
                        </Button>
                    </div>
                </div>
            </DialogContent>
        </Dialog>
    )
}
