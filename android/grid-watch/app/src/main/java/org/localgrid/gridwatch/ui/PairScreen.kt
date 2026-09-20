package org.localgrid.gridwatch.ui

import android.Manifest
import android.content.pm.PackageManager
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Button
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.material3.TopAppBar
import androidx.compose.material3.TopAppBarDefaults
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.input.ImeAction
import androidx.compose.ui.text.input.KeyboardCapitalization
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.core.content.ContextCompat
import com.journeyapps.barcodescanner.ScanContract
import com.journeyapps.barcodescanner.ScanOptions
import org.localgrid.gridwatch.proto.Pairing
import org.localgrid.gridwatch.proto.PairingCode
import org.localgrid.gridwatch.watch.GridHub

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun PairScreen(pairing: Pairing?, onDone: () -> Unit) {
    val ctx = LocalContext.current
    var text by rememberSaveable { mutableStateOf("") }
    var error by rememberSaveable { mutableStateOf<String?>(null) }
    var confirmForget by remember { mutableStateOf(false) }

    fun accept(code: String) {
        when (val r = PairingCode.parse(code)) {
            is PairingCode.Bad -> error = r.message
            is PairingCode.Ok -> {
                error = null
                text = ""
                GridHub.pair(ctx, r.pairing)
                onDone()
            }
        }
    }

    val scan = rememberLauncherForActivityResult(ScanContract()) { result ->
        val contents = result.contents
        if (contents != null) accept(contents)
    }
    fun launchScan() {
        scan.launch(
            ScanOptions()
                .setDesiredBarcodeFormats(ScanOptions.QR_CODE)
                .setPrompt("Point the camera at the pairing code on the laptop")
                .setBeepEnabled(false)
                .setOrientationLocked(false),
        )
    }
    var cameraDenied by remember { mutableStateOf(false) }
    val camera = rememberLauncherForActivityResult(ActivityResultContracts.RequestPermission()) { ok ->
        cameraDenied = !ok
        if (ok) launchScan()
    }

    Scaffold(
        containerColor = Lg.bg,
        topBar = {
            TopAppBar(
                title = { Text(if (pairing == null) "Pair with a grid" else "This grid") },
                colors = TopAppBarDefaults.topAppBarColors(containerColor = Lg.bg, titleContentColor = Lg.text),
                navigationIcon = {
                    if (pairing != null) TextButton(onClick = onDone) { Text("Back") }
                },
            )
        },
    ) { pad ->
        Column(
            Modifier.fillMaxSize().padding(pad).verticalScroll(rememberScrollState()).padding(horizontal = 16.dp, vertical = 8.dp),
            verticalArrangement = Arrangement.spacedBy(12.dp),
        ) {
            if (pairing != null) {
                Text("Paired with grid ${pairing.gridId}", fontSize = 18.sp, fontWeight = FontWeight.SemiBold)
                Text("This phone holds only the key that reads the grid's status. It cannot join the grid, " +
                    "send messages, or change anything.", color = Lg.muted, fontSize = 14.sp)
                OutlinedButton(
                    onClick = { confirmForget = true },
                    colors = ButtonDefaults.outlinedButtonColors(contentColor = Lg.danger),
                ) { Text("Forget this grid") }
                Spacer(Modifier.height(8.dp))
                Text("Pair with another grid instead", fontWeight = FontWeight.SemiBold)
            } else {
                Text("LocalGrid Watch shows how an offline network is doing, from its APs' Bluetooth " +
                    "status beacon, without joining the grid.", fontSize = 15.sp)
            }
            Text("On the laptop that holds this grid's secrets, run\n    python tools/grid_watch.py --pair\n" +
                "and scan the code it shows. The code is the key to the grid's status: show it only to phones you trust.",
                color = Lg.muted, fontSize = 14.sp)
            Button(onClick = {
                if (ContextCompat.checkSelfPermission(ctx, Manifest.permission.CAMERA) == PackageManager.PERMISSION_GRANTED) {
                    launchScan()
                } else {
                    camera.launch(Manifest.permission.CAMERA)
                }
            }, modifier = Modifier.fillMaxWidth()) { Text("Scan the QR code") }
            if (cameraDenied) {
                Text("The camera is needed only to read the pairing code, and only while the scanner is open. " +
                    "You can paste the code below instead.", color = Lg.warn, fontSize = 14.sp)
            }
            Text("Or paste the code (it starts with LGW1:)", color = Lg.muted, fontSize = 14.sp)
            OutlinedTextField(
                value = text,
                onValueChange = { text = it; error = null },
                modifier = Modifier.fillMaxWidth(),
                placeholder = { Text("LGW1:…") },
                textStyle = androidx.compose.ui.text.TextStyle(fontFamily = FontFamily.Monospace, fontSize = 14.sp),
                keyboardOptions = KeyboardOptions(capitalization = KeyboardCapitalization.None, autoCorrectEnabled = false, imeAction = ImeAction.Done),
                singleLine = false,
                minLines = 2,
            )
            OutlinedButton(onClick = { accept(text) }, enabled = text.isNotBlank()) { Text("Pair") }
            error?.let { Text(it, color = Lg.danger, fontSize = 14.sp) }
        }
    }

    if (confirmForget) {
        AlertDialog(
            onDismissRequest = { confirmForget = false },
            title = { Text("Forget this grid?") },
            text = { Text("Watching stops and the key is deleted from this phone. To watch again, scan the pairing code again.") },
            confirmButton = {
                TextButton(onClick = {
                    confirmForget = false
                    GridHub.forget(ctx)
                }) { Text("Forget", color = Lg.danger) }
            },
            dismissButton = { TextButton(onClick = { confirmForget = false }) { Text("Cancel") } },
        )
    }
}
