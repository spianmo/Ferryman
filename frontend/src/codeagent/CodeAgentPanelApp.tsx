import { useEffect, useMemo } from 'react'
import { QueryClientProvider } from '@tanstack/react-query'
import { RouterProvider, createMemoryHistory } from '@tanstack/react-router'
import { I18nProvider } from './lib/i18n-context'
import { ThemeProvider } from './lib/theme-context'
import { createAppRouter } from './router'
import { queryClient } from './lib/query-client'
import { initializeFontScale } from './hooks/useFontScale'
import { CODEAGENT_PORTAL_ROOT_ID, CODEAGENT_ROOT_ID } from './lib/dom'
import './index.css'

export default function CodeAgentPanelApp() {
  if (typeof window !== 'undefined') {
    window.__FERRYMAN_CODEAGENT_EMBEDDED__ = true
  }

  const router = useMemo(() => {
    const history = createMemoryHistory({ initialEntries: ['/sessions'] })
    return createAppRouter(history)
  }, [])

  useEffect(() => {
    initializeFontScale()
  }, [])

  useEffect(() => () => {
    delete window.__FERRYMAN_CODEAGENT_EMBEDDED__
  }, [])

  return (
    <div
      id={CODEAGENT_ROOT_ID}
      className="ferryman-codeagent-root h-full min-h-0 overflow-hidden"
    >
      <ThemeProvider>
        <I18nProvider>
          <QueryClientProvider client={queryClient}>
            <RouterProvider router={router} />
          </QueryClientProvider>
        </I18nProvider>
      </ThemeProvider>
      <div id={CODEAGENT_PORTAL_ROOT_ID} />
    </div>
  )
}
