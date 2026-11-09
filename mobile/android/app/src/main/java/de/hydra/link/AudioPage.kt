package de.hydra.link

import android.Manifest
import android.app.AlertDialog
import android.content.pm.PackageManager
import android.os.Build
import android.text.InputType
import android.view.Gravity
import android.view.View
import android.view.ViewGroup
import android.widget.EditText
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.Switch
import android.widget.TextView
import org.json.JSONObject

/** Audio over Hydra Link: listen to what Hydra sends to this phone, send the phone's microphone to a Hydra. */
class AudioPage(private val act: MainActivity) {
    private val prefs = act.prefs
    val view: ScrollView
    private val content: LinearLayout

    private val streamList: LinearLayout
    private val listenHint: TextView
    private val listenStats: TextView
    private val listenMeter: Meter
    private val listenStop: TextView
    private val hydraList: LinearLayout
    private val sendStats: TextView
    private val sendHint: TextView
    private val sendMeter: Meter
    private val sendButton: TextView
    private val qualityPcm: LinearLayout
    private val qualityOpus: LinearLayout
    private val voice: Switch
    private val phoneName: TextView
    private val phoneAddr: TextView

    private var shownKey = -1
    private var pendingSend = false
    private var last: JSONObject? = null

    init {
        content = LinearLayout(act).apply { orientation = LinearLayout.VERTICAL }
        view = ScrollView(act).apply {
            isFillViewport = true
            setBackgroundColor(Hc.bg)
            clipToPadding = false
            addView(content)
        }

        // This phone
        val phone = Card(act, act.getString(R.string.phone_card), Hc.hw)
        val phoneRow = LinearLayout(act).apply { orientation = LinearLayout.HORIZONTAL; gravity = Gravity.CENTER_VERTICAL }
        val phoneTexts = LinearLayout(act).apply { orientation = LinearLayout.VERTICAL }
        phoneName = act.label("", 16f, Hc.text, true)
        phoneAddr = act.label("", 12f, Hc.dim)
        phoneTexts.addView(phoneName)
        phoneTexts.addView(phoneAddr, lp(top = act.dpi(4)))
        phoneRow.addView(phoneTexts, LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f))
        phoneRow.addView(act.hbutton(act.getString(R.string.rename)) { act.renameDialog() })
        phone.add(phoneRow, 0)
        content.addView(phone, lp(bottom = act.dpi(12)))

        // Listen
        val listen = Card(act, act.getString(R.string.listen), Hc.virt)
        streamList = LinearLayout(act).apply { orientation = LinearLayout.VERTICAL }
        listenHint = act.label("", 13f, Hc.dim).apply { setLineSpacing(0f, 1.2f) }
        listenMeter = Meter(act, Hc.virt)
        listenStats = act.label("", 12f, Hc.dim)
        listenStop = act.hbutton(act.getString(R.string.stop_listening)) {
            NativeLink.listen("", "")
            act.updateService()
        }
        listen.add(streamList, 0)
        listen.add(listenHint, 4)
        listen.add(listenMeter, 10)
        listen.add(act.gainSlider(act.getString(R.string.volume), prefs.getFloat("play", 1f), Hc.virt) { g ->
            prefs.edit().putFloat("play", g).apply(); act.applyGains()
        }, 8)
        listen.add(listenStats, 6)
        listen.add(listenStop, 12)

        // Send
        val send = Card(act, act.getString(R.string.send), Hc.input)
        hydraList = LinearLayout(act).apply { orientation = LinearLayout.VERTICAL }
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
            setOnCheckedChangeListener { _, on -> prefs.edit().putBoolean("voice", on).apply() }
        }
        sendMeter = Meter(act, Hc.input)
        sendStats = act.label("", 12f, Hc.dim)
        sendHint = act.label("", 13f, Hc.dim).apply { setLineSpacing(0f, 1.2f) }
        sendButton = act.hbutton(act.getString(R.string.start_sending), primary = true) { toggleSend() }
        send.add(hydraList, 0)
        send.add(act.hbutton(act.getString(R.string.other_address)) { addressDialog() }, 2)
        send.add(act.label(act.getString(R.string.quality).uppercase(), 11f, Hc.dim, true).apply { letterSpacing = 0.09f }, 14)
        send.add(quality, 8)
        send.add(voice, 10)
        send.add(sendButton, 12)
        send.add(sendMeter, 12)
        send.add(act.gainSlider(act.getString(R.string.mic_level), prefs.getFloat("mic", 1f), Hc.input) { g ->
            prefs.edit().putFloat("mic", g).apply(); act.applyGains()
        }, 8)
        send.add(sendStats, 6)
        send.add(sendHint, 10)

        content.addView(listen, lp(bottom = act.dpi(12)))
        content.addView(send, lp(bottom = act.dpi(12)))
        content.addView(act.label(act.getString(R.string.footer), 12f, Hc.faint).apply { setPadding(act.dpi(4), act.dpi(4), act.dpi(4), 0); setLineSpacing(0f, 1.2f) })
        setCodec(prefs.getInt("codec", NativeLink.CODEC_PCM24))
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

    private fun setCodec(c: Int) {
        prefs.edit().putInt("codec", c).apply()
        for ((seg, on) in listOf(qualityPcm to (c != NativeLink.CODEC_OPUS), qualityOpus to (c == NativeLink.CODEC_OPUS))) {
            seg.background = act.rounded(if (on) blend(Hc.input, Hc.panel2, 0.18f) else Hc.panel2, if (on) Hc.input else Hc.outline, 6f)
        }
    }

    // ---- actions ---------------------------------------------------------------------------------------------
    private fun addressDialog() {
        val edit = EditText(act).apply {
            setText(prefs.getString("manual", "") ?: "")
            hint = "192.168.1.20"
            inputType = InputType.TYPE_CLASS_PHONE
            setSingleLine()
        }
        AlertDialog.Builder(act, android.R.style.Theme_DeviceDefault_Dialog_Alert)
            .setTitle(R.string.other_address)
            .setMessage(R.string.address_message)
            .setView(edit)
            .setPositiveButton(android.R.string.ok) { _, _ ->
                val a = edit.text.toString().trim()
                prefs.edit().putString("manual", a).apply()
                if (a.isNotEmpty()) prefs.edit().putString("hydra", a).apply()
                shownKey = -1
            }
            .setNegativeButton(android.R.string.cancel, null)
            .show()
    }

    private fun chosenHydra() = prefs.getString("hydra", "") ?: ""

    private fun toggleSend() {
        if (last?.optJSONObject("tx")?.optBoolean("sending") == true) {
            NativeLink.stopSending()
            act.updateService()
            return
        }
        if (chosenHydra().isEmpty()) { sendHint.text = act.getString(R.string.choose_hydra_first); return }
        if (act.checkSelfPermission(Manifest.permission.RECORD_AUDIO) != PackageManager.PERMISSION_GRANTED) {
            pendingSend = true
            act.requestPermissions(arrayOf(Manifest.permission.RECORD_AUDIO), MainActivity.REQ_MIC)
            return
        }
        startSending()
    }

    fun onMicPermission(granted: Boolean) {
        if (!pendingSend) return
        pendingSend = false
        if (granted) startSending() else sendHint.text = act.getString(R.string.mic_denied)
    }

    private fun startSending() {
        val codec = prefs.getInt("codec", NativeLink.CODEC_PCM24)
        val preset = when {
            voice.isChecked -> NativeLink.PRESET_VOICE_COMMUNICATION
            Build.VERSION.SDK_INT >= 29 -> NativeLink.PRESET_VOICE_PERFORMANCE
            else -> 1   // generic
        }
        // The service first: Android only allows the microphone in the background for a foreground service.
        act.updateService(forceMic = true)
        val err = NativeLink.send(chosenHydra(), NativeLink.PORT, act.getString(R.string.stream_name), codec, 96, 1, preset)
        if (err.isNotEmpty()) sendHint.text = err
        act.updateService()
    }

    private fun listenTo(stream: String, sender: String) {
        act.updateService()
        val err = NativeLink.listen(stream, sender)
        if (err.isNotEmpty()) act.toast(err)
        act.handlerPostDelayed(300) { act.updateService() }
    }

    // ---- refresh (10x per second) ----------------------------------------------------------------------------
    fun refresh(s: JSONObject) {
        last = s
        phoneName.text = s.optString("name")
        phoneAddr.text = act.wifiAddress()?.let { act.getString(R.string.phone_address, it) } ?: act.getString(R.string.no_wifi)
        phoneAddr.setTextColor(if (act.wifiAddress() == null) Hc.mute else Hc.dim)
        val rx = s.getJSONObject("rx")
        val tx = s.getJSONObject("tx")
        val key = s.optInt("changes") * 31 + (rx.optString("stream") + "@" + rx.optString("sender") + rx.optBoolean("listening")).hashCode() +
            (tx.optBoolean("sending").toString() + chosenHydra()).hashCode()
        if (key != shownKey) {
            shownKey = key
            rebuildStreams(s, rx)
            rebuildHydras(s, tx)
        }

        listenMeter.set(rx.optDouble("peak").toFloat())
        val listening = rx.optBoolean("listening")
        listenStop.visibility = if (listening) View.VISIBLE else View.GONE
        listenMeter.visibility = if (listening) View.VISIBLE else View.GONE
        listenStats.text = when {
            !listening -> ""
            !rx.optBoolean("receiving") -> act.getString(R.string.waiting_for, rx.optString("stream"), rx.optString("sender"))
            else -> String.format(act.getString(R.string.rx_stats), rx.optString("codec"), rx.optDouble("buffer") + rx.optDouble("device"),
                rx.optDouble("loss"), rx.optInt("underruns"))
        }

        val sending = tx.optBoolean("sending")
        sendButton.text = act.getString(if (sending) R.string.stop_sending else R.string.start_sending)
        sendButton.style(true, if (sending) Hc.mute else Hc.input)
        sendMeter.set(tx.optDouble("peak").toFloat())
        sendMeter.visibility = if (sending) View.VISIBLE else View.GONE
        for (seg in listOf(qualityPcm, qualityOpus)) { seg.alpha = if (sending) 0.5f else 1f; seg.isEnabled = !sending }
        voice.isEnabled = !sending
        sendStats.text = when {
            !sending -> ""
            !tx.optBoolean("reachable") -> act.getString(R.string.looking_for, tx.optString("to"))
            !tx.optBoolean("heard") -> act.getString(R.string.sending_unheard, tx.optString("codec"), tx.optString("to"))
            else -> String.format(act.getString(R.string.tx_stats), tx.optString("codec"), tx.optString("to"), tx.optDouble("buffer"), tx.optDouble("loss"))
        }
        sendHint.text = act.getString(R.string.send_hint, act.getString(R.string.stream_name), s.optString("name"))
    }

    private fun rebuildStreams(s: JSONObject, rx: JSONObject) {
        streamList.removeAllViews()
        val streams = s.getJSONArray("streams")
        val listening = rx.optBoolean("listening")
        for (i in 0 until streams.length()) {
            val st = streams.getJSONObject(i)
            val name = st.optString("stream")
            val sender = st.optString("sender")
            val active = listening && name.equals(rx.optString("stream"), true) && sender.equals(rx.optString("sender"), true)
            val codec = st.optString("codec") + if (st.optInt("kbps") > 0) " ${st.optInt("kbps")} kbit/s" else ""
            streamList.addView(act.listRow(act.getString(R.string.stream_from, name, sender),
                codec + if (active) " · " + act.getString(R.string.playing) else "", active, Hc.virt, R.drawable.ic_nav_audio) {
                if (!active) listenTo(name, sender)
            })
        }
        listenHint.text = if (streams.length() == 0) act.getString(R.string.listen_hint, s.optString("name")) else ""
        listenHint.visibility = if (streams.length() == 0) View.VISIBLE else View.GONE
    }

    private fun rebuildHydras(s: JSONObject, tx: JSONObject) {
        hydraList.removeAllViews()
        val names = mutableListOf<Pair<String, String>>()
        val peers = s.getJSONArray("peers")
        for (i in 0 until peers.length()) {
            val p = peers.getJSONObject(i)
            names += p.optString("name") to p.optString("address")
        }
        val manual = prefs.getString("manual", "") ?: ""
        if (manual.isNotEmpty() && names.none { it.second == manual }) names += manual to act.getString(R.string.manual_address)
        if (chosenHydra().isEmpty() && names.size == 1) prefs.edit().putString("hydra", names[0].first).apply()
        val sending = tx.optBoolean("sending")
        for ((name, addr) in names) {
            val active = name.equals(chosenHydra(), true)
            hydraList.addView(act.listRow(name, addr, active, Hc.input, R.drawable.ic_nav_out) {
                if (sending) return@listRow
                prefs.edit().putString("hydra", name).apply()
                shownKey = -1
            })
        }
        if (names.isEmpty()) hydraList.addView(act.label(act.getString(R.string.no_hydra), 13f, Hc.dim).apply { setLineSpacing(0f, 1.2f); gravity = Gravity.START })
    }
}
