package org.localgrid.gridwatch.ui

import android.Manifest
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import org.localgrid.gridwatch.link.LinkData
import org.localgrid.gridwatch.link.LinkLog
import org.localgrid.gridwatch.watch.LinkHub
import org.localgrid.gridwatch.watch.LinkView
import java.text.DateFormat
import java.util.Date

/**
 * What the app tried on the admin link and what came back, on screen.
 *
 * A wrong guess in an error message ("the AP may be busy") twice sent people to look at healthy
 * APs, so the steps are now readable in the app: which AP was tried and how loud it was, the MTU
 * agreed, the GATT status of anything that failed. Nothing sensitive is in here — no address, no
 * password, no grid content.
 */
@Composable
fun ConnectionLog(link: LinkView, nowMs: Long) {
    // Recomposes with the rest of the screen each second; the revision keeps it honest.
    val revision = LinkLog.revision
    val lines = remember(revision, nowMs / 1000) { LinkLog.lines() }
    var open by remember { mutableStateOf(false) }
    val interesting = link.state == LinkData.State.ERROR || link.state == LinkData.State.NEEDS_PERMISSION

    if (link.state == LinkData.State.NEEDS_PERMISSION) {
        PermissionCard(link)
    }
    if (lines.isEmpty()) return

    Section("Connection log") {
        Hint(
            "What this phone tried and what came back. Useful when an AP will not answer; there " +
                "is nothing private in it.",
        )
        Spacer(Modifier.height(8.dp))
        val fmt = remember { DateFormat.getTimeInstance(DateFormat.MEDIUM) }
        val shown = if (open || interesting) lines.take(24) else lines.take(4)
        for (l in shown) {
            Text(
                fmt.format(Date(l.atMs)) + "  " + l.text,
                color = Lg.muted, fontSize = 12.sp, fontFamily = FontFamily.Monospace,
                modifier = Modifier.padding(vertical = 1.dp),
            )
        }
        if (lines.size > shown.size) {
            Text(
                if (open) "Show less" else "Show all ${lines.size} lines",
                color = Lg.accent, fontSize = 13.sp,
                modifier = Modifier.padding(top = 6.dp).clickable { open = !open },
            )
        }
    }
}

/** The one failure a person can fix in a tap, so it gets its own card and a button. */
@Composable
private fun PermissionCard(link: LinkView) {
    val ctx = LocalContext.current
    val launcher = rememberLauncherForActivityResult(ActivityResultContracts.RequestPermission()) {
        LinkHub.refresh(ctx, now = true)
    }
    Card {
        SectionTitle("Nearby devices")
        Spacer(Modifier.height(6.dp))
        Text(link.message, color = Lg.warn, fontSize = 14.sp)
        Notice(
            "The app still never joins the grid's Wi-Fi and still cannot send anything into it.",
            Lg.muted,
            "Allow",
        ) { launcher.launch(Manifest.permission.BLUETOOTH_CONNECT) }
    }
}

