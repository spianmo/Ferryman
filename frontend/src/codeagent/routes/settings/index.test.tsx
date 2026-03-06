import { describe, it, expect, vi, beforeEach } from 'vitest'
import { render, screen } from '@testing-library/react'
import { I18nContext, I18nProvider } from '@/lib/i18n-context'
import { en } from '@/lib/locales'
import SettingsPage from './index'

// Mock the router hooks
vi.mock('@tanstack/react-router', () => ({
    useNavigate: () => vi.fn(),
    useRouter: () => ({ history: { back: vi.fn() } }),
    useLocation: () => '/settings',
}))

// Mock useFontScale hook
vi.mock('@/hooks/useFontScale', () => ({
    useFontScale: () => ({ fontScale: 1, setFontScale: vi.fn() }),
    getFontScaleOptions: () => [
        { value: 0.875, label: '87.5%' },
        { value: 1, label: '100%' },
        { value: 1.125, label: '112.5%' },
    ],
}))

// Mock languages
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
        // Mock localStorage
        const localStorageMock = {
            getItem: vi.fn(() => 'en'),
            setItem: vi.fn(),
            removeItem: vi.fn(),
        }
        Object.defineProperty(window, 'localStorage', { value: localStorageMock })
    })

    it('renders the Voice Assistant section', () => {
        renderWithProviders(<SettingsPage />)
        expect(screen.getByText('Voice Assistant')).toBeInTheDocument()
    })

    it('does not render About section content', () => {
        renderWithProviders(<SettingsPage />)
        expect(screen.queryByText('About')).not.toBeInTheDocument()
        expect(screen.queryByText('Website')).not.toBeInTheDocument()
        expect(screen.queryByText('App Version')).not.toBeInTheDocument()
        expect(screen.queryByText('Protocol Version')).not.toBeInTheDocument()
    })

    it('uses correct i18n keys for voice section', () => {
        const spyT = renderWithSpyT(<SettingsPage />)
        const calledKeys = spyT.mock.calls.map((call) => call[0])
        expect(calledKeys).toContain('settings.voice.title')
        expect(calledKeys).toContain('settings.voice.language')
        expect(calledKeys).toContain('settings.voice.autoDetect')
        expect(calledKeys).not.toContain('settings.language.title')
        expect(calledKeys).not.toContain('settings.display.title')
        expect(calledKeys).not.toContain('settings.about.title')
    })
})
