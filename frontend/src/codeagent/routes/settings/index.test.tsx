import { describe, it, expect, vi, beforeEach } from 'vitest'
import { fireEvent, render, screen } from '@testing-library/react'
import { I18nContext, I18nProvider } from '@/lib/i18n-context'
import { en } from '@/lib/locales'
import SettingsPage from './index'

vi.mock('@tanstack/react-router', () => ({
    useNavigate: () => vi.fn(),
    useRouter: () => ({ history: { back: vi.fn() } }),
    useLocation: () => '/settings',
}))

vi.mock('@/hooks/useFontScale', () => ({
    useFontScale: () => ({ fontScale: 1, setFontScale: vi.fn() }),
    getFontScaleOptions: () => [
        { value: 0.875, label: '87.5%' },
        { value: 1, label: '100%' },
        { value: 1.125, label: '112.5%' },
    ],
}))

vi.mock('@/lib/languages', () => ({
    getElevenLabsSupportedLanguages: () => [
        { code: null, name: 'Auto-detect' },
        { code: 'en', name: 'English' },
    ],
    getLanguageDisplayName: (lang: { code: string | null; name: string }) => lang.name,
}))

function renderWithProviders(ui: React.ReactElement) {
    return render(
        <I18nProvider>
            {ui}
        </I18nProvider>
    )
}

function renderWithSpyT(ui: React.ReactElement) {
    const translations = en as Record<string, string>
    const spyT = vi.fn((key: string) => translations[key] ?? key)
    render(
        <I18nContext.Provider value={{ t: spyT, locale: 'en', setLocale: vi.fn() }}>
            {ui}
        </I18nContext.Provider>
    )
    return spyT
}

describe('SettingsPage', () => {
    beforeEach(() => {
        vi.clearAllMocks()
        const storage = new Map<string, string>([['hapi-voice-lang', 'en']])
        const localStorageMock = {
            getItem: vi.fn((key: string) => storage.get(key) ?? null),
            setItem: vi.fn((key: string, value: string) => {
                storage.set(key, value)
            }),
            removeItem: vi.fn((key: string) => {
                storage.delete(key)
            }),
        }
        Object.defineProperty(window, 'localStorage', { value: localStorageMock, configurable: true })
    })

    it('renders the Voice Assistant section', () => {
        renderWithProviders(<SettingsPage />)
        expect(screen.getByText('Voice Assistant')).toBeInTheDocument()
    })

    it('renders the sessions section and external session toggle', () => {
        renderWithProviders(<SettingsPage />)
        expect(screen.getByText('Sessions')).toBeInTheDocument()
        expect(screen.getByRole('switch', { name: 'Read External Sessions' })).toHaveAttribute('aria-checked', 'true')
    })

    it('toggles external session reading in localStorage', () => {
        renderWithProviders(<SettingsPage />)
        const toggle = screen.getByRole('switch', { name: 'Read External Sessions' })

        fireEvent.click(toggle)
        expect(window.localStorage.setItem).toHaveBeenCalledWith('hapi-codeagent-read-external-sessions', 'false')
        expect(toggle).toHaveAttribute('aria-checked', 'false')

        fireEvent.click(toggle)
        expect(window.localStorage.removeItem).toHaveBeenCalledWith('hapi-codeagent-read-external-sessions')
        expect(toggle).toHaveAttribute('aria-checked', 'true')
    })

    it('does not render About section content', () => {
        renderWithProviders(<SettingsPage />)
        expect(screen.queryByText('About')).not.toBeInTheDocument()
        expect(screen.queryByText('Website')).not.toBeInTheDocument()
        expect(screen.queryByText('App Version')).not.toBeInTheDocument()
        expect(screen.queryByText('Protocol Version')).not.toBeInTheDocument()
    })

    it('uses correct i18n keys for settings sections', () => {
        const spyT = renderWithSpyT(<SettingsPage />)
        const calledKeys = spyT.mock.calls.map((call) => call[0])
        expect(calledKeys).toContain('settings.voice.title')
        expect(calledKeys).toContain('settings.voice.language')
        expect(calledKeys).toContain('settings.voice.autoDetect')
        expect(calledKeys).toContain('settings.sessions.title')
        expect(calledKeys).toContain('settings.sessions.readExternal')
        expect(calledKeys).toContain('settings.sessions.readExternal.description')
        expect(calledKeys).not.toContain('settings.language.title')
        expect(calledKeys).not.toContain('settings.display.title')
        expect(calledKeys).not.toContain('settings.about.title')
    })
})
