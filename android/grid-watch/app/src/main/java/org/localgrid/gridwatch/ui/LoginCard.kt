package org.localgrid.gridwatch.ui

import android.Manifest
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.text.KeyboardActions
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.material3.Button
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.input.ImeAction
import androidx.compose.ui.text.input.PasswordVisualTransformation
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import org.localgrid.gridwatch.link.AdminAuth
import org.localgrid.gridwatch.link.LinkData
import org.localgrid.gridwatch.watch.LinkHub
import org.localgrid.gridwatch.watch.LinkView

/**
 * The admin password, asked for when the user opens a tab that needs it (D70).
 *
 * It is held in memory only unless the user switches "remember on this phone" on, and even then
 * it is sealed with the same Android Keystore protection as the pairing key. The password never
 * crosses the air: the phone derives PBKDF2 and sends one HMAC proof. Nothing here is logged.
 */
@Composable
fun LoginCard(link: LinkView, tab: WatchTab) {
    val ctx = LocalContext.current
    var password by remember { mutableStateOf("") }
    var remember by remember { mutableStateOf(link.remembered) }
    var askedConnect by remember { mutableStateOf(false) }

    val connectLauncher = rememberLauncherForActivityResult(
        ActivityResultContracts.RequestPermission(),
    ) { granted ->
        if (granted && password.isNotEmpty()) submit(ctx, password, remember).also { password = "" }
    }

    fun go() {
        if (password.isEmpty()) return
        if (!LinkHub.canConnect(ctx) && !askedConnect) {
            askedConnect = true
            connectLauncher.launch(Manifest.permission.BLUETOOTH_CONNECT)
            return
        }
        submit(ctx, password, remember)
        password = ""
    }

    Card {
        SectionTitle("Admin password")
        Spacer(Modifier.height(6.dp))
        Text(
            "The ${tab.label} tab shows what the AP's own admin page shows, so it needs the same " +
                "password. Positions, groups, availability, what happened to the APs and the " +
                "traffic counters stay hidden until it works. The beacon view on Overview needs none.",
            color = Lg.muted, fontSize = 14.sp,
        )
        Spacer(Modifier.height(10.dp))
        OutlinedTextField(
            value = password,
            onValueChange = { password = it },
            singleLine = true,
            label = { Text("Admin password") },
            visualTransformation = PasswordVisualTransformation(),
            keyboardOptions = KeyboardOptions(imeAction = ImeAction.Go),
            keyboardActions = KeyboardActions(onGo = { go() }),
            modifier = Modifier.fillMaxWidth(),
        )
        Row(
            Modifier.fillMaxWidth().padding(top = 8.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            Switch(checked = remember, onCheckedChange = { remember = it })
            Spacer(Modifier.height(0.dp))
            Text(
                "  Remember on this phone",
                color = Lg.text, fontSize = 14.sp, modifier = Modifier.weight(1f),
            )
            Button(onClick = { go() }, enabled = password.isNotEmpty()) { Text("Log in") }
        }
        Text(
            if (remember) {
                "Kept in this app's private storage, sealed by a key in the Android Keystore, as the " +
                    "pairing key is. It never leaves the phone."
            } else {
                "Kept in memory only: it is gone when the app stops."
            },
            color = Lg.muted, fontSize = 13.sp, modifier = Modifier.padding(top = 6.dp),
        )
        if (link.authState == AdminAuth.State.FAILED || link.authState == AdminAuth.State.TRYING) {
            Spacer(Modifier.height(8.dp))
            Text(
                link.authMessage,
                color = if (link.authState == AdminAuth.State.FAILED) Lg.danger else Lg.accent,
                fontSize = 14.sp,
            )
            // Why it has not got anywhere yet: not watching, no AP in range, the AP busy with
            // another watcher. Without this the card would just say "checking" for ever.
            if (link.state == LinkData.State.ERROR && link.message != link.authMessage) {
                Text(link.message, color = Lg.warn, fontSize = 13.sp,
                    modifier = Modifier.padding(top = 4.dp))
            }
        }
        if (!LinkHub.canConnect(ctx)) {
            Notice(
                "Logging in asks Android for the Nearby devices permission again, this time to " +
                    "connect to an AP. The app still never joins the grid's Wi-Fi.",
                Lg.muted,
            )
        }
    }
}

/** "Log out": the password and everything pulled with it go at once. */
@Composable
fun LogOutRow() {
    val ctx = LocalContext.current
    Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.End) {
        TextButton(onClick = { LinkHub.logOut(ctx) }) { Text("Log out") }
    }
}

private fun submit(ctx: android.content.Context, password: String, remember: Boolean) {
    LinkHub.logIn(ctx, password.toCharArray(), remember)
}
