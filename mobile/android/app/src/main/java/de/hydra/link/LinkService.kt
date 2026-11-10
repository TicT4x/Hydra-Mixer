package de.hydra.link

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.Context
import android.content.Intent
import android.content.pm.ServiceInfo
import android.net.wifi.WifiManager
import android.os.Build
import android.os.IBinder
import android.os.PowerManager

/** Foreground service while the phone listens and / or sends: audio and network keep running with the screen off.
    Holds a low-latency Wi-Fi lock (no Wi-Fi power save, which adds tens of ms of jitter), a multicast lock
    (Hydra's announcements are broadcasts) and a partial wake lock (the network thread keeps running). */
class LinkService : Service() {
    companion object {
        const val ACTION_UPDATE = "de.hydra.link.UPDATE"
        const val ACTION_STOP = "de.hydra.link.STOP"
        const val EXTRA_TEXT = "text"
        const val EXTRA_MIC = "mic"
        private const val CHANNEL = "link"
        private const val ID = 1

        fun update(context: Context, text: String, mic: Boolean) {
            val i = Intent(context, LinkService::class.java).setAction(ACTION_UPDATE)
                .putExtra(EXTRA_TEXT, text).putExtra(EXTRA_MIC, mic)
            if (Build.VERSION.SDK_INT >= 26) context.startForegroundService(i) else context.startService(i)
        }

        fun stop(context: Context) {
            context.stopService(Intent(context, LinkService::class.java))
        }
    }

    private var wifiLock: WifiManager.WifiLock? = null
    private var multicastLock: WifiManager.MulticastLock? = null
    private var wakeLock: PowerManager.WakeLock? = null

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onCreate() {
        super.onCreate()
        val wifi = applicationContext.getSystemService(Context.WIFI_SERVICE) as WifiManager
        @Suppress("DEPRECATION")
        val mode = if (Build.VERSION.SDK_INT >= 29) WifiManager.WIFI_MODE_FULL_LOW_LATENCY else WifiManager.WIFI_MODE_FULL_HIGH_PERF
        wifiLock = wifi.createWifiLock(mode, "HydraLink:wifi").apply { setReferenceCounted(false); acquire() }
        multicastLock = wifi.createMulticastLock("HydraLink:announce").apply { setReferenceCounted(false); acquire() }
        val power = getSystemService(Context.POWER_SERVICE) as PowerManager
        wakeLock = power.newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "HydraLink:net").apply { setReferenceCounted(false); acquire() }
        if (Build.VERSION.SDK_INT >= 26) {
            val nm = getSystemService(NotificationManager::class.java)
            nm.createNotificationChannel(NotificationChannel(CHANNEL, getString(R.string.notification_channel), NotificationManager.IMPORTANCE_LOW))
        }
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        if (intent?.action == ACTION_STOP) {
            // "Stop" in the notification = disconnect (the pairing stays; the app does not reconnect by itself).
            getSharedPreferences("hydra", Context.MODE_PRIVATE).edit().putString("hydra", "").apply()
            NativeLink.listen("", "")
            NativeLink.stopSending()
            stopSelf()
            return START_NOT_STICKY
        }
        val text = intent?.getStringExtra(EXTRA_TEXT) ?: getString(R.string.app_name)
        val mic = intent?.getBooleanExtra(EXTRA_MIC, false) ?: false
        val n = notification(text)
        if (Build.VERSION.SDK_INT >= 30) {
            var type = ServiceInfo.FOREGROUND_SERVICE_TYPE_MEDIA_PLAYBACK
            if (mic) type = type or ServiceInfo.FOREGROUND_SERVICE_TYPE_MICROPHONE
            startForeground(ID, n, type)
        } else if (Build.VERSION.SDK_INT >= 29) {
            startForeground(ID, n, ServiceInfo.FOREGROUND_SERVICE_TYPE_MEDIA_PLAYBACK)
        } else {
            startForeground(ID, n)
        }
        return START_NOT_STICKY
    }

    private fun notification(text: String): Notification {
        val open = PendingIntent.getActivity(this, 0, Intent(this, MainActivity::class.java)
            .addFlags(Intent.FLAG_ACTIVITY_SINGLE_TOP), PendingIntent.FLAG_IMMUTABLE)
        val stop = PendingIntent.getService(this, 1, Intent(this, LinkService::class.java).setAction(ACTION_STOP),
            PendingIntent.FLAG_IMMUTABLE)
        val b = if (Build.VERSION.SDK_INT >= 26) Notification.Builder(this, CHANNEL) else @Suppress("DEPRECATION") Notification.Builder(this)
        return b.setContentTitle(getString(R.string.app_name))
            .setContentText(text)
            .setSmallIcon(R.drawable.ic_stat_link)
            .setOngoing(true)
            .setContentIntent(open)
            .addAction(Notification.Action.Builder(null, getString(R.string.stop), stop).build())
            .build()
    }

    override fun onDestroy() {
        wifiLock?.release()
        multicastLock?.release()
        wakeLock?.release()
        super.onDestroy()
    }
}
