package org.localgrid.gridwatch.ui

import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxHeight
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp

// ---------------------------------------------------------------- words for numbers
//
// The same wording the laptop dashboard uses (tools/grid_watch.py), so a grid looks the same
// whoever is watching it.

fun ago(s: Long?): String = when {
    s == null -> "never"
    s < 60 -> "$s s ago"
    s < 3600 -> "${s / 60} min ago"
    else -> "${s / 3600} h ${s % 3600 / 60} min ago"
}

fun forText(s: Long?) = ago(s).removeSuffix(" ago")

fun fmtDur(s: Long?): String {
    if (s == null || s < 0) return "--"
    val d = s / 86400
    val h = s % 86400 / 3600
    val m = s % 3600 / 60
    return when {
        d > 0 -> "$d d $h h"
        h > 0 -> "$h h $m min"
        m > 0 -> "$m min"
        else -> "$s s"
    }
}

fun fmtBytes(b: Long?): String = when {
    b == null -> "--"
    b >= 1048576 -> String.format(java.util.Locale.US, "%.1f MB", b / 1048576.0)
    b >= 1024 -> "${(b / 1024.0).toLong()} KB"
    else -> "$b B"
}

fun fmtKB(b: Long?): String = when {
    b == null -> "--"
    b >= 1048576 -> String.format(java.util.Locale.US, "%.1f MB", b / 1048576.0)
    else -> "${Math.round(b / 1024.0)} KB"
}

// ---------------------------------------------------------------- building blocks

@Composable
fun Card(modifier: Modifier = Modifier, content: @Composable () -> Unit) {
    Column(
        modifier
            .fillMaxWidth()
            .clip(RoundedCornerShape(10.dp))
            .background(Lg.surface)
            .border(1.dp, Lg.line, RoundedCornerShape(10.dp))
            .padding(14.dp),
    ) { content() }
}

@Composable
fun SectionTitle(title: String) {
    Text(
        title.uppercase(), color = Lg.muted, fontSize = 13.sp, letterSpacing = 1.5.sp,
        fontWeight = FontWeight.Medium,
    )
}

@Composable
fun Section(title: String, source: String? = null, content: @Composable () -> Unit) {
    Card {
        SectionTitle(title)
        if (source != null) {
            Spacer(Modifier.height(2.dp))
            Text(source, color = Lg.muted, fontSize = 13.sp)
        }
        Spacer(Modifier.height(8.dp))
        content()
    }
}

@Composable
fun Hint(text: String, color: Color = Lg.muted) = Text(text, color = color, fontSize = 14.sp)

@Composable
fun SubHeading(text: String) {
    Text(text, color = Lg.text, fontSize = 15.sp, fontWeight = FontWeight.SemiBold,
        modifier = Modifier.padding(top = 12.dp, bottom = 4.dp))
}

@Composable
fun Pill(text: String, bg: Color, fg: Color) {
    Text(
        text, color = fg, fontSize = 12.sp, fontWeight = FontWeight.SemiBold,
        modifier = Modifier.clip(RoundedCornerShape(50)).background(bg)
            .padding(horizontal = 8.dp, vertical = 1.dp),
    )
}

@Composable
fun Field(label: String, value: String, color: Color? = null) {
    Row(Modifier.fillMaxWidth().padding(vertical = 1.dp)) {
        Text(label, color = Lg.muted, fontSize = 14.sp,
            modifier = Modifier.widthIn(min = 96.dp).padding(end = 12.dp))
        Text(value, color = color ?: Lg.text, fontSize = 14.sp, modifier = Modifier.weight(1f))
    }
}

@Composable
fun Divider() = Box(Modifier.fillMaxWidth().height(1.dp).background(Lg.line))

@Composable
fun BatteryBar(pct: Int) {
    val color = if (pct < 20) Lg.danger else if (pct < 40) Lg.warn else Lg.accent
    Box(
        Modifier.width(56.dp).height(10.dp).clip(RoundedCornerShape(5.dp)).background(Lg.bg)
            .border(1.dp, Lg.line, RoundedCornerShape(5.dp)),
    ) {
        Box(Modifier.fillMaxHeight().fillMaxWidth(pct.coerceIn(0, 100) / 100f).background(color))
    }
}

/** One number with its label, as the dashboard's stat tiles. */
@Composable
fun Stat(label: String, value: String, modifier: Modifier = Modifier, color: Color = Lg.text) {
    Column(modifier.padding(end = 18.dp, bottom = 8.dp)) {
        Text(label, color = Lg.muted, fontSize = 13.sp)
        Text(value, color = color, fontSize = 18.sp, fontWeight = FontWeight.SemiBold,
            fontFamily = FontFamily.Monospace)
    }
}

/** A plain-English line about something over its limit: red when it matters, amber when it may. */
@Composable
fun NoteLine(text: String, red: Boolean) {
    Row(Modifier.fillMaxWidth().padding(vertical = 2.dp), verticalAlignment = Alignment.Top) {
        Text("•  ", color = if (red) Lg.danger else Lg.warn, fontSize = 14.sp)
        Text(text, color = if (red) Lg.danger else Lg.warn, fontSize = 14.sp)
    }
}

/** A table row of cells with weights; the first is the name and the rest are numbers. */
@Composable
fun TableRow(cells: List<Pair<String, Float>>, header: Boolean = false, colors: List<Color?> = emptyList()) {
    Row(Modifier.fillMaxWidth().padding(vertical = 5.dp), verticalAlignment = Alignment.CenterVertically) {
        cells.forEachIndexed { i, (text, weight) ->
            Text(
                text,
                color = colors.getOrNull(i) ?: if (header) Lg.muted else Lg.text,
                fontSize = if (header) 13.sp else 14.sp,
                fontFamily = if (!header && i > 0) FontFamily.Monospace else FontFamily.Default,
                modifier = Modifier.weight(weight).padding(end = 6.dp),
            )
        }
    }
}
