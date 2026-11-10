package de.hydra.link

import android.annotation.SuppressLint
import android.app.AlertDialog
import android.graphics.Bitmap
import android.text.InputType
import android.view.Gravity
import android.view.View
import android.view.ViewGroup
import android.webkit.JavascriptInterface
import android.webkit.WebResourceError
import android.webkit.WebResourceRequest
import android.webkit.WebSettings
import android.webkit.WebView
import android.webkit.WebViewClient
import android.widget.EditText
import android.widget.FrameLayout
import android.widget.ImageView
import android.widget.LinearLayout
import android.widget.TextView
import org.json.JSONObject

/** Hydra Remote inside the app: the web control Hydra serves (resources/remote/index.html, ?embed=1), shown in a
    WebView without its own header and navigation. The app's navigation switches its views (hydra.setView), the
    page reports its status back (HydraApp.status). Which Hydra: the one this phone is connected to (Audio page);
    its access code comes with the pairing (Hydra Link), so nothing has to be typed. Or an address entered by hand. */
@SuppressLint("SetJavaScriptEnabled")
class MixerPage(private val act: MainActivity) {
    private val prefs = act.prefs
    val view = FrameLayout(act)
    private val web = WebView(act)
    private val empty = LinearLayout(act)
    private val emptyTitle: TextView
    private val emptyText: TextView
    private var loadedUrl = ""
    private var pageOk = false
    private var wantView = "in"
    private var failedAt = 0L
    private lateinit var emptyAction: TextView

    init {
        view.setBackgroundColor(Hc.bg)
        web.setBackgroundColor(Hc.bg)
        web.overScrollMode = View.OVER_SCROLL_NEVER
        web.isVerticalScrollBarEnabled = false
        web.isHorizontalScrollBarEnabled = false
        with(web.settings) {
            javaScriptEnabled = true
            domStorageEnabled = true        // the access code stays in the page's localStorage
            cacheMode = WebSettings.LOAD_NO_CACHE
            builtInZoomControls = false
            setSupportZoom(false)
        }
        web.addJavascriptInterface(Bridge(), "HydraApp")
        web.webViewClient = object : WebViewClient() {
            override fun onPageStarted(v: WebView?, url: String?, favicon: Bitmap?) { pageOk = true }
            override fun onPageFinished(v: WebView?, url: String?) { if (pageOk) { showWeb(true); setView(wantView) } }
            override fun onReceivedError(v: WebView?, req: WebResourceRequest?, err: WebResourceError?) {
                if (req?.isForMainFrame == true) {
                    pageOk = false
                    failedAt = System.currentTimeMillis()
                    showEmpty(act.getString(R.string.mixer_unreachable_title), act.getString(R.string.mixer_unreachable, loadedUrl.substringAfter("//").substringBefore("/")))
                }
            }
            override fun shouldOverrideUrlLoading(v: WebView?, req: WebResourceRequest?) = false
        }
        view.addView(web, FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT))

        empty.orientation = LinearLayout.VERTICAL
        empty.gravity = Gravity.CENTER
        empty.setPadding(act.dpi(32), act.dpi(24), act.dpi(32), act.dpi(24))
        empty.addView(ImageView(act).apply { setImageResource(R.drawable.logo) }, LinearLayout.LayoutParams(act.dpi(54), act.dpi(51)).apply { bottomMargin = act.dpi(18) })
        emptyTitle = act.label("", 19f, Hc.text, true).apply { gravity = Gravity.CENTER }
        emptyText = act.label("", 14f, Hc.dim).apply { gravity = Gravity.CENTER; setLineSpacing(0f, 1.25f) }
        empty.addView(emptyTitle, lp(bottom = act.dpi(10)))
        empty.addView(emptyText, lp(bottom = act.dpi(22)))
        emptyAction = act.hbutton(act.getString(R.string.mixer_connect), primary = true) { act.selectTab("audio") }
        empty.addView(emptyAction, lp(bottom = act.dpi(8)))
        empty.addView(act.hbutton(act.getString(R.string.mixer_address)) { addressDialog() })
        view.addView(empty, FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT))
        showEmpty(act.getString(R.string.mixer_searching_title), act.getString(R.string.mixer_searching))
    }

    fun setInsets(l: Int, t: Int, r: Int, b: Int) {
        web.setPadding(0, 0, 0, 0)
        (web.layoutParams as FrameLayout.LayoutParams).setMargins(l, t, r, b)
        empty.setPadding(l + act.dpi(32), t + act.dpi(24), r + act.dpi(32), b + act.dpi(24))
        web.requestLayout()
    }

    private fun showWeb(on: Boolean) {
        web.visibility = if (on) View.VISIBLE else View.INVISIBLE
        empty.visibility = if (on) View.GONE else View.VISIBLE
    }

    private fun showEmpty(title: String, text: String) {
        emptyTitle.text = title
        emptyText.text = text
        showWeb(false)
        act.onMixerStatus(null)
    }

    /** Inputs / outputs / mixer / scenes. */
    fun setView(v: String) {
        wantView = v
        if (pageOk) web.evaluateJavascript("window.hydra && hydra.setView('$v')", null)
    }

    /** Loads the connected Hydra's page when it changed (called 10x per second with the endpoint's state). */
    fun update(s: JSONObject, hydra: String, peer: JSONObject?) {
        val url = targetUrl(hydra, peer)
        if (url == null) {
            if (loadedUrl.isNotEmpty() || emptyTitle.text.isEmpty()) {
                loadedUrl = ""
                web.loadUrl("about:blank")
            }
            emptyAction.visibility = View.VISIBLE
            when {
                hydra.isEmpty() -> showEmpty(act.getString(R.string.mixer_not_connected_title), act.getString(R.string.mixer_not_connected_text))
                peer == null -> showEmpty(act.getString(R.string.mixer_searching_title), act.getString(R.string.hydra_offline, hydra))
                peer.optBoolean("trusted") && peer.optInt("remote") <= 0 -> {
                    emptyAction.visibility = View.GONE
                    showEmpty(act.getString(R.string.mixer_remote_off_title), act.getString(R.string.mixer_remote_off, hydra))
                }
                else -> showEmpty(act.getString(R.string.mixer_not_connected_title), act.getString(R.string.mixer_waiting, hydra))
            }
            return
        }
        // After an error: try again every 5 s (Hydra started, Wi-Fi back).
        if (url == loadedUrl && !(failedAt > 0 && System.currentTimeMillis() - failedAt > 5000)) return
        loadedUrl = url
        failedAt = 0
        pageOk = false
        web.loadUrl(url)
    }

    private fun targetUrl(hydra: String, peer: JSONObject?): String? {
        val manual = prefs.getString("mixerHost", "") ?: ""
        if (manual.isNotEmpty()) {
            val host = if (manual.contains(":")) manual else "$manual:6981"
            return "http://$host/?embed=1"
        }
        if (hydra.isEmpty() || peer == null || !peer.optBoolean("trusted") || peer.optInt("remote") <= 0) return null
        // The code goes in the fragment (the page takes it from there); "c" changes the URL when the code changes,
        // so the page loads anew (a fragment alone would not reload it).
        val code = peer.optString("code")
        val base = "http://${peer.optString("address")}:${peer.optInt("remote")}/?embed=1"
        return if (code.isEmpty()) base else "$base&c=${Integer.toHexString(code.hashCode())}#k=$code"
    }

    private fun addressDialog() {
        val edit = EditText(act).apply {
            setText(prefs.getString("mixerHost", "") ?: "")
            hint = "192.168.1.20"
            inputType = InputType.TYPE_CLASS_TEXT or InputType.TYPE_TEXT_VARIATION_URI
            setSingleLine()
        }
        AlertDialog.Builder(act, android.R.style.Theme_DeviceDefault_Dialog_Alert)
            .setTitle(R.string.mixer_address)
            .setMessage(R.string.mixer_address_message)
            .setView(edit)
            .setPositiveButton(android.R.string.ok) { _, _ ->
                prefs.edit().putString("mixerHost", edit.text.toString().trim()).apply()
                loadedUrl = ""
            }
            .setNegativeButton(android.R.string.cancel, null)
            .show()
    }

    fun back(): Boolean = false

    private inner class Bridge {
        @JavascriptInterface
        fun status(json: String) {
            val o = try { JSONObject(json) } catch (e: Exception) { return }
            act.runOnUiThread { act.onMixerStatus(o) }
        }
    }
}
