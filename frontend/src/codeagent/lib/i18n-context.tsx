import { createContext, useContext, useMemo, type ReactNode } from 'react'
import {
  CODEAGENT_I18N_PREFIX,
  I18nContext as FerrymanI18nContext,
  I18nProvider as FerrymanI18nProvider,
  useI18n,
  type Lang,
} from '../../i18n'

export type Locale = Lang

export type I18nContextValue = {
  t: (key: string, params?: Record<string, string | number>) => string
  locale: Locale
  setLocale: (locale: Locale) => void
}

export const I18nContext = createContext<I18nContextValue | null>(null)

function ScopedCodeAgentI18nProvider({ children }: { children: ReactNode }) {
  const { lang, setLang, t: ferrymanT } = useI18n()
  const t = useMemo<I18nContextValue['t']>(() => {
    return (key, params) => {
      const scopedKey = `${CODEAGENT_I18N_PREFIX}${key}`
      const scopedValue = ferrymanT(scopedKey, params)
      if (scopedValue !== scopedKey) {
        return scopedValue
      }
      return ferrymanT(key, params)
    }
  }, [ferrymanT])

  const value = useMemo<I18nContextValue>(() => {
    return {
      t,
      locale: lang,
      setLocale: setLang,
    }
  }, [lang, setLang, t])

  return (
    <I18nContext.Provider value={value}>
      {children}
    </I18nContext.Provider>
  )
}

export function I18nProvider({ children }: { children: ReactNode }) {
  const ferrymanContext = useContext(FerrymanI18nContext)
  if (ferrymanContext) {
    return <ScopedCodeAgentI18nProvider>{children}</ScopedCodeAgentI18nProvider>
  }

  return (
    <FerrymanI18nProvider>
      <ScopedCodeAgentI18nProvider>{children}</ScopedCodeAgentI18nProvider>
    </FerrymanI18nProvider>
  )
}
