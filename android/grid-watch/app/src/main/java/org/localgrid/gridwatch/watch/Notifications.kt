package org.localgrid.gridwatch.watch

import android.Manifest
import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.content.Context
import android.content.Intent
import android.content.pm.PackageManager
import android.media.AudioAttributes
import android.media.RingtoneManager
import android.os.Build
import androidx.core.app.NotificationCompat
import androidx.core.app.NotificationManagerCompat
import androidx.core.content.ContextCompat
import org.localgrid.gridwatch.R
import org.localgrid.gridwatch.grid.GridEvent
import org.localgrid.gridwatch.ui.MainActivity

object Notifications {
    const val CH_WATCH = "watch"
    const val CH_SOS = "sos"
    const val CH_ALERTS = "alerts"
    const val ID_WATCH = 1
    private const val ID_ALERT_BASE = 1000

    /** An all clear older than this, heard on starting to watch, is history, not news. */
    private const val ALL_CLEAR_NEWS_S = 15 * 60

    private val VIBRATE = longArrayOf(0, 600, 250, 600, 250, 600, 250, 1200)
    private val sosSeen = HashSet<Int>()

    fun createChannels(context: Context) {
        val nm = context.getSystemService(NotificationManager::class.java)
        val watch = NotificationChannel(CH_WATCH, context.getString(R.string.channel_watch),
            NotificationManager.IMPORTANCE_LOW).apply {
            description = context.getString(R.string.channel_watch_desc)
            setShowBadge(false)
        }
        val sos = NotificationChannel(CH_SOS, context.getString(R.string.channel_sos),
            NotificationManager.IMPORTANCE_HIGH).apply {
            description = context.getString(R.string.channel_sos_desc)
            enableVibration(true)
            vibrationPattern = VIBRATE
            enableLights(true)
            lightColor = 0xFFEF6B6B.toInt()
            setSound(RingtoneManager.getDefaultUri(RingtoneManager.TYPE_ALARM),
                AudioAttributes.Builder().setUsage(AudioAttributes.USAGE_ALARM)
                    .setContentType(AudioAttributes.CONTENT_TYPE_SONIFICATION).build())
            lockscreenVisibility = Notification.VISIBILITY_PUBLIC
        }
        val alerts = NotificationChannel(CH_ALERTS, context.getString(R.string.channel_alerts),
            NotificationManager.IMPORTANCE_DEFAULT).apply {
            description = context.getString(R.string.channel_alerts_desc)
        }
        nm.createNotificationChannels(listOf(watch, sos, alerts))
    }

    private fun openApp(context: Context): PendingIntent = PendingIntent.getActivity(
        context, 0,
        Intent(context, MainActivity::class.java).addFlags(Intent.FLAG_ACTIVITY_SINGLE_TOP or Intent.FLAG_ACTIVITY_CLEAR_TOP),
        PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT,
    )

    fun watchText(context: Context, apsHeard: Int, sos: Boolean): String {
        val base = context.resources.getQuantityString(R.plurals.watching_aps, apsHeard, apsHeard)
        return if (sos) base + context.getString(R.string.watching_sos_suffix) else base
    }

    fun watching(context: Context, text: String): Notification {
        val stop = PendingIntent.getService(
            context, 1, Intent(context, WatchService::class.java).setAction(WatchService.ACTION_STOP),
            PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT,
        )
        return NotificationCompat.Builder(context, CH_WATCH)
            .setSmallIcon(R.drawable.ic_stat_grid)
            .setContentTitle(text)
            .setContentText(context.getString(R.string.watching_detail))
            .setOngoing(true)
            .setOnlyAlertOnce(true)
            .setSilent(true)
            .setPriority(NotificationCompat.PRIORITY_LOW)
            .setCategory(NotificationCompat.CATEGORY_SERVICE)
            .setForegroundServiceBehavior(NotificationCompat.FOREGROUND_SERVICE_IMMEDIATE)
            .setContentIntent(openApp(context))
            .addAction(0, context.getString(R.string.stop_watching), stop)
            .build()
    }

    private fun canNotify(context: Context): Boolean {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU &&
            ContextCompat.checkSelfPermission(context, Manifest.permission.POST_NOTIFICATIONS) !=
            PackageManager.PERMISSION_GRANTED) return false
        return NotificationManagerCompat.from(context).areNotificationsEnabled()
    }

    /** Turns a grid event into a notification when it is an SOS or an all clear. */
    fun onGridEvent(context: Context, e: GridEvent) {
        val dev = e.device ?: return
        when (e.kind) {
            "alert" -> {
                sosSeen += dev
                post(context, ID_ALERT_BASE + dev, NotificationCompat.Builder(context, CH_SOS)
                    .setSmallIcon(R.drawable.ic_stat_grid)
                    .setContentTitle(context.getString(R.string.sos_title, e.name ?: "Handheld $dev"))
                    .setContentText(context.getString(R.string.sos_text, e.near ?: "?", (e.ageS ?: 0) / 60))
                    .setPriority(NotificationCompat.PRIORITY_MAX)
                    .setCategory(NotificationCompat.CATEGORY_ALARM)
                    .setColor(0xFFEF6B6B.toInt())
                    .setVibrate(VIBRATE)
                    .setVisibility(NotificationCompat.VISIBILITY_PUBLIC)
                    .setAutoCancel(true)
                    .setContentIntent(openApp(context))
                    .build())
            }
            "all_clear" -> {
                if (dev !in sosSeen && (e.ageS ?: Int.MAX_VALUE) > ALL_CLEAR_NEWS_S) return
                sosSeen -= dev
                post(context, ID_ALERT_BASE + dev, NotificationCompat.Builder(context, CH_ALERTS)
                    .setSmallIcon(R.drawable.ic_stat_grid)
                    .setContentTitle(context.getString(R.string.all_clear_title, e.name ?: "Handheld $dev"))
                    .setContentText(context.getString(R.string.all_clear_text))
                    .setColor(0xFF5FD38D.toInt())
                    .setAutoCancel(true)
                    .setContentIntent(openApp(context))
                    .build())
            }
        }
    }

    private fun post(context: Context, id: Int, n: Notification) {
        if (!canNotify(context)) return
        try {
            NotificationManagerCompat.from(context).notify(id, n)
        } catch (_: SecurityException) {
            // Permission withdrawn between the check and the post: nothing to show.
        }
    }
}
