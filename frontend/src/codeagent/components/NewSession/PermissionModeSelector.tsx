import type { NewSessionPermissionChoice, NewSessionPermissionChoiceOption } from './permissionChoices'
import { useTranslation } from '@/lib/use-translation'

export function PermissionModeSelector(props: {
    value: NewSessionPermissionChoice
    options: NewSessionPermissionChoiceOption[]
    isDisabled: boolean
    onChange: (value: NewSessionPermissionChoice) => void
}) {
    const { t } = useTranslation()
    const selected = props.options.find((option) => option.value === props.value)

    return (
        <div className="flex flex-col gap-1.5 px-3 py-3">
            <label className="text-xs font-medium text-[var(--app-hint)]">
                {t('misc.permissionMode')}
            </label>
            <select
                value={props.value}
                onChange={(e) => props.onChange(e.target.value as NewSessionPermissionChoice)}
                disabled={props.isDisabled}
                className="w-full rounded-lg border border-[var(--app-divider)] bg-[var(--app-bg)] px-3 py-2 text-sm text-[var(--app-text)] focus:outline-none focus:ring-2 focus:ring-[var(--app-link)] disabled:opacity-50"
            >
                {props.options.map((option) => (
                    <option key={option.value} value={option.value}>
                        {option.label}
                    </option>
                ))}
            </select>
            {selected?.description ? (
                <p className="text-xs text-[var(--app-hint)]">
                    {selected.description}
                </p>
            ) : null}
        </div>
    )
}
