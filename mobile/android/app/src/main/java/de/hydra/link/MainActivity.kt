package de.hydra.link

import android.Manifest
import android.app.Activity
import android.app.AlertDialog
import android.content.Context
import android.content.SharedPreferences
import android.content.pm.PackageManager
import android.content.res.Configuration
import android.graphics.Color
import android.net.ConnectivityManager
import android.net.NetworkCapabilities
import android.net.wifi.WifiManager
import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.provider.Settings
import android.view.Gravity
import android.view.View
import android.view.ViewGroup
import android.view.WindowInsets
import android.widget.EditText
import android.widget.FrameLayout
import android.widget.ImageView
import android.widget.LinearLayout
import android.widget.TextView
import android.widget.Toast
import org.json.JSONObject
import java.net.Inet4Address

/** The app: Hydra's mixer (the web control Hydra serves, embedded) and audio over Hydra Link, in Hydra's look.
    Portrait: app bar on top, navigation at the bottom (Inputs, Outputs, Scenes, Audio). Landscape: a navigation rail
    on the left (Mixer, Scenes, Audio) and the channels as vertical strips. Edge to edge: the bars keep clear of the
    status bar, the camera cut-out and the gesture area (window insets). */
class MainActivity : Activity() {
    companion object { const val REQ_MIC = 1 }

    lateinit var prefs: SharedPreferences
    private val handler = Handler(Looper.getMainLooper())
    private var multicastLock: WifiManager.MulticastLock? = null

    private lateinit var root: FrameLayout
    private lateinit var audio: AudioPage
    private lateinit var mixer: MixerPage
    private val pages by lazy { FrameLayout(this) }
    private var tab = "in"
    private var land = false
    private var insetL = 0; private var insetT = 0; private var insetR = 0; private var insetB = 0

    // app bar / rail parts (rebuilt with the orientation)
    private var subtitle: TextView? = null
    private var dsp: TextView? = null
    private var dot: View? = null
    private val navItems = mutableMapOf<String, LinearLayout>()
    private var mixerStatus: JSONObject? = null

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        prefs = getSharedPreferences("hydra", Context.MODE_PRIVATE)
        tab = prefs.getString("tab", "in") ?: "in"
        edgeToEdge()
        root = FrameLayout(this).apply { setBackgroundColor(Hc.bg) }
        setContentView(root)
        audio = AudioPage(this)
        mixer = MixerPage(this)
        pages.addView(mixer.view, FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT))
        pages.addView(audio.view, FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT))
        root.setOnApplyWindowInsetsListener { _, insets -> onInsets(insets); insets }
        buildChrome()
        applyGains()
        if (Build.VERSION.SDK_INT >= 33 && checkSelfPermission(Manifest.permission.POST_NOTIFICATIONS) != PackageManager.PERMISSION_GRANTED)
            requestPermissions(arrayOf(Manifest.permission.POST_NOTIFICATIONS), 2)
    }

    @Suppress("DEPRECATION")
    private fun edgeToEdge() {
        if (Build.VERSION.SDK_INT >= 30) window.setDecorFitsSystemWindows(false)
        else window.decorView.systemUiVisibility = View.SYSTEM_UI_FLAG_LAYOUT_STABLE or View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN or View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION
        if (Build.VERSION.SDK_INT < 35) { window.statusBarColor = Color.TRANSPARENT; window.navigationBarColor = Color.TRANSPARENT }
        if (Build.VERSION.SDK_INT >= 29) window.isNavigationBarContrastEnforced = false
        if (Build.VERSION.SDK_INT >= 28) window.attributes.layoutInDisplayCutoutMode = android.view.WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES
    }

    @Suppress("DEPRECATION")
    private fun onInsets(insets: WindowInsets) {
        if (Build.VERSION.SDK_INT >= 30) {
            val i = insets.getInsets(WindowInsets.Type.systemBars() or WindowInsets.Type.displayCutout())
            val ime = insets.getInsets(WindowInsets.Type.ime())
            insetL = i.left; insetT = i.top; insetR = i.right; insetB = maxOf(i.bottom, ime.bottom)
        } else {
            insetL = insets.systemWindowInsetLeft; insetT = insets.systemWindowInsetTop
            insetR = insets.systemWindowInsetRight; insetB = insets.systemWindowInsetBottom
        }
        applyInsets()
    }

    override fun onConfigurationChanged(newConfig: Configuration) {
        super.onConfigurationChanged(newConfig)
        buildChrome()
    }

    override fun onResume() {
        super.onResume()
        val wifi = applicationContext.getSystemService(Context.WIFI_SERVICE) as WifiManager
        multicastLock = wifi.createMulticastLock("HydraLink:ui").apply { setReferenceCounted(false); acquire() }
        startLink()
        handler.post(tick)
    }

    override fun onPause() {
        handler.removeCallbacks(tick)
        multicastLock?.release()
        super.onPause()
    }

    // ---- chrome ----------------------------------------------------------------------------------------------
    private fun buildChrome() {
        land = resources.configuration.orientation == Configuration.ORIENTATION_LANDSCAPE
        if (land && (tab == "in" || tab == "out")) tab = "mix"
        if (!land && tab == "mix") tab = "in"
        (pages.parent as? ViewGroup)?.removeView(pages)
        root.removeAllViews()
        navItems.clear()
        if (land) {
            val row = LinearLayout(this).apply { orientation = LinearLayout.HORIZONTAL }
            row.addView(rail(), LinearLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.MATCH_PARENT))
            row.addView(pages, LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.MATCH_PARENT, 1f))
            root.addView(row)
        } else {
            val col = LinearLayout(this).apply { orientation = LinearLayout.VERTICAL }
            col.addView(appBar(), lp())
            col.addView(pages, LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, 0, 1f))
            col.addView(bottomNav(), lp())
            root.addView(col)
        }
        selectTab(tab)
        onMixerStatus(mixerStatus)
        root.requestApplyInsets()
    }

    private var appBarView: View? = null
    private var navView: View? = null

    private fun appBar(): View = LinearLayout(this).apply {
        orientation = LinearLayout.HORIZONTAL
        gravity = Gravity.CENTER_VERTICAL
        setBackgroundColor(Hc.panel)
        addView(ImageView(context).apply { setImageResource(R.drawable.logo) }, LinearLayout.LayoutParams(dpi(26), dpi(25)).apply { marginEnd = dpi(12) })
        val texts = LinearLayout(context).apply { orientation = LinearLayout.VERTICAL }
        texts.addView(label("Hydra", 17f, Hc.text, true))
        subtitle = label("", 12f, Hc.dim).apply { setSingleLine(); ellipsize = android.text.TextUtils.TruncateAt.END }
        texts.addView(subtitle, lp(top = dpi(3)))
        addView(texts, LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f))
        dsp = pill()
        addView(dsp, LinearLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT).apply { marginEnd = dpi(10) })
        dot = statusDot()
        addView(dot, LinearLayout.LayoutParams(dpi(8), dpi(8)))
        appBarView = this
    }

    private fun pill() = label("", 11f, Hc.dim).apply {
        typeface = Hc.mono
        setPadding(dpi(7), dpi(5), dpi(7), dpi(5))
        background = rounded(Hc.panel2, Hc.outline, 5f)
        visibility = View.GONE
    }

    private fun statusDot() = View(this).apply { background = rounded(Hc.faint, 0, 4f) }

    private fun navEntries() = if (land)
        listOf(Triple("mix", R.drawable.ic_nav_mix, R.string.nav_mixer), Triple("scenes", R.drawable.ic_nav_scenes, R.string.nav_scenes), Triple("audio", R.drawable.ic_nav_audio, R.string.nav_audio))
    else
        listOf(Triple("in", R.drawable.ic_nav_in, R.string.nav_inputs), Triple("out", R.drawable.ic_nav_out, R.string.nav_outputs),
               Triple("scenes", R.drawable.ic_nav_scenes, R.string.nav_scenes), Triple("audio", R.drawable.ic_nav_audio, R.string.nav_audio))

    private fun navItem(key: String, iconRes: Int, text: Int) = LinearLayout(this).apply {
        orientation = LinearLayout.VERTICAL
        gravity = Gravity.CENTER
        isClickable = true
        setPadding(0, dpi(6), 0, dpi(6))
        addView(icon(iconRes, Hc.dim, 22).apply { tag = "icon" })
        addView(label(getString(text), 11f, Hc.dim, true).apply { tag = "text" }, LinearLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT).apply { topMargin = dpi(4) })
        setOnClickListener { selectTab(key) }
        navItems[key] = this
    }

    private fun bottomNav(): View = LinearLayout(this).apply {
        orientation = LinearLayout.HORIZONTAL
        setBackgroundColor(Hc.panel)
        setPadding(dpi(10), dpi(6), dpi(10), dpi(6))
        for ((key, ic, text) in navEntries())
            addView(navItem(key, ic, text), LinearLayout.LayoutParams(0, dpi(56), 1f).apply { marginStart = dpi(3); marginEnd = dpi(3) })
        navView = this
        appBarView?.let { } // set in applyInsets
    }

    private fun rail(): View = LinearLayout(this).apply {
        orientation = LinearLayout.VERTICAL
        gravity = Gravity.CENTER_HORIZONTAL
        setBackgroundColor(Hc.panel)
        addView(ImageView(context).apply { setImageResource(R.drawable.logo) }, LinearLayout.LayoutParams(dpi(30), dpi(28)).apply { bottomMargin = dpi(14); topMargin = dpi(4) })
        for ((key, ic, text) in navEntries())
            addView(navItem(key, ic, text), LinearLayout.LayoutParams(dpi(64), dpi(60)).apply { bottomMargin = dpi(6) })
        addView(View(context), LinearLayout.LayoutParams(1, 0, 1f))
        dsp = pill()
        addView(dsp, LinearLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT).apply { bottomMargin = dpi(8) })
        dot = statusDot()
        addView(dot, LinearLayout.LayoutParams(dpi(8), dpi(8)))
        subtitle = null
        appBarView = null
        navView = this
    }

    private fun applyInsets() {
        if (land) {
            navView?.setPadding(insetL + dpi(6), insetT + dpi(12), dpi(6), insetB + dpi(12))
            audio.setPadding(0, insetT, insetR, insetB)
            mixer.setInsets(0, insetT, insetR, insetB)
        } else {
            appBarView?.setPadding(insetL + dpi(16), insetT + dpi(10), insetR + dpi(16), dpi(10))
            navView?.setPadding(insetL + dpi(10), dpi(6), insetR + dpi(10), insetB + dpi(6))
            audio.setPadding(insetL, 0, insetR, 0)
            mixer.setInsets(insetL, 0, insetR, 0)
        }
    }

    fun selectTab(key: String) {
        tab = key
        prefs.edit().putString("tab", key).apply()
        for ((k, item) in navItems) {
            val on = k == key
            item.background = if (on) rounded(Hc.raised, 0, 10f) else null
            (item.findViewWithTag<ImageView>("icon"))?.setColorFilter(if (on) Hc.input else Hc.dim)
            (item.findViewWithTag<TextView>("text"))?.setTextColor(if (on) Hc.text else Hc.dim)
        }
        val isAudio = key == "audio"
        audio.view.visibility = if (isAudio) View.VISIBLE else View.GONE
        mixer.view.visibility = if (isAudio) View.GONE else View.VISIBLE
        if (!isAudio) mixer.setView(key)
        onMixerStatus(mixerStatus)
    }

    /** Status of the embedded web control (null: no page). */
    fun onMixerStatus(s: JSONObject?) {
        mixerStatus = s
        val connected = s?.optBoolean("connected") == true
        if (s != null && !s.optBoolean("paired", true)) audio.refreshCode()   // the page asks for a code: get the current one
        dot?.background = rounded(if (connected) Hc.good else Hc.faint, 0, 4f)
        val d = s?.optString("dsp") ?: ""
        dsp?.text = if (land) d.removePrefix("DSP ") else d
        dsp?.visibility = if (connected && d.isNotEmpty()) View.VISIBLE else View.GONE
        subtitle?.text = when {
            tab == "audio" -> getString(R.string.this_phone, phoneName()) + (wifiAddress()?.let { " · $it" } ?: "")
            s == null -> getString(R.string.mixer_not_connected)
            !s.optBoolean("paired", true) -> getString(R.string.mixer_pairing)
            connected -> s.optString("name")
            else -> getString(R.string.mixer_connecting)
        }
    }

    // ---- names, network --------------------------------------------------------------------------------------
    fun phoneName(): String {
        val saved = prefs.getString("name", "") ?: ""
        if (saved.isNotBlank()) return saved
        val dev = Settings.Global.getString(contentResolver, "device_name")
        return (if (dev.isNullOrBlank()) Build.MODEL else dev).take(40)
    }

    /** The phone's address in the Wi-Fi (not the mobile network's, which may be the active one). */
    fun wifiAddress(): String? {
        val cm = getSystemService(ConnectivityManager::class.java) ?: return null
        @Suppress("DEPRECATION")
        val nets = cm.allNetworks
        val wifi = nets.firstOrNull { cm.getNetworkCapabilities(it)?.hasTransport(NetworkCapabilities.TRANSPORT_WIFI) == true } ?: return null
        return cm.getLinkProperties(wifi)?.linkAddresses?.map { it.address }?.firstOrNull { it is Inet4Address && !it.isLoopbackAddress }?.hostAddress
    }

    /** This phone's persistent id for pairing (16 hex digits, made once). */
    private fun deviceId(): String {
        var id = prefs.getString("deviceId", "") ?: ""
        if (id.length != 16) {
            val r = java.security.SecureRandom()
            id = String.format("%016x", r.nextLong() or 1L)
            prefs.edit().putString("deviceId", id).apply()
        }
        return id
    }

    fun startLink() {
        val err = NativeLink.start(phoneName(), NativeLink.PORT, deviceId(), prefs.getString("trusted", "") ?: "")
        if (err.isNotEmpty()) toast(err)
    }

    // ---- pairing ---------------------------------------------------------------------------------------------
    private var trustShown = -1
    private var requestDialog: AlertDialog? = null

    /** Stores the paired Hydras when they change; asks the user about a Hydra that wants to connect. */
    private fun pairing(s: JSONObject) {
        val v = s.optInt("trustVersion")
        if (v != trustShown) {
            trustShown = v
            val a = s.optJSONArray("trusted")
            if (a != null) {
                val lines = (0 until a.length()).joinToString("\n") { a.getJSONObject(it).let { t -> t.optString("id") + "|" + t.optString("name") } }
                prefs.edit().putString("trusted", lines).apply()
            }
        }
        if (requestDialog?.isShowing == true) return
        val req = s.optJSONArray("requests") ?: return
        if (req.length() == 0) return
        val r = req.getJSONObject(0)
        val id = r.optString("id")
        val name = r.optString("name")
        requestDialog = AlertDialog.Builder(this, android.R.style.Theme_DeviceDefault_Dialog_Alert)
            .setTitle(getString(R.string.request_title, name))
            .setMessage(getString(R.string.request_message, name, r.optString("address")))
            .setPositiveButton(R.string.allow) { _, _ -> NativeLink.answer(id, true); audio.onAllowed(name) }
            .setNegativeButton(R.string.deny) { _, _ -> NativeLink.answer(id, false) }
            .setCancelable(false)
            .show()
    }

    fun renameDialog() {
        val edit = EditText(this).apply { setText(phoneName()); setSingleLine(); setSelection(text.length) }
        AlertDialog.Builder(this, android.R.style.Theme_DeviceDefault_Dialog_Alert)
            .setTitle(R.string.rename_title)
            .setMessage(R.string.rename_message)
            .setView(edit)
            .setPositiveButton(android.R.string.ok) { _, _ ->
                prefs.edit().putString("name", edit.text.toString().trim().take(40)).apply()
                NativeLink.listen("", "")
                NativeLink.stopSending()
                NativeLink.stop()
                startLink()
                updateService()
            }
            .setNegativeButton(android.R.string.cancel, null)
            .show()
    }

    fun applyGains() = NativeLink.setGains(prefs.getFloat("play", 1f), prefs.getFloat("mic", 1f))
    fun toast(t: String) = Toast.makeText(this, t, Toast.LENGTH_LONG).show()
    fun handlerPostDelayed(ms: Long, r: () -> Unit) { handler.postDelayed(r, ms) }

    /** Foreground service while anything runs (audio with the screen off), stopped when idle. */
    fun updateService(forceMic: Boolean = false) {
        val s = try { JSONObject(NativeLink.state()) } catch (e: Exception) { return }
        val rx = s.optJSONObject("rx")
        val tx = s.optJSONObject("tx")
        val listening = rx?.optBoolean("listening") == true
        val sending = forceMic || tx?.optBoolean("sending") == true
        if (!listening && !sending) { LinkService.stop(this); return }
        val hydra = (rx?.optString("sender") ?: "").ifEmpty { (tx?.optString("to") ?: "").ifEmpty { prefs.getString("hydra", "") ?: "" } }
        LinkService.update(this, getString(R.string.notif_connected, hydra), sending)
    }

    override fun onRequestPermissionsResult(requestCode: Int, permissions: Array<out String>, grantResults: IntArray) {
        if (requestCode == REQ_MIC) audio.onMicPermission(grantResults.firstOrNull() == PackageManager.PERMISSION_GRANTED)
    }

    private val tick = object : Runnable {
        override fun run() {
            val s = try { JSONObject(NativeLink.state()) } catch (e: Exception) { null }
            if (s != null) {
                audio.refresh(s)
                mixer.update(s, audio.hydra(), audio.hydraPeer(s))
                pairing(s)
            }
            if (tab == "audio") onMixerStatus(mixerStatus)
            handler.postDelayed(this, 100)
        }
    }
}
