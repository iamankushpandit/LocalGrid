package org.localgrid.gridwatch.ui

import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.SystemBarStyle
import androidx.activity.compose.BackHandler
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import androidx.compose.runtime.Composable
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.graphics.toArgb
import org.localgrid.gridwatch.watch.GridHub

class MainActivity : ComponentActivity() {
    private var autoStart = false

    override fun onCreate(savedInstanceState: Bundle?) {
        enableEdgeToEdge(
            statusBarStyle = SystemBarStyle.dark(Lg.bg.toArgb()),
            navigationBarStyle = SystemBarStyle.dark(Lg.bg.toArgb()),
        )
        super.onCreate(savedInstanceState)
        GridHub.init(this)
        autoStart = savedInstanceState == null
        setContent { GridWatchTheme { App() } }
    }

    override fun onStart() {
        super.onStart()
        GridHub.visible.value = true                   // the service scans at low latency now
        // Watching was on when the app was last used: carry on (the app is visible, so Android
        // allows the foreground service to start).
        if (autoStart) {
            autoStart = false
            if (GridHub.wantWatch(this) && !GridHub.watching.value) GridHub.startWatching(this)
        }
    }

    override fun onStop() {
        GridHub.visible.value = false                  // back to a balanced scan
        super.onStop()
    }
}

@Composable
private fun App() {
    val pairing by GridHub.pairing.collectAsState()
    var showPair by rememberSaveable { mutableStateOf(false) }
    if (pairing == null || showPair) {
        BackHandler(enabled = pairing != null) { showPair = false }
        PairScreen(pairing = pairing, onDone = { showPair = false })
    } else {
        Dashboard(onOpenPairing = { showPair = true })
    }
}
