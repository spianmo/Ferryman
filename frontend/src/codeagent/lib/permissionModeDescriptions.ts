import type { PermissionMode } from '@hapi/protocol'
import type { I18nContextValue } from '@/lib/use-translation'

const PERMISSION_MODE_DESCRIPTION_KEYS: Partial<Record<PermissionMode, string>> = {
  default: 'permissionMode.description.default',
  acceptEdits: 'permissionMode.description.acceptEdits',
  plan: 'permissionMode.description.plan',
  bypassPermissions: 'permissionMode.description.bypassPermissions',
  ask: 'permissionMode.description.ask',
  'read-only': 'permissionMode.description.readOnly',
  auto: 'permissionMode.description.auto',
  'full-access': 'permissionMode.description.fullAccess',
  'auto-edit': 'permissionMode.description.autoEdit',
  yolo: 'permissionMode.description.yolo',
  allow: 'permissionMode.description.allow',
  deny: 'permissionMode.description.deny',
  agent: 'permissionMode.description.agent',
  force: 'permissionMode.description.force',
}

export function getPermissionModeDescriptionText(
  t: I18nContextValue['t'],
  mode: PermissionMode
): string | undefined {
  const key = PERMISSION_MODE_DESCRIPTION_KEYS[mode]
  if (!key) {
    return undefined
  }
  return t(key)
}
