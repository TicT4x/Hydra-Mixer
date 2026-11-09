package de.hydra.link

import android.content.Context
import android.content.res.ColorStateList
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
import android.graphics.PorterDuff
import android.graphics.RectF
import android.graphics.Typeface
import android.graphics.drawable.GradientDrawable
import android.util.TypedValue
import android.view.Gravity
import android.view.View
import android.view.ViewGroup
import android.widget.FrameLayout
import android.widget.ImageView
import android.widget.LinearLayout
import android.widget.SeekBar
import android.widget.TextView
import kotlin.math.exp
import kotlin.math.log10
import kotlin.math.max
import kotlin.math.min

/** Hydra's colours (ui/Theme.h, theme "Default"). */
object Hc {
    val bg = Color.rgb(0x0d, 0x0f, 0x13)
    val panel = Color.rgb(0x15, 0x19, 0x20)
    val panel2 = Color.rgb(0x1b, 0x20, 0x29)
    val raised = Color.rgb(0x23, 0x29, 0x35)
    val outline = Color.rgb(0x2a, 0x31, 0x3e)
    val text = Color.rgb(0xe8, 0xeb, 0xf1)
    val dim = Color.rgb(0x8b, 0x94, 0xa5)
    val faint = Color.rgb(0x5a, 0x62, 0x73)
    val input = Color.rgb(0x35, 0xd0, 0xf4)
    val hw = Color.rgb(0x8d, 0x6c, 0xff)
    val virt = Color.rgb(0xff, 0x6a, 0xa6)
    val mute = Color.rgb(0xff, 0x4d, 0x5e)
    val good = Color.rgb(0x3e, 0xe0, 0x8f)
    val mono: Typeface = Typeface.create("monospace", Typeface.BOLD)
}

fun Context.dp(v: Float) = TypedValue.applyDimension(TypedValue.COMPLEX_UNIT_DIP, v, resources.displayMetrics)
fun Context.dpi(v: Int) = dp(v.toFloat()).toInt()

fun Context.label(text: String, size: Float = 14f, color: Int = Hc.text, bold: Boolean = false) = TextView(this).apply {
    this.text = text
    setTextColor(color)
    setTextSize(TypedValue.COMPLEX_UNIT_SP, size)
    if (bold) typeface = Typeface.create("sans-serif-medium", Typeface.NORMAL)
    includeFontPadding = false
}

fun Context.rounded(fill: Int, stroke: Int = Hc.outline, radius: Float = 6f) = GradientDrawable().apply {
    setColor(fill)
    cornerRadius = dp(radius)
    if (stroke != 0) setStroke(dpi(1), stroke)
}

fun Context.icon(res: Int, color: Int, sizeDp: Int = 22) = ImageView(this).apply {
    setImageResource(res)
    setColorFilter(color, PorterDuff.Mode.SRC_IN)
    layoutParams = LinearLayout.LayoutParams(dpi(sizeDp), dpi(sizeDp))
}

/** Hydra-style button: dark with an outline, or filled in the accent colour (primary). */
fun Context.hbutton(text: String, primary: Boolean = false, color: Int = Hc.input, onClick: () -> Unit) = TextView(this).apply {
    this.text = text
    gravity = Gravity.CENTER
    setTextSize(TypedValue.COMPLEX_UNIT_SP, 14f)
    typeface = Typeface.create("sans-serif-medium", Typeface.NORMAL)
    minHeight = dpi(44)
    setPadding(dpi(16), 0, dpi(16), 0)
    isClickable = true
    style(primary, color)
    setOnClickListener { onClick() }
}

fun TextView.style(primary: Boolean, color: Int = Hc.input) {
    setTextColor(if (primary) Hc.bg else Hc.text)
    background = context.rounded(if (primary) color else Hc.panel2, if (primary) color else Hc.outline, 6f)
}

fun lp(w: Int = ViewGroup.LayoutParams.MATCH_PARENT, h: Int = ViewGroup.LayoutParams.WRAP_CONTENT, weight: Float = 0f,
       top: Int = 0, bottom: Int = 0) = LinearLayout.LayoutParams(w, h, weight).apply { topMargin = top; bottomMargin = bottom }

/** Card as in Hydra: panel colour, fine outline, 2 dp stripe in the section colour at the top. */
class Card(context: Context, title: String, stripe: Int) : FrameLayout(context) {
    val body = LinearLayout(context).apply { orientation = LinearLayout.VERTICAL }
    private val paint = Paint().apply { color = stripe }

    init {
        background = context.rounded(Hc.panel)
        clipToOutline = true
        setWillNotDraw(false)
        val inner = LinearLayout(context).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(context.dpi(14), context.dpi(14), context.dpi(14), context.dpi(14))
        }
        inner.addView(context.label(title.uppercase(), 11f, Hc.dim, true).apply { letterSpacing = 0.09f })
        inner.addView(body, lp(top = context.dpi(10)))
        addView(inner)
    }

    fun add(v: View, top: Int = 8) = body.addView(v, lp(top = if (body.childCount == 0) 0 else context.dpi(top)))

    override fun onDraw(canvas: Canvas) {
        super.onDraw(canvas)
        canvas.drawRect(0f, 0f, width.toFloat(), context.dp(2f), paint)
    }
}

/** A tappable list row: title, subtitle, coloured when active. */
fun Context.listRow(title: String, sub: String, active: Boolean, color: Int, iconRes: Int, onClick: () -> Unit) = LinearLayout(this).apply {
    orientation = LinearLayout.HORIZONTAL
    gravity = Gravity.CENTER_VERTICAL
    background = rounded(if (active) blend(color, Hc.panel2, 0.18f) else Hc.panel2, if (active) color else Hc.outline, 6f)
    setPadding(dpi(12), dpi(10), dpi(12), dpi(10))
    addView(icon(iconRes, if (active) color else Hc.dim, 22), LinearLayout.LayoutParams(dpi(22), dpi(22)).apply { marginEnd = dpi(12) })
    val texts = LinearLayout(context).apply { orientation = LinearLayout.VERTICAL }
    texts.addView(label(title, 15f, Hc.text, true))
    if (sub.isNotEmpty()) texts.addView(label(sub, 12f, Hc.dim), lp(top = dpi(3)))
    addView(texts, lp(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f))
    isClickable = true
    setOnClickListener { onClick() }
    layoutParams = lp(bottom = dpi(6))
}

fun blend(a: Int, b: Int, t: Float) = Color.rgb(
    (Color.red(a) * t + Color.red(b) * (1 - t)).toInt(),
    (Color.green(a) * t + Color.green(b) * (1 - t)).toInt(),
    (Color.blue(a) * t + Color.blue(b) * (1 - t)).toInt())

fun SeekBar.tint(color: Int) {
    progressTintList = ColorStateList.valueOf(color)
    thumbTintList = ColorStateList.valueOf(color)
    progressBackgroundTintList = ColorStateList.valueOf(Hc.raised)
}

/** Level meter: peak (linear) shown -60 .. 0 dB, falls back gently. Two bars like Hydra's strips when stereo. */
class Meter(context: Context, private val colour: Int) : View(context) {
    private var level = 0f
    private var shown = 0f
    private var last = System.nanoTime()
    private val paint = Paint(Paint.ANTI_ALIAS_FLAG)
    private val rect = RectF()

    fun set(peak: Float) {
        val db = if (peak > 1e-6f) 20f * log10(peak) else -100f
        level = ((db + 60f) / 60f).coerceIn(0f, 1f)
        postInvalidateOnAnimation()
    }

    override fun onMeasure(w: Int, h: Int) = setMeasuredDimension(MeasureSpec.getSize(w), (6 * resources.displayMetrics.density).toInt())

    override fun onDraw(c: Canvas) {
        val now = System.nanoTime()
        val dt = min(0.2f, (now - last) / 1e9f)
        last = now
        // glide towards the value (the app delivers ~10 per second): fast up, slower down
        shown += (level - shown) * (1f - exp(-dt / (if (level > shown) 0.06f else 0.2f)))
        if (shown < 0.003f) shown = 0f
        if (kotlin.math.abs(level - shown) > 0.002f) postInvalidateOnAnimation()
        val r = height / 2f
        paint.color = Hc.bg
        rect.set(0f, 0f, width.toFloat(), height.toFloat())
        c.drawRoundRect(rect, r, r, paint)
        paint.color = if (shown > 0.98f) Hc.mute else colour
        rect.set(0f, 0f, width * shown, height.toFloat())
        c.drawRoundRect(rect, r, r, paint)
    }
}

/** Gain slider row ("Volume  ───●──  +0.0 dB"): 0 = off, 75 = 0 dB, 100 = +6 dB. */
fun Context.gainSlider(labelText: String, initial: Float, color: Int, onChange: (Float) -> Unit): View {
    fun posToGain(p: Int): Float = if (p <= 0) 0f else Math.pow(10.0, ((p - 75) * 0.24) / 20.0).toFloat()
    fun gainToPos(g: Float): Int = if (g <= 0f) 0 else (75 + 20 * log10(g.toDouble()) / 0.24).toInt().coerceIn(0, 100)
    fun text(g: Float) = if (g <= 0f) "off" else String.format("%+.1f dB", 20 * log10(g.toDouble()))
    val row = LinearLayout(this).apply { orientation = LinearLayout.HORIZONTAL; gravity = Gravity.CENTER_VERTICAL }
    val value = label(text(initial), 12f, Hc.text).apply { typeface = Hc.mono; gravity = Gravity.END }
    val bar = SeekBar(this).apply {
        max = 100
        progress = gainToPos(initial)
        tint(color)
        setOnSeekBarChangeListener(object : SeekBar.OnSeekBarChangeListener {
            override fun onProgressChanged(s: SeekBar?, p: Int, fromUser: Boolean) {
                val g = posToGain(p)
                value.text = text(g)
                onChange(g)
            }
            override fun onStartTrackingTouch(s: SeekBar?) {}
            override fun onStopTrackingTouch(s: SeekBar?) {}
        })
    }
    row.addView(label(labelText, 13f, Hc.dim), LinearLayout.LayoutParams(dpi(58), ViewGroup.LayoutParams.WRAP_CONTENT))
    row.addView(bar, LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f))
    row.addView(value, LinearLayout.LayoutParams(dpi(70), ViewGroup.LayoutParams.WRAP_CONTENT))
    return row
}
