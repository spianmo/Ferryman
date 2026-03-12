import { useContext, type ReactNode } from 'react'
import { ThemeContext as FerrymanThemeContext, ThemeProvider as FerrymanThemeProvider } from '../../theme'

export function ThemeProvider({ children }: { children: ReactNode }) {
  const ferrymanTheme = useContext(FerrymanThemeContext)
  if (ferrymanTheme) {
    return <>{children}</>
  }

  return <FerrymanThemeProvider>{children}</FerrymanThemeProvider>
}
