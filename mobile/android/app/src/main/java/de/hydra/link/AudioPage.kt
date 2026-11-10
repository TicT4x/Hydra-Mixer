package de.hydra.link

import android.Manifest
import android.app.AlertDialog
import android.content.pm.PackageManager
import android.media.AudioDeviceInfo
import android.media.AudioManager
import android.os.Build
import android.view.Gravity
import android.view.View
import android.view.ViewGroup
import android.widget.ImageView
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.Switch
import android.widget.TextView
import org.json.JSONObject

/** One connection to one Hydra: find it, connect once (Hydra confirms, both remember it), then audio runs both ways by
    itself (what Hydra sends to this phone plays, the microphone goes to Hydra) and the mixer page controls it.
    Two tiles mute playback / microphone; each shows its level and the phone's device in use. */
class AudioPage(private val act: MainActivity) {
    private val prefs = act.prefs
    val view: ScrollView
    private val content: LinearLayout

    // Hydra card
    private val hydraTitle: TextView
    private val hydraStatus: TextView
    private val hydraDot: View
    private val hydraButton: TextView
    private val hydraList: LinearLayout
    private val hydraHint: TextView

    // Audio card
    private val audioCard: Card
    private val playTile: Tile
    private val micTile: Tile

    // Settings
    private val phoneName: TextView
    private val phoneAddr: TextView
    private val qualityPcm: LinearLayout
    private val qualityOpus: LinearLayout
    private val voice: Switch

    private var shownKey = -1
    private var last: JSONObject? = null
    private var micAsked = false
    private var lastCodeAsk = 0L
    private var lastStart = 0L
    private var askedAtStart = false
    private var deviceTextAt = 0L
    private var outDeviceText = ""
    private var inDeviceText = ""

    /** The Hydra this phone is connected to (its announced name), "" = none. */
    fun hydra() = prefs.getString("hydra", "") ?: ""

    init {
        content = LinearLayout(act).apply { orientation = LinearLayout.VERTICAL }
        view = ScrollView(act).apply {
            isFillViewport = true
            setBackgroundColor(Hc.bg)
            clipToPadding = false
            addView(content)
        }

        // ---- Hydra: the connection
        val hydraCard = Card(act, act.getString(R.string.hydra_card), Hc.hw)
        val head = LinearLayout(act).apply { orientation = LinearLayout.HORIZONTAL; gravity = Gravity.CENTER_VERTICAL }
        head.addView(ImageView(act).apply { setImageResource(R.drawable.logo) }, LinearLayout.LayoutParams(act.dpi(34), act.dpi(32)).apply { marginEnd = act.dpi(12) })
        val texts = LinearLayout(act).apply { orientation = LinearLayout.VERTICAL }
        hydraTitle = act.label("", 18f, Hc.text, true)
        val statusRow = LinearLayout(act).apply { orientation = LinearLayout.HORIZONTAL; gravity = Gravity.CENTER_VERTICAL }
        hydraDot = View(act).apply { background = act.rounded(Hc.faint, 0, 4f) }
        hydraStatus = act.label("", 12.5f, Hc.dim).apply { setLineSpacing(0f, 1.15f) }
        statusRow.addView(hydraDot, LinearLayout.LayoutParams(act.dpi(8), act.dpi(8)).apply { marginEnd = act.dpi(7) })
        statusRow.addView(hydraStatus, LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f))
        texts.addView(hydraTitle)
        texts.addView(statusRow, lp(top = act.dpi(5)))
        head.addView(texts, LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f))
        hydraButton = act.hbutton(act.getString(R.string.disconnect)) { disconnectDialog() }
        head.addView(hydraButton, LinearLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT).apply { marginStart = act.dpi(8) })
        hydraList = LinearLayout(act).apply { orientation = LinearLayout.VERTICAL }
        hydraHint = act.label("", 13f, Hc.dim).apply { setLineSpacing(0f, 1.2f) }
        hydraCard.add(head, 0)
        hydraCard.add(hydraList, 14)
        hydraCard.add(hydraHint, 6)
        content.addView(hydraCard, lp(bottom = act.dpi(12)))

        // ---- Audio: playback + microphone
        audioCard = Card(act, act.getString(R.string.audio_card), Hc.input)
        val tiles = LinearLayout(act).apply { orientation = LinearLayout.HORIZONTAL }
        playTile = Tile(act.getString(R.string.playback), R.drawable.ic_nav_out, Hc.virt) { togglePlay() }
        micTile = Tile(act.getString(R.string.microphone), R.drawable.ic_nav_in, Hc.input) { toggleMic() }
        tiles.addView(playTile.view, LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.MATCH_PARENT, 1f).apply { marginEnd = act.dpi(8) })
        tiles.addView(micTile.view, LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.MATCH_PARENT, 1f))
        audioCard.add(tiles, 0)
        audioCard.add(act.gainSlider(act.getString(R.string.volume), prefs.getFloat("play", 1f), Hc.virt) { g ->
            prefs.edit().putFloat("play", g).apply(); act.applyGains()
        }, 14)
        audioCard.add(act.gainSlider(act.getString(R.string.mic_level), prefs.getFloat("mic", 1f), Hc.input) { g ->
            prefs.edit().putFloat("mic", g).apply(); act.applyGains()
        }, 4)
        content.addView(audioCard, lp(bottom = act.dpi(12)))

        // ---- Settings: this phone, quality
        val settings = Card(act, act.getString(R.string.settings_card), Hc.faint)
        val phoneRow = LinearLayout(act).apply { orientation = LinearLayout.HORIZONTAL; gravity = Gravity.CENTER_VERTICAL }
        val phoneTexts = LinearLayout(act).apply { orientation = LinearLayout.VERTICAL }
        phoneName = act.label("", 15f, Hc.text, true)
        phoneAddr = act.label("", 12f, Hc.dim)
        phoneTexts.addView(phoneName)
        phoneTexts.addView(phoneAddr, lp(top = act.dpi(4)))
        phoneRow.addView(phoneTexts, LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f))
        phoneRow.addView(act.hbutton(act.getString(R.string.rename)) { act.renameDialog() })
        val quality = LinearLayout(act).apply { orientation = LinearLayout.HORIZONTAL }
        qualityPcm = segment(act.getString(R.string.quality_pcm_short), act.getString(R.string.quality_pcm_sub))
        qualityOpus = segment(act.getString(R.string.quality_opus_short), act.getString(R.string.quality_opus_sub))
        quality.addView(qualityPcm, LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f).apply { marginEnd = act.dpi(6) })
        quality.addView(qualityOpus, LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f))
        qualityPcm.setOnClickListener { setCodec(NativeLink.CODEC_PCM24) }
        qualityOpus.setOnClickListener { setCodec(NativeLink.CODEC_OPUS) }
        voice = Switch(act).apply {
            text = act.getString(R.string.voice_processing)
            setTextColor(Hc.text)
            textSize = 13f
            isChecked = prefs.getBoolean("voice", false)
            thumbTintList = android.content.res.ColorStateList(arrayOf(intArrayOf(android.R.attr.state_checked), intArrayOf()), intArrayOf(Hc.input, Hc.dim))
            trackTintList = android.content.res.ColorStateList(arrayOf(intArrayOf(android.R.attr.state_checked), intArrayOf()), intArrayOf(blend(Hc.input, Hc.panel2, 0.45f), Hc.raised))
            setOnCheckedChangeListener { _, on -> prefs.edit().putBoolean("voice", on).apply(); restartMic() }
        }
        settings.add(phoneRow, 0)
        settings.add(act.label(act.getString(R.string.mic_quality).uppercase(), 11f, Hc.dim, true).apply { letterSpacing = 0.09f }, 16)
        settings.add(quality, 8)
        settings.add(voice, 10)
        content.addView(settings, lp(bottom = act.dpi(12)))
        content.addView(act.label(act.getString(R.string.footer), 12f, Hc.faint).apply { setPadding(act.dpi(4), act.dpi(4), act.dpi(4), 0); setLineSpacing(0f, 1.2f) })
        setCodec(prefs.getInt("codec", NativeLink.CODEC_PCM24), restart = false)
        applyMutes()
    }

    /** Space for the system bars / navigation around the content. */
    fun setPadding(l: Int, t: Int, r: Int, b: Int) = content.setPadding(l + act.dpi(14), t + act.dpi(14), r + act.dpi(14), b + act.dpi(20))

    private fun segment(title: String, sub: String) = LinearLayout(act).apply {
        orientation = LinearLayout.VERTICAL
        setPadding(act.dpi(12), act.dpi(10), act.dpi(12), act.dpi(10))
        isClickable = true
        addView(act.label(title, 14f, Hc.text, true))
        addView(act.label(sub, 11f, Hc.dim), lp(top = act.dpi(3)))
    }

    private fun setCodec(c: Int, restart: Boolean = true) {
        val changed = prefs.getInt("codec", NativeLink.CODEC_PCM24) != c
        prefs.edit().putInt("codec", c).apply()
        for ((seg, on) in listOf(qualityPcm to (c != NativeLink.CODEC_OPUS), qualityOpus to (c == NativeLink.CODEC_OPUS))) {
            seg.background = act.rounded(if (on) blend(Hc.input, Hc.panel2, 0.18f) else Hc.panel2, if (on) Hc.input else Hc.outline, 6f)
        }
        if (restart && changed) restartMic()
    }

    // ---- tiles -----------------------------------------------------------------------------------------------
    /** A big tap target: icon, title, the device in use, a level meter, "On" / "Muted". */
    private inner class Tile(title: String, iconRes: Int, val color: Int, onTap: () -> Unit) {
        val view = LinearLayout(act).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(act.dpi(14), act.dpi(14), act.dpi(14), act.dpi(12))
            isClickable = true
            setOnClickListener { onTap() }
        }
        private val icon = act.icon(iconRes, color, 30)
        private val state = act.label("", 11f, Hc.text, true).apply { letterSpacing = 0.08f; setPadding(act.dpi(7), act.dpi(3), act.dpi(7), act.dpi(3)) }
        private val device = act.label("", 12f, Hc.dim).apply { maxLines = 2; setLineSpacing(0f, 1.1f) }
        private val detail = act.label("", 11f, Hc.faint).apply { maxLines = 3; setLineSpacing(0f, 1.15f) }
        val meter = Meter(act, color)

        init {
            val top = LinearLayout(act).apply { orientation = LinearLayout.HORIZONTAL; gravity = Gravity.CENTER_VERTICAL }
            top.addView(icon)
            top.addView(View(act), LinearLayout.LayoutParams(0, 1, 1f))
            top.addView(state)
            view.addView(top)
            view.addView(act.label(title, 16f, Hc.text, true), lp(top = act.dpi(12)))
            view.addView(device, lp(top = act.dpi(4)))
            view.addView(meter, lp(top = act.dpi(12)))
            view.addView(detail, lp(top = act.dpi(8)))
        }

        private var shown = -1

        fun set(muted: Boolean, available: Boolean, deviceText: String, detailText: String, peak: Float) {
            if (device.text != deviceText) device.text = deviceText
            if (detail.text != detailText) detail.text = detailText
            meter.set(if (available) peak else 0f)
            val key = (if (muted) 1 else 0) + (if (available) 2 else 0)
            if (key == shown) return   // the look only changes with the state (no new drawables 10x per second)
            shown = key
            val c = if (!available) Hc.faint else if (muted) Hc.mute else color
            view.background = act.rounded(if (muted && available) blend(Hc.mute, Hc.panel2, 0.12f) else Hc.panel2, if (available && !muted) blend(color, Hc.outline, 0.5f) else if (muted) Hc.mute else Hc.outline, 10f)
            icon.setColorFilter(c)
            state.text = act.getString(if (!available) R.string.tile_off else if (muted) R.string.tile_muted else R.string.tile_on).uppercase()
            state.setTextColor(if (!available) Hc.dim else if (muted) Hc.text else Hc.bg)
            state.background = act.rounded(if (!available) Hc.raised else if (muted) Hc.mute else color, 0, 4f)
            meter.alpha = if (muted) 0.35f else 1f
        }
    }

    private fun applyMutes() = NativeLink.setMuted(prefs.getBoolean("playMuted", false), prefs.getBoolean("micMuted", false))

    private fun togglePlay() {
        if (hydra().isEmpty()) return
        prefs.edit().putBoolean("playMuted", !prefs.getBoolean("playMuted", false)).apply()
        applyMutes()
    }

    private fun toggleMic() {
        if (hydra().isEmpty()) return
        if (!hasMic()) { micAsked = true; act.requestPermissions(arrayOf(Manifest.permission.RECORD_AUDIO), MainActivity.REQ_MIC); return }
        prefs.edit().putBoolean("micMuted", !prefs.getBoolean("micMuted", false)).apply()
        applyMutes()
    }

    private fun hasMic() = act.checkSelfPermission(Manifest.permission.RECORD_AUDIO) == PackageManager.PERMISSION_GRANTED

    fun onMicPermission(granted: Boolean) {
        if (granted) { shownKey = -1; ensureAudio(last, force = true) }
    }

    // ---- connection ------------------------------------------------------------------------------------------
    private fun peerOf(s: JSONObject?, name: String): JSONObject? {
        val peers = s?.optJSONArray("peers") ?: return null
        for (i in 0 until peers.length()) {
            val p = peers.getJSONObject(i)
            if (p.optString("name").equals(name, true)) return p
        }
        return null
    }

    /** The connected Hydra as the endpoint sees it (null: not in the Wi-Fi right now). */
    fun hydraPeer(s: JSONObject?) = if (hydra().isEmpty()) null else peerOf(s, hydra())

    private fun connectTo(name: String) {
        prefs.edit().putString("hydra", name).apply()
        NativeLink.connect(name)
        shownKey = -1
        if (!hasMic() && !micAsked) { micAsked = true; act.requestPermissions(arrayOf(Manifest.permission.RECORD_AUDIO), MainActivity.REQ_MIC) }
    }

    /** Called when the user allowed a Hydra that asked this phone: use it if nothing else is connected. */
    fun onAllowed(name: String) {
        if (hydra().isEmpty()) connectTo(name)
    }

    private fun disconnectDialog() {
        val name = hydra()
        AlertDialog.Builder(act, android.R.style.Theme_DeviceDefault_Dialog_Alert)
            .setTitle(act.getString(R.string.disconnect_title, name))
            .setMessage(R.string.disconnect_message)
            .setPositiveButton(R.string.disconnect) { _, _ -> disconnect(false) }
            .setNeutralButton(R.string.disconnect_forget) { _, _ -> disconnect(true) }
            .setNegativeButton(android.R.string.cancel, null)
            .show()
    }

    private fun disconnect(forget: Boolean) {
        val p = hydraPeer(last)
        if (forget) {
            val id = p?.optString("id") ?: ""
            if (id.isNotEmpty()) NativeLink.forget(id)
            else for (t in trustedOf(last)) if (t.second.equals(hydra(), true)) NativeLink.forget(t.first)
        }
        prefs.edit().putString("hydra", "").apply()
        NativeLink.listen("", "")
        NativeLink.stopSending()
        act.updateService()
        shownKey = -1
    }

    private fun trustedOf(s: JSONObject?): List<Pair<String, String>> {
        val a = s?.optJSONArray("trusted") ?: return emptyList()
        return (0 until a.length()).map { a.getJSONObject(it).let { t -> t.optString("id") to t.optString("name") } }
    }

    /** Connected + paired: playback listens to the Hydra, the microphone sends to it (once; again after changes). */
    private fun ensureAudio(s: JSONObject?, force: Boolean = false) {
        s ?: return
        val name = hydra()
        val peer = hydraPeer(s)
        val rx = s.getJSONObject("rx")
        val tx = s.getJSONObject("tx")
        if (name.isEmpty()) return
        if (peer == null || !peer.optBoolean("trusted")) {
            // Not paired (any more): ask once per start of the app, e.g. after Hydra forgot this phone.
            if (peer != null && !askedAtStart && peer.optInt("pair") == NativeLink.PAIR_NONE) { askedAtStart = true; NativeLink.connect(name) }
            return
        }
        var changed = false
        // A start that failed (audio device busy ...) is tried again every 3 s, not on every refresh.
        if (!force && System.currentTimeMillis() - lastStart < 3000) return
        if (!rx.optBoolean("listening") || !rx.optString("sender").equals(name, true)) {
            lastStart = System.currentTimeMillis()
            val err = NativeLink.listen("*", name)
            if (err.isNotEmpty()) act.toast(err)
            changed = true
        }
        if (hasMic() && (force || !tx.optBoolean("sending") || !tx.optString("to").equals(name, true))) {
            lastStart = System.currentTimeMillis()
            startMic(name)
            changed = true
        }
        // Hydra Remote's code (control): comes with the answer to a request; ask again when it is missing.
        if (peer.optInt("remote") > 0 && peer.optString("code").isEmpty() && peer.optInt("pair") != NativeLink.PAIR_ASKING &&
            System.currentTimeMillis() - lastCodeAsk > 8000) {
            lastCodeAsk = System.currentTimeMillis()
            NativeLink.connect(name)
        }
        if (changed) act.handlerPostDelayed(300) { act.updateService() }
    }

    /** The mixer page has an outdated code (shows the code screen): ask Hydra for the current one. */
    fun refreshCode() {
        if (hydra().isEmpty() || System.currentTimeMillis() - lastCodeAsk < 8000) return
        lastCodeAsk = System.currentTimeMillis()
        NativeLink.connect(hydra())
    }

    private fun startMic(name: String) {
        val codec = prefs.getInt("codec", NativeLink.CODEC_PCM24)
        val preset = when {
            prefs.getBoolean("voice", false) -> NativeLink.PRESET_VOICE_COMMUNICATION
            Build.VERSION.SDK_INT >= 29 -> NativeLink.PRESET_VOICE_PERFORMANCE
            else -> 1   // generic
        }
        // The service first: Android only allows the microphone in the background for a foreground service.
        act.updateService(forceMic = true)
        val err = NativeLink.send(name, NativeLink.PORT, act.getString(R.string.stream_name), codec, 96, 1, preset)
        if (err.isNotEmpty()) act.toast(err)
    }

    private fun restartMic() {
        val name = hydra()
        if (name.isEmpty() || last?.getJSONObject("tx")?.optBoolean("sending") != true) return
        NativeLink.stopSending()
        startMic(name)
    }

    // ---- devices of the phone --------------------------------------------------------------------------------
    private fun deviceName(d: AudioDeviceInfo): String {
        val product = d.productName?.toString()?.takeIf { it.isNotBlank() && !it.equals(Build.MODEL, true) }
        return when (d.type) {
            AudioDeviceInfo.TYPE_BUILTIN_SPEAKER -> act.getString(R.string.dev_speaker)
            AudioDeviceInfo.TYPE_BUILTIN_EARPIECE -> act.getString(R.string.dev_earpiece)
            AudioDeviceInfo.TYPE_BUILTIN_MIC -> act.getString(R.string.dev_mic)
            AudioDeviceInfo.TYPE_WIRED_HEADSET -> act.getString(R.string.dev_wired_headset)
            AudioDeviceInfo.TYPE_WIRED_HEADPHONES -> act.getString(R.string.dev_wired_headphones)
            AudioDeviceInfo.TYPE_BLUETOOTH_A2DP, AudioDeviceInfo.TYPE_BLUETOOTH_SCO -> "Bluetooth" + (product?.let { ": $it" } ?: "")
            AudioDeviceInfo.TYPE_USB_HEADSET, AudioDeviceInfo.TYPE_USB_DEVICE, AudioDeviceInfo.TYPE_USB_ACCESSORY -> "USB" + (product?.let { ": $it" } ?: "")
            else -> if (Build.VERSION.SDK_INT >= 31 && (d.type == AudioDeviceInfo.TYPE_BLE_HEADSET || d.type == AudioDeviceInfo.TYPE_BLE_SPEAKER))
                "Bluetooth" + (product?.let { ": $it" } ?: "") else product ?: act.getString(R.string.dev_other)
        }
    }

    /** The device AAudio uses (its id), else what Android routes to by default (Bluetooth > wired > built-in). */
    private fun deviceText(output: Boolean, id: Int): String {
        val am = act.getSystemService(AudioManager::class.java) ?: return ""
        val list = am.getDevices(if (output) AudioManager.GET_DEVICES_OUTPUTS else AudioManager.GET_DEVICES_INPUTS)
        list.firstOrNull { it.id == id && id != 0 }?.let { return deviceName(it) }
        val order = if (output) listOf(AudioDeviceInfo.TYPE_BLUETOOTH_A2DP, AudioDeviceInfo.TYPE_USB_HEADSET, AudioDeviceInfo.TYPE_WIRED_HEADPHONES,
                                       AudioDeviceInfo.TYPE_WIRED_HEADSET, AudioDeviceInfo.TYPE_BUILTIN_SPEAKER)
                    else listOf(AudioDeviceInfo.TYPE_USB_HEADSET, AudioDeviceInfo.TYPE_WIRED_HEADSET, AudioDeviceInfo.TYPE_BUILTIN_MIC)
        for (t in order) list.firstOrNull { it.type == t }?.let { return deviceName(it) }
        return list.firstOrNull()?.let { deviceName(it) } ?: ""
    }

    // ---- refresh (10x per second) ----------------------------------------------------------------------------
    fun refresh(s: JSONObject) {
        last = s
        phoneName.text = s.optString("name")
        phoneAddr.text = act.wifiAddress()?.let { act.getString(R.string.phone_address, it) } ?: act.getString(R.string.no_wifi)
        phoneAddr.setTextColor(if (act.wifiAddress() == null) Hc.mute else Hc.dim)
        ensureAudio(s)

        val name = hydra()
        val peer = hydraPeer(s)
        val paired = peer?.optBoolean("trusted") == true
        val connected = name.isNotEmpty() && paired
        val rx = s.getJSONObject("rx")
        val tx = s.getJSONObject("tx")

        // Hydra card
        hydraTitle.text = if (name.isEmpty()) act.getString(R.string.connect_title) else name
        hydraButton.visibility = if (name.isEmpty()) View.GONE else View.VISIBLE
        val (dotColor, status) = when {
            name.isEmpty() -> Hc.faint to act.getString(R.string.connect_sub)
            peer == null -> Hc.faint to act.getString(R.string.hydra_offline, name)
            paired -> Hc.good to act.getString(R.string.hydra_connected, peer.optString("address"))
            peer.optInt("pair") == NativeLink.PAIR_ASKING -> Hc.input to act.getString(R.string.hydra_asking, name)
            peer.optInt("pair") == NativeLink.PAIR_DENIED -> Hc.mute to act.getString(R.string.hydra_denied, name)
            peer.optString("id").isEmpty() -> Hc.mute to act.getString(R.string.hydra_old)
            else -> Hc.faint to act.getString(R.string.hydra_not_paired, name)
        }
        hydraDot.background = act.rounded(dotColor, 0, 4f)
        hydraStatus.text = status

        val key = s.optInt("changes") * 31 + name.hashCode()
        if (key != shownKey) {
            shownKey = key
            rebuildHydras(s, name, peer)
        }

        // Audio card
        audioCard.visibility = if (connected) View.VISIBLE else View.GONE
        if (!connected) return
        if (System.currentTimeMillis() - deviceTextAt > 1000) {
            deviceTextAt = System.currentTimeMillis()
            outDeviceText = deviceText(true, rx.optInt("deviceId"))
            inDeviceText = deviceText(false, tx.optInt("deviceId"))
        }
        val playMuted = prefs.getBoolean("playMuted", false)
        val micMuted = prefs.getBoolean("micMuted", false)
        val playDetail = when {
            !rx.optBoolean("receiving") -> act.getString(R.string.play_nothing, s.optString("name"))
            else -> String.format(act.getString(R.string.rx_stats), rx.optString("codec"), rx.optDouble("buffer") + rx.optDouble("device"), rx.optDouble("loss"))
        }
        playTile.set(playMuted, rx.optBoolean("listening"), outDeviceText, playDetail, rx.optDouble("peak").toFloat())
        val micDetail = when {
            !hasMic() -> act.getString(R.string.mic_permission)
            !tx.optBoolean("sending") -> ""
            !tx.optBoolean("heard") -> act.getString(R.string.mic_unheard, act.getString(R.string.stream_name), s.optString("name"))
            else -> String.format(act.getString(R.string.tx_stats), tx.optString("codec"), tx.optDouble("buffer"), tx.optDouble("loss"))
        }
        micTile.set(micMuted, tx.optBoolean("sending"), if (hasMic()) inDeviceText else act.getString(R.string.mic_tap_allow), micDetail, tx.optDouble("peak").toFloat())
    }

    /** Not connected: the Hydras in the Wi-Fi (tap = connect). Connected: nothing. */
    private fun rebuildHydras(s: JSONObject, name: String, peer: JSONObject?) {
        hydraList.removeAllViews()
        val peers = s.getJSONArray("peers")
        val paired = peer?.optBoolean("trusted") == true
        val showList = name.isEmpty() || (!paired && peer?.optInt("pair") != NativeLink.PAIR_ASKING)
        hydraList.visibility = if (showList) View.VISIBLE else View.GONE
        if (!showList) { hydraHint.visibility = View.GONE; return }
        var count = 0
        for (i in 0 until peers.length()) {
            val p = peers.getJSONObject(i)
            val n = p.optString("name")
            if (n.equals(name, true) && name.isNotEmpty()) continue
            val sub = when {
                p.optString("id").isEmpty() -> act.getString(R.string.hydra_old)
                p.optBoolean("trusted") -> act.getString(R.string.peer_paired, p.optString("address"))
                else -> act.getString(R.string.peer_tap, p.optString("address"))
            }
            hydraList.addView(act.listRow(n, sub, false, Hc.hw, R.drawable.ic_nav_mix) {
                if (p.optString("id").isNotEmpty()) connectTo(n)
            })
            ++count
        }
        hydraHint.text = when {
            count == 0 -> act.getString(R.string.no_hydra)
            name.isEmpty() -> act.getString(R.string.connect_hint)
            else -> act.getString(R.string.connect_other)
        }
        hydraHint.visibility = View.VISIBLE
    }
}
