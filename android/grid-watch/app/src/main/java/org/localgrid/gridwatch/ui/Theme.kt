package org.localgrid.gridwatch.ui

import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.darkColorScheme
import androidx.compose.runtime.Composable
import androidx.compose.ui.graphics.Color

/** Theme tokens from the grid_watch dashboard and the AP admin page (D10). */
object Lg {
    val bg = Color(0xFF0B1310)
    val surface = Color(0xFF111D18)
    val line = Color(0xFF1F3329)
    val text = Color(0xFFD7EFE0)
    val muted = Color(0xFF86A596)
    val accent = Color(0xFF5FD38D)
    val accentInk = Color(0xFF06120C)
    val warn = Color(0xFFF0B64A)
    val warnInk = Color(0xFF1C1302)
    val danger = Color(0xFFEF6B6B)
    val dangerInk = Color(0xFF1A0505)
}

private val scheme = darkColorScheme(
    primary = Lg.accent,
    onPrimary = Lg.accentInk,
    secondary = Lg.muted,
    onSecondary = Lg.bg,
    background = Lg.bg,
    onBackground = Lg.text,
    surface = Lg.surface,
    onSurface = Lg.text,
    surfaceVariant = Lg.surface,
    onSurfaceVariant = Lg.muted,
    surfaceContainer = Lg.surface,
    surfaceContainerHigh = Lg.surface,
    surfaceContainerHighest = Lg.line,
    outline = Lg.line,
    outlineVariant = Lg.line,
    error = Lg.danger,
    onError = Lg.dangerInk,
)

@Composable
fun GridWatchTheme(content: @Composable () -> Unit) {
    MaterialTheme(colorScheme = scheme, content = content)
}
