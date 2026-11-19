package de.hydra.link

import android.annotation.SuppressLint
import android.content.Context
import android.content.SharedPreferences
import android.graphics.Rect
import android.hardware.camera2.CameraCaptureSession
import android.hardware.camera2.CameraCharacteristics
import android.hardware.camera2.CameraDevice
import android.hardware.camera2.CameraManager
import android.hardware.camera2.CameraMetadata
import android.hardware.camera2.CaptureRequest
import android.hardware.camera2.CaptureResult
import android.hardware.camera2.TotalCaptureResult
import android.hardware.camera2.params.ColorSpaceTransform
import android.hardware.camera2.params.MeteringRectangle
import android.hardware.camera2.params.RggbChannelVector
import android.media.MediaCodec
import android.media.MediaCodecInfo
import android.media.MediaFormat
import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.os.HandlerThread
import android.os.SystemClock
import android.util.Log
import android.util.Range
import android.util.Size
import android.view.OrientationEventListener
import android.view.Surface
import java.io.ByteArrayOutputStream
import java.nio.ByteBuffer
import java.nio.ByteOrder
import kotlin.math.abs
import kotlin.math.hypot
import kotlin.math.ln
import kotlin.math.pow
import kotlin.math.roundToInt

/** The phone's camera as Hydra's webcam ("Hydra Camera" on the PC, engine/PhoneCamera.h).

    Switched on (Camera tile), the phone only tells its Hydra that a camera is there. Hydra says once a second whether
    an app on the PC shows the camera; only then the camera opens and the hardware H.264 encoder runs (baseline
    profile, no B frames: lowest latency), its output goes straight into Hydra Link (NativeLink.pushVideo). Nobody
    watching for 3 s: camera and encoder close again (battery). A lost frame on the way: Hydra asks for a key frame.

    The picture is sent as the sensor delivers it, plus the turn that shows it upright (from the phone's
    orientation): Hydra turns it and fits it into the format the app chose. Everything runs on one camera thread.

    Camera settings (since 18.11.): the phone describes what its camera can (NativeLink.cameraControls: which
    camera, zoom, exposure, ISO, shutter, focus, white balance, scene modes, ...), Hydra draws that and sends back
    what the user changed there (NativeLink.cameraSettings). The values are kept here per camera (SharedPreferences),
    so they survive a restart and the next camera has its own. Most changes only rebuild the repeating request;
    camera, quality and frame rate reopen the camera. */
object CameraSender {
    private const val TAG = "HydraCamera"

    /** Ids of the controls (stable: Hydra sends them back; engine/NetLink.cpp CamCaps / CamSet). */
    private object Id {
        const val CAMERA = 1; const val QUALITY = 2; const val FPS = 3; const val ZOOM = 4; const val TORCH = 5
        const val STABILIZE = 6; const val RESET = 7; const val BITRATE = 8
        const val AE_MODE = 10; const val EV = 11; const val AE_LOCK = 12; const val ISO = 13; const val SHUTTER = 14
        const val ANTIBANDING = 15
        const val AF_MODE = 20; const val FOCUS_DIST = 21; const val FOCUS_POINT = 22; const val FOCUS_AGAIN = 23
        const val AWB_MODE = 30; const val KELVIN = 31; const val AWB_LOCK = 32; const val EFFECT = 34
        const val SCENE = 40; const val NOISE = 41; const val SHARPEN = 42
    }
    private const val K_TOGGLE = 0; private const val K_RANGE = 1; private const val K_CHOICE = 2
    private const val K_BUTTON = 3; private const val K_POINT = 4
    private const val F_USABLE = 1; private const val F_LOG = 2; private const val F_SECONDS = 4
    private const val F_DIOPTERS = 8; private const val F_LIVE = 16
    private const val G_CAMERA = 0; private const val G_EXPOSURE = 1; private const val G_FOCUS = 2
    private const val G_COLOUR = 3; private const val G_IMAGE = 4

    private var context: Context? = null
    private var thread: HandlerThread? = null
    private var handler: Handler? = null
    private var orientation: OrientationEventListener? = null
    @Volatile private var deviceDegrees = 0

    @Volatile var enabled = false; private set
    @Volatile var sending = false; private set
    @Volatile var lastError = ""; private set
    @Volatile var size = Size(0, 0); private set
    @Volatile var facingFront = false; private set
    /** The Hydra is reached over the USB cable (USB tethering): high bit rates, High profile. */
    @Volatile var overUsb = false; private set
    /** Bumped when Hydra changed camera or quality (the Audio page shows them too). */
    @Volatile var settingsVersion = 0; private set
    private var hydra = ""

    private var camera: CameraDevice? = null
    private var session: CameraCaptureSession? = null
    private var codec: MediaCodec? = null
    private var codecSurface: Surface? = null
    private var opening = false
    private var notWantedSince = 0L
    private var sensorDegrees = 90
    private var fps = 30
    private var sentRotation = 0
    private var mbps = 0              // chosen bit rate, 0 = automatic (depends on Wi-Fi / USB)
    private val mbpsChoices = listOf(0, 4, 8, 15, 25, 40, 60, 80)

    /** The chosen height (720 / 1080 / 1440 / 2160); "camHd" is the Full HD switch of the Audio page. */
    private fun camHeight(): Int {
        val p = prefs() ?: return 720
        return p.getInt("camHeight", if (p.getBoolean("camHd", false)) 1080 else 720)
    }

    /** Heights this lens gives as 16:9 video and the phone's H.264 encoder takes (720p / 1080p always). */
    private fun qualityChoices(): List<Int> {
        val c = cam ?: return listOf(720, 1080)
        val map = c.lens.get(CameraCharacteristics.SCALER_STREAM_CONFIGURATION_MAP) ?: c.ch.get(CameraCharacteristics.SCALER_STREAM_CONFIGURATION_MAP)
        val sizes = map?.getOutputSizes(MediaCodec::class.java) ?: emptyArray()
        val caps = try {
            android.media.MediaCodecList(android.media.MediaCodecList.REGULAR_CODECS).codecInfos
                .filter { it.isEncoder && it.supportedTypes.any { t -> t.equals(MediaFormat.MIMETYPE_VIDEO_AVC, true) } }
                .mapNotNull { it.getCapabilitiesForType(MediaFormat.MIMETYPE_VIDEO_AVC).videoCapabilities }
        } catch (e: Exception) { emptyList() }
        return listOf(720, 1080, 1440, 2160).filter { h ->
            if (h <= 1080) return@filter true
            val w = h * 16 / 9
            sizes.any { it.width == w && it.height == h } && caps.any { it.isSizeSupported(w, h) }
        }
    }

    // ---- the cameras and their settings --------------------------------------------------------------------
    /** id = the camera to open; physical = one lens of a logical multi-camera (e.g. the tele of many Samsungs,
        which is not in cameraIdList), streamed alone. ch = what the requests go to (the opened camera), lens = the
        lens itself (sizes, orientation, focal length). */
    private class Cam(val id: String, val physical: String?, val label: String, val front: Boolean,
                      val ch: CameraCharacteristics, val lens: CameraCharacteristics) {
        val key get() = if (physical != null) "$id/$physical" else id
    }
    private var cams: List<Cam> = emptyList()
    private var cam: Cam? = null
    private val vals = HashMap<Int, Float>()            // of the current camera (persisted per camera)
    private val presses = HashMap<Int, Float>()          // the last count of each button / point
    private var region: MeteringRectangle? = null        // focus / expose here (tap on the picture in Hydra)
    private var liveIso = 0; private var liveExposureNs = 0L; private var liveFocus = 0f
    private var liveCcm: ColorSpaceTransform? = null
    private var lastDescribe = 0L

    private fun prefs(): SharedPreferences? = context?.getSharedPreferences("hydra", Context.MODE_PRIVATE)

    /** Camera on for this Hydra. Camera, quality and the other settings come from the preferences (Audio page / Hydra). */
    fun enable(ctx: Context, hydraName: String): String {
        if (enabled && hydraName == hydra) { handler?.post { reconfigure() }; return "" }
        disable()
        context = ctx.applicationContext
        hydra = hydraName
        val hd = camHeight() >= 1080
        val err = NativeLink.startVideo(hydraName, NativeLink.PORT, if (hd) 1920 else 1280, if (hd) 1080 else 720, 30)
        if (err.isNotEmpty()) return err
        thread = HandlerThread("HydraCamera").apply { start() }
        handler = Handler(thread!!.looper)
        orientation = object : OrientationEventListener(context) {
            override fun onOrientationChanged(o: Int) { if (o != ORIENTATION_UNKNOWN) deviceDegrees = ((o + 45) / 90 * 90) % 360 }
        }.apply { if (canDetectOrientation()) enable() }
        enabled = true
        lastError = ""
        handler?.post { loadCameras(); describe() }
        handler?.post(poll)
        return ""
    }

    fun disable() {
        if (!enabled) return
        enabled = false
        NativeLink.stopVideo()
        orientation?.disable()
        orientation = null
        val h = handler
        val t = thread
        handler = null
        thread = null
        h?.removeCallbacksAndMessages(null)
        h?.post { close(); t?.quitSafely() }
    }

    /** Audio page: front or back camera (the first of that side; Hydra can choose among all). */
    fun chooseFacing(ctx: Context, front: Boolean) {
        val cm = ctx.getSystemService(CameraManager::class.java) ?: return
        val id = try {
            cm.cameraIdList.firstOrNull { (cm.getCameraCharacteristics(it).get(CameraCharacteristics.LENS_FACING) == CameraCharacteristics.LENS_FACING_FRONT) == front }
        } catch (e: Exception) { null } ?: return
        ctx.getSharedPreferences("hydra", Context.MODE_PRIVATE).edit().putString("camId", id).putBoolean("camFront", front).apply()
        handler?.post { reconfigure() }
    }

    /** Audio page changed the quality: reopen with it. */
    fun qualityChanged() { handler?.post { reconfigure() } }

    /** Camera / quality / frame rate changed: reload, tell Hydra, reopen if open. */
    private fun reconfigure() {
        loadCameras()
        if (camera != null || opening) close()   // the poll opens it again while Hydra wants pictures
        describe()
    }

    // ---- what Hydra wants (10x per second, camera thread) ----------------------------------------------------
    private val poll = object : Runnable {
        override fun run() {
            if (!enabled) return
            NativeLink.cameraSettings()?.let { applySettings(it) }
            val w = NativeLink.videoWish()
            val want = (w and 1) != 0
            val key = (w and 2) != 0
            val appHeight = w ushr 8
            val now = SystemClock.elapsedRealtime()
            val usb = (w and 4) != 0
            if (usb != overUsb) {
                // Cable plugged in / out: open again with the bit rate and profile of that way.
                overUsb = usb
                if (camera != null || opening) close()
                describe()
            }
            if (want) {
                notWantedSince = 0L
                if (camera == null && !opening) open(appHeight)
                else if (key) requestKeyFrame()
            } else if (camera != null || opening) {
                if (notWantedSince == 0L) notWantedSince = now
                else if (now - notWantedSince > 3000) close()
            }
            // The measured values (ISO, shutter, focus in auto modes) once a second while filming.
            if (sending && now - lastDescribe > 1000) describe()
            handler?.postDelayed(this, 100)
        }
    }

    private fun requestKeyFrame() {
        try { codec?.setParameters(Bundle().apply { putInt(MediaCodec.PARAMETER_KEY_REQUEST_SYNC_FRAME, 0) }) }
        catch (e: Exception) { Log.w(TAG, "key frame request", e) }
    }

    /** Quarter turns clockwise that show the sensor's picture upright (as JPEG_ORIENTATION). */
    private fun rotation(): Int {
        val d = if (facingFront) -deviceDegrees else deviceDegrees
        return ((sensorDegrees + d + 360) % 360) / 90
    }

    // ---- cameras ---------------------------------------------------------------------------------------------
    private fun loadCameras() {
        val ctx = context ?: return
        val cm = ctx.getSystemService(CameraManager::class.java) ?: return
        fun usable(ch: CameraCharacteristics) =
            CameraMetadata.REQUEST_AVAILABLE_CAPABILITIES_BACKWARD_COMPATIBLE in (ch.get(CameraCharacteristics.REQUEST_AVAILABLE_CAPABILITIES) ?: intArrayOf())
        fun front(ch: CameraCharacteristics) = ch.get(CameraCharacteristics.LENS_FACING) == CameraCharacteristics.LENS_FACING_FRONT
        // Focal length as on a 35 mm camera (names, and to see two entries are the same lens).
        fun equiv(ch: CameraCharacteristics): Float {
            val f = ch.get(CameraCharacteristics.LENS_INFO_AVAILABLE_FOCAL_LENGTHS)?.firstOrNull() ?: return 0f
            val sz = ch.get(CameraCharacteristics.SENSOR_INFO_PHYSICAL_SIZE) ?: return 0f
            val d = hypot(sz.width.toDouble(), sz.height.toDouble())
            return if (d > 0) (f * 43.27 / d).toFloat() else 0f
        }
        class Found(val id: String, val physical: String?, val ch: CameraCharacteristics, val lens: CameraCharacteristics)
        val found = ArrayList<Found>()
        fun same(a: CameraCharacteristics) = found.any { front(it.lens) == front(a) && abs(equiv(it.lens) - equiv(a)) < 1.5f && equiv(a) > 0f }
        val listed = try { cm.cameraIdList.toList() } catch (e: Exception) { Log.w(TAG, "cameras", e); emptyList() }
        for (id in listed) {
            val ch = try { cm.getCameraCharacteristics(id) } catch (e: Exception) { continue }
            if (usable(ch)) found.add(Found(id, null, ch, ch))   // depth sensors etc. are left out
        }
        // Lenses only reachable through a logical camera (Samsung: tele / ultra wide behind camera 0): streamed as
        // a physical camera of it. Not if an entry with that lens exists already.
        if (Build.VERSION.SDK_INT >= 28)
            for (f in found.toList()) {
                if (f.physical != null) continue
                val caps = f.ch.get(CameraCharacteristics.REQUEST_AVAILABLE_CAPABILITIES) ?: intArrayOf()
                if (CameraMetadata.REQUEST_AVAILABLE_CAPABILITIES_LOGICAL_MULTI_CAMERA !in caps) continue
                for (pid in f.ch.physicalCameraIds) {
                    if (pid in listed) continue
                    val pch = try { cm.getCameraCharacteristics(pid) } catch (e: Exception) { continue }
                    if (same(pch)) continue
                    found.add(Found(f.id, pid, f.ch, pch))
                }
            }
        // Main lens per side = the first listed one; several on one side: ultra wide / tele next to it.
        val main = HashMap<Boolean, Float>()
        val counts = HashMap<Boolean, Int>()
        for (f in found) {
            if (front(f.lens) !in main) main[front(f.lens)] = equiv(f.lens)
            counts[front(f.lens)] = (counts[front(f.lens)] ?: 0) + 1
        }
        cams = found.map { f ->
            val fr = front(f.lens)
            val e = equiv(f.lens)
            val m = main[fr] ?: 0f
            var label = if (fr) "Front" else "Back"
            if (e > 0f) label += " " + e.roundToInt() + " mm"
            if ((counts[fr] ?: 0) > 1 && m > 0f && e > 0f) {
                if (e < m * 0.75f) label += " (ultra wide)" else if (e > m * 1.6f) label += " (tele " + String.format("%.0f", e / m) + "x)"
            }
            Cam(f.id, f.physical, label, fr, f.ch, f.lens)
        }.sortedWith(compareBy({ it.front }, { equiv(it.lens) }))
        val p = prefs()
        val wantId = p?.getString("camId", null)
        val wantFront = p?.getBoolean("camFront", true) ?: true
        val c = cams.firstOrNull { it.key == wantId } ?: cams.firstOrNull { it.front == wantFront && it.physical == null } ?: cams.firstOrNull()
        if (c?.key != cam?.key) { region = null; liveCcm = null }
        cam = c
        if (c != null) facingFront = c.front
        loadValues()
    }

    private fun key(id: Int) = "cc." + (cam?.key ?: "") + "." + id

    private fun loadValues() {
        vals.clear()
        val p = prefs() ?: return
        for (id in listOf(Id.ZOOM, Id.TORCH, Id.STABILIZE, Id.AE_MODE, Id.EV, Id.AE_LOCK, Id.ISO, Id.SHUTTER, Id.ANTIBANDING,
                          Id.AF_MODE, Id.FOCUS_DIST, Id.AWB_MODE, Id.KELVIN, Id.AWB_LOCK, Id.EFFECT, Id.SCENE, Id.NOISE, Id.SHARPEN))
            if (p.contains(key(id))) vals[id] = p.getFloat(key(id), 0f)
        fps = p.getInt("camFps", 30)
        mbps = p.getInt("camMbps", 0)
    }

    private fun store(id: Int, v: Float) {
        vals[id] = v
        prefs()?.edit()?.putFloat(key(id), v)?.apply()
    }

    // What the camera offers (each list is the choice's options in order; index = the value Hydra sends).
    private fun chars() = cam?.ch
    private fun capabilities() = chars()?.get(CameraCharacteristics.REQUEST_AVAILABLE_CAPABILITIES) ?: intArrayOf()
    private fun manualSensor(): Boolean {
        val ch = chars() ?: return false
        return CameraMetadata.REQUEST_AVAILABLE_CAPABILITIES_MANUAL_SENSOR in capabilities()
            && CameraMetadata.CONTROL_AE_MODE_OFF in (ch.get(CameraCharacteristics.CONTROL_AE_AVAILABLE_MODES) ?: intArrayOf())
            && ch.get(CameraCharacteristics.SENSOR_INFO_SENSITIVITY_RANGE) != null
            && ch.get(CameraCharacteristics.SENSOR_INFO_EXPOSURE_TIME_RANGE) != null
    }
    private fun manualWhiteBalance(): Boolean =
        CameraMetadata.REQUEST_AVAILABLE_CAPABILITIES_MANUAL_POST_PROCESSING in capabilities()
            && CameraMetadata.CONTROL_AWB_MODE_OFF in (chars()?.get(CameraCharacteristics.CONTROL_AWB_AVAILABLE_MODES) ?: intArrayOf())

    private fun fpsChoices(): List<Int> {
        val r = chars()?.get(CameraCharacteristics.CONTROL_AE_AVAILABLE_TARGET_FPS_RANGES) ?: return listOf(30)
        val l = r.map { it.upper }.filter { it in listOf(15, 24, 25, 30, 48, 50, 60) }.distinct().sorted()
        return l.ifEmpty { listOf(30) }
    }

    private class Mode(val value: Int, val name: String)
    private fun afModes(): List<Mode> {
        val av = chars()?.get(CameraCharacteristics.CONTROL_AF_AVAILABLE_MODES) ?: intArrayOf()
        val l = ArrayList<Mode>()
        if (CameraMetadata.CONTROL_AF_MODE_CONTINUOUS_VIDEO in av) l.add(Mode(CameraMetadata.CONTROL_AF_MODE_CONTINUOUS_VIDEO, "Continuous"))
        if (CameraMetadata.CONTROL_AF_MODE_AUTO in av) l.add(Mode(CameraMetadata.CONTROL_AF_MODE_AUTO, "Single (focus, then hold)"))
        if (CameraMetadata.CONTROL_AF_MODE_OFF in av && minFocus() > 0f && manualSensor()) l.add(Mode(CameraMetadata.CONTROL_AF_MODE_OFF, "Manual"))
        return l
    }
    private fun minFocus() = chars()?.get(CameraCharacteristics.LENS_INFO_MINIMUM_FOCUS_DISTANCE) ?: 0f

    private fun awbModes(): List<Mode> {
        val av = chars()?.get(CameraCharacteristics.CONTROL_AWB_AVAILABLE_MODES) ?: intArrayOf()
        val names = listOf(CameraMetadata.CONTROL_AWB_MODE_AUTO to "Auto", CameraMetadata.CONTROL_AWB_MODE_DAYLIGHT to "Daylight",
            CameraMetadata.CONTROL_AWB_MODE_CLOUDY_DAYLIGHT to "Cloudy", CameraMetadata.CONTROL_AWB_MODE_SHADE to "Shade",
            CameraMetadata.CONTROL_AWB_MODE_TWILIGHT to "Twilight", CameraMetadata.CONTROL_AWB_MODE_INCANDESCENT to "Incandescent (bulb)",
            CameraMetadata.CONTROL_AWB_MODE_FLUORESCENT to "Fluorescent", CameraMetadata.CONTROL_AWB_MODE_WARM_FLUORESCENT to "Warm fluorescent")
        val l = names.filter { it.first in av }.map { Mode(it.first, it.second) }.toMutableList()
        if (l.isEmpty()) l.add(Mode(CameraMetadata.CONTROL_AWB_MODE_AUTO, "Auto"))
        if (manualWhiteBalance()) l.add(Mode(CameraMetadata.CONTROL_AWB_MODE_OFF, "Temperature"))
        return l
    }

    private fun sceneModes(): List<Mode> {
        val av = chars()?.get(CameraCharacteristics.CONTROL_AVAILABLE_SCENE_MODES) ?: intArrayOf()
        val names = mapOf(1 to "Face priority", 2 to "Action", 3 to "Portrait", 4 to "Landscape", 5 to "Night", 6 to "Night portrait",
            7 to "Theatre", 8 to "Beach", 9 to "Snow", 10 to "Sunset", 11 to "Steady", 12 to "Fireworks", 13 to "Sports", 14 to "Party",
            15 to "Candlelight", 16 to "Barcode", 18 to "HDR")
        return listOf(Mode(CameraMetadata.CONTROL_SCENE_MODE_DISABLED, "None")) +
            av.filter { it in names }.distinct().map { Mode(it, names[it]!!) }
    }

    private fun effects(): List<Mode> {
        val av = chars()?.get(CameraCharacteristics.CONTROL_AVAILABLE_EFFECTS) ?: intArrayOf()
        val names = mapOf(0 to "None", 1 to "Black & white", 2 to "Negative", 3 to "Solarize", 4 to "Sepia", 5 to "Posterize",
            6 to "Whiteboard", 7 to "Blackboard", 8 to "Aqua")
        val l = av.filter { it in names }.distinct().sorted().map { Mode(it, names[it]!!) }
        return if (l.size > 1) l else emptyList()
    }

    private fun antibanding(): List<Mode> {
        val av = chars()?.get(CameraCharacteristics.CONTROL_AE_AVAILABLE_ANTIBANDING_MODES) ?: intArrayOf()
        val names = listOf(3 to "Auto", 1 to "50 Hz (Europe)", 2 to "60 Hz (America)", 0 to "Off")
        return names.filter { it.first in av }.map { Mode(it.first, it.second) }
    }

    /** Noise reduction / sharpening: "Phone default" (not set) + what the camera has. */
    private fun processing(key: CameraCharacteristics.Key<IntArray>, names: Map<Int, String>): List<Mode> {
        val av = chars()?.get(key) ?: intArrayOf()
        val l = av.filter { it in names }.distinct().sorted().map { Mode(it, names[it]!!) }
        return if (l.size > 1) listOf(Mode(-1, "Phone default")) + l else emptyList()
    }
    private val nrNames = mapOf(0 to "Off", 3 to "Minimal", 1 to "Fast", 2 to "High quality")
    private val edgeNames = mapOf(0 to "Off", 1 to "Fast", 2 to "High quality")

    private class Stab(val ois: Boolean, val eis: Boolean, val name: String)
    private fun stabilization(): List<Stab> {
        val ch = chars()
        val ois = CameraMetadata.LENS_OPTICAL_STABILIZATION_MODE_ON in (ch?.get(CameraCharacteristics.LENS_INFO_AVAILABLE_OPTICAL_STABILIZATION) ?: intArrayOf())
        val eis = CameraMetadata.CONTROL_VIDEO_STABILIZATION_MODE_ON in (ch?.get(CameraCharacteristics.CONTROL_AVAILABLE_VIDEO_STABILIZATION_MODES) ?: intArrayOf())
        val l = arrayListOf(Stab(false, false, "Off"))
        if (ois) l.add(Stab(true, false, "Optical"))
        if (eis) l.add(Stab(false, true, "Digital (a little more delay)"))
        if (ois && eis) l.add(Stab(true, true, "Optical + digital"))
        return l
    }
    private fun stabDefault() = if (stabilization().size > 1 && stabilization()[1].ois) 1f else 0f

    private fun zoomRange(): Range<Float> {
        val ch = chars() ?: return Range(1f, 1f)
        if (cam?.physical != null) return Range(1f, 1f)   // one lens on its own: the zoom of the logical camera does not apply
        if (Build.VERSION.SDK_INT >= 30) ch.get(CameraCharacteristics.CONTROL_ZOOM_RATIO_RANGE)?.let { return it }
        return Range(1f, maxOf(1f, ch.get(CameraCharacteristics.SCALER_AVAILABLE_MAX_DIGITAL_ZOOM) ?: 1f))
    }

    private fun evStep(): Float {
        val r = chars()?.get(CameraCharacteristics.CONTROL_AE_COMPENSATION_STEP) ?: return 0f
        return if (r.denominator != 0) r.numerator.toFloat() / r.denominator else 0f
    }

    private fun value(id: Int, def: Float) = vals[id] ?: def
    private fun choice(id: Int, size: Int, def: Int = 0) = value(id, def.toFloat()).roundToInt().coerceIn(0, maxOf(0, size - 1))
    private fun aeManual() = manualSensor() && choice(Id.AE_MODE, 2) == 1
    private fun afMode(): Int = afModes().let { if (it.isEmpty()) -1 else it[choice(Id.AF_MODE, it.size)].value }
    private fun awbMode(): Int = awbModes().let { it[choice(Id.AWB_MODE, it.size)].value }
    private fun shutterMax(): Float {
        val r = chars()?.get(CameraCharacteristics.SENSOR_INFO_EXPOSURE_TIME_RANGE) ?: return 1f / 30
        return minOf(r.upper / 1e9f, 1f / fps)   // never longer than a frame: the frame rate stays
    }

    // ---- the control list for Hydra --------------------------------------------------------------------------
    private class ListOut {
        val b = ByteArrayOutputStream()
        var count = 0
        fun u8(v: Int) = b.write(v and 0xFF)
        fun f32(f: Float) { val v = java.lang.Float.floatToIntBits(f); for (i in 0 until 4) u8(v ushr (8 * i)) }
        fun str(s: String) { val a = s.toByteArray(Charsets.UTF_8).let { if (it.size > 63) it.copyOf(63) else it }; u8(a.size); b.write(a) }
        fun add(id: Int, kind: Int, group: Int, label: String, min: Float, max: Float, step: Float, value: Float,
                flags: Int = F_USABLE, unit: String = "", options: List<String> = emptyList()) {
            ++count
            u8(id); u8(kind); u8(flags); u8(group); str(label); f32(min); f32(max); f32(step); f32(value); str(unit)
            u8(options.size); for (o in options) str(o)
        }
        fun bytes(): ByteArray { val a = b.toByteArray(); return byteArrayOf(count.toByte()) + a }
    }

    private fun describe() {
        lastDescribe = SystemClock.elapsedRealtime()
        val ch = chars()
        val o = ListOut()
        if (ch == null) { NativeLink.cameraControls(o.bytes()); return }
        val p = prefs()

        // Camera
        o.add(Id.CAMERA, K_CHOICE, G_CAMERA, "Camera", 0f, (cams.size - 1).toFloat(), 1f, maxOf(0, cams.indexOf(cam)).toFloat(), options = cams.map { it.label })
        val ql = qualityChoices()
        o.add(Id.QUALITY, K_CHOICE, G_CAMERA, "Quality", 0f, (ql.size - 1).toFloat(), 1f, maxOf(0, ql.indexOf(ql.lastOrNull { it <= camHeight() } ?: 720)).toFloat(),
              options = ql.map { (if (it >= 2160) "4K (2160p)" else "${it}p") + (if (it >= 1440 && !overUsb) " (USB cable recommended)" else "") })
        val auto = autoBitrate(camHeight()) / 1_000_000
        o.add(Id.BITRATE, K_CHOICE, G_CAMERA, "Bit rate", 0f, (mbpsChoices.size - 1).toFloat(), 1f, maxOf(0, mbpsChoices.indexOf(mbps)).toFloat(),
              options = mbpsChoices.map { if (it == 0) "Auto ($auto Mbit/s, " + (if (overUsb) "USB cable" else "Wi-Fi") + ")" else "$it Mbit/s" + (if (it > 8 && !overUsb) " (USB)" else "") })
        val fl = fpsChoices()
        if (fl.size > 1) o.add(Id.FPS, K_CHOICE, G_CAMERA, "Frame rate", 0f, (fl.size - 1).toFloat(), 1f,
                               maxOf(0, fl.indexOf(fps).let { if (it < 0) fl.indexOf(30) else it }).toFloat(), options = fl.map { "$it fps" })
        val z = zoomRange()
        if (z.upper > z.lower) o.add(Id.ZOOM, K_RANGE, G_CAMERA, "Zoom", z.lower, z.upper, 0.01f, value(Id.ZOOM, 1f).coerceIn(z.lower, z.upper), F_USABLE or F_LOG, "x")
        if (ch.get(CameraCharacteristics.FLASH_INFO_AVAILABLE) == true) o.add(Id.TORCH, K_TOGGLE, G_CAMERA, "Light", 0f, 1f, 1f, value(Id.TORCH, 0f))
        val st = stabilization()
        if (st.size > 1) o.add(Id.STABILIZE, K_CHOICE, G_CAMERA, "Stabilization", 0f, (st.size - 1).toFloat(), 1f,
                               choice(Id.STABILIZE, st.size, stabDefault().toInt()).toFloat(), options = st.map { it.name })

        // Exposure
        val manual = manualSensor()
        if (manual) o.add(Id.AE_MODE, K_CHOICE, G_EXPOSURE, "Exposure", 0f, 1f, 1f, if (aeManual()) 1f else 0f, options = listOf("Auto", "Manual"))
        val ev = ch.get(CameraCharacteristics.CONTROL_AE_COMPENSATION_RANGE)
        val step = evStep()
        if (ev != null && ev.upper > ev.lower && step > 0f)
            o.add(Id.EV, K_RANGE, G_EXPOSURE, "Brightness", ev.lower * step, ev.upper * step, step, value(Id.EV, 0f),
                  if (aeManual()) 0 else F_USABLE, "EV")
        if (ch.get(CameraCharacteristics.CONTROL_AE_LOCK_AVAILABLE) == true)
            o.add(Id.AE_LOCK, K_TOGGLE, G_EXPOSURE, "Hold exposure", 0f, 1f, 1f, value(Id.AE_LOCK, 0f), if (aeManual()) 0 else F_USABLE)
        if (manual) {
            val iso = ch.get(CameraCharacteristics.SENSOR_INFO_SENSITIVITY_RANGE)!!
            val isoV = if (aeManual()) value(Id.ISO, if (liveIso > 0) liveIso.toFloat() else iso.lower.toFloat()) else liveIso.toFloat()
            o.add(Id.ISO, K_RANGE, G_EXPOSURE, "ISO", iso.lower.toFloat(), iso.upper.toFloat(), 1f, isoV.coerceIn(iso.lower.toFloat(), iso.upper.toFloat()),
                  (if (aeManual()) F_USABLE else F_LIVE) or F_LOG)
            val er = ch.get(CameraCharacteristics.SENSOR_INFO_EXPOSURE_TIME_RANGE)!!
            val sMin = er.lower / 1e9f
            val sMax = maxOf(sMin, shutterMax())
            val sV = if (aeManual()) value(Id.SHUTTER, if (liveExposureNs > 0) liveExposureNs / 1e9f else 1f / 60) else liveExposureNs / 1e9f
            o.add(Id.SHUTTER, K_RANGE, G_EXPOSURE, "Shutter", sMin, sMax, 0f, sV.coerceIn(sMin, sMax),
                  (if (aeManual()) F_USABLE else F_LIVE) or F_LOG or F_SECONDS, "s")
        }
        val ab = antibanding()
        if (ab.size > 1) o.add(Id.ANTIBANDING, K_CHOICE, G_EXPOSURE, "Against flicker", 0f, (ab.size - 1).toFloat(), 1f,
                               choice(Id.ANTIBANDING, ab.size).toFloat(), options = ab.map { it.name })

        // Focus
        val af = afModes()
        if (af.isNotEmpty()) {
            if (af.size > 1) o.add(Id.AF_MODE, K_CHOICE, G_FOCUS, "Focus", 0f, (af.size - 1).toFloat(), 1f, choice(Id.AF_MODE, af.size).toFloat(), options = af.map { it.name })
            val maxRegions = ch.get(CameraCharacteristics.CONTROL_MAX_REGIONS_AF) ?: 0
            if (afMode() != CameraMetadata.CONTROL_AF_MODE_OFF) {
                o.add(Id.FOCUS_AGAIN, K_BUTTON, G_FOCUS, "Focus again", 0f, 0f, 0f, 0f)
                if (maxRegions > 0) o.add(Id.FOCUS_POINT, K_POINT, G_FOCUS, "Focus on a point (click the picture)", 0f, 1f, 0f, 0f)
            }
            if (minFocus() > 0f && manual) {
                val mf = afMode() == CameraMetadata.CONTROL_AF_MODE_OFF
                o.add(Id.FOCUS_DIST, K_RANGE, G_FOCUS, "Focus distance", 0f, minFocus(), 0f,
                      (if (mf) value(Id.FOCUS_DIST, liveFocus) else liveFocus).coerceIn(0f, minFocus()), if (mf) F_USABLE or F_DIOPTERS else F_LIVE or F_DIOPTERS)
            }
        } else if ((ch.get(CameraCharacteristics.CONTROL_MAX_REGIONS_AE) ?: 0) > 0)
            o.add(Id.FOCUS_POINT, K_POINT, G_EXPOSURE, "Expose for a point (click the picture)", 0f, 1f, 0f, 0f)

        // Colour
        val awb = awbModes()
        if (awb.size > 1) o.add(Id.AWB_MODE, K_CHOICE, G_COLOUR, "White balance", 0f, (awb.size - 1).toFloat(), 1f, choice(Id.AWB_MODE, awb.size).toFloat(), options = awb.map { it.name })
        if (manualWhiteBalance())
            o.add(Id.KELVIN, K_RANGE, G_COLOUR, "Colour temperature", 2000f, 10000f, 50f, value(Id.KELVIN, 5500f),
                  if (awbMode() == CameraMetadata.CONTROL_AWB_MODE_OFF) F_USABLE else 0, "K")
        if (ch.get(CameraCharacteristics.CONTROL_AWB_LOCK_AVAILABLE) == true)
            o.add(Id.AWB_LOCK, K_TOGGLE, G_COLOUR, "Hold white balance", 0f, 1f, 1f, value(Id.AWB_LOCK, 0f),
                  if (awbMode() == CameraMetadata.CONTROL_AWB_MODE_AUTO) F_USABLE else 0)
        val fx = effects()
        if (fx.isNotEmpty()) o.add(Id.EFFECT, K_CHOICE, G_COLOUR, "Effect", 0f, (fx.size - 1).toFloat(), 1f, choice(Id.EFFECT, fx.size).toFloat(), options = fx.map { it.name })

        // Image
        val sc = sceneModes()
        if (sc.size > 1) o.add(Id.SCENE, K_CHOICE, G_IMAGE, "Scene mode", 0f, (sc.size - 1).toFloat(), 1f, choice(Id.SCENE, sc.size).toFloat(), options = sc.map { it.name })
        val nr = processing(CameraCharacteristics.NOISE_REDUCTION_AVAILABLE_NOISE_REDUCTION_MODES, nrNames)
        if (nr.isNotEmpty()) o.add(Id.NOISE, K_CHOICE, G_IMAGE, "Noise reduction", 0f, (nr.size - 1).toFloat(), 1f, choice(Id.NOISE, nr.size).toFloat(), options = nr.map { it.name })
        val ed = processing(CameraCharacteristics.EDGE_AVAILABLE_EDGE_MODES, edgeNames)
        if (ed.isNotEmpty()) o.add(Id.SHARPEN, K_CHOICE, G_IMAGE, "Sharpening", 0f, (ed.size - 1).toFloat(), 1f, choice(Id.SHARPEN, ed.size).toFloat(), options = ed.map { it.name })
        o.add(Id.RESET, K_BUTTON, G_IMAGE, "Reset this camera", 0f, 0f, 0f, 0f)

        NativeLink.cameraControls(o.bytes())
    }

    // ---- what Hydra set ----------------------------------------------------------------------------------------
    private fun applySettings(data: ByteArray) {
        val b = ByteBuffer.wrap(data).order(ByteOrder.LITTLE_ENDIAN)
        if (!b.hasRemaining()) return
        val n = b.get().toInt() and 0xFF
        var reopen = false
        var focusNow = false
        for (i in 0 until n) {
            if (b.remaining() < 2) break
            val id = b.get().toInt() and 0xFF
            val count = b.get().toInt() and 0xFF
            if (b.remaining() < 4 * count) break
            val v = FloatArray(count) { b.float }
            if (v.isEmpty()) continue
            val x = v[0]
            when (id) {
                Id.CAMERA -> cams.getOrNull(x.roundToInt())?.let {
                    if (it.key != cam?.key) { prefs()?.edit()?.putString("camId", it.key)?.putBoolean("camFront", it.front)?.apply(); reopen = true }
                }
                Id.BITRATE -> mbpsChoices.getOrNull(x.roundToInt())?.let {
                    mbps = it
                    prefs()?.edit()?.putInt("camMbps", it)?.apply()
                    applyBitrate()
                }
                Id.QUALITY -> qualityChoices().getOrNull(x.roundToInt())?.let {
                    prefs()?.edit()?.putInt("camHeight", it)?.putBoolean("camHd", it >= 1080)?.apply(); reopen = true
                }
                Id.FPS -> fpsChoices().getOrNull(x.roundToInt())?.let { if (it != fps) { prefs()?.edit()?.putInt("camFps", it)?.apply(); reopen = true } }
                Id.RESET -> if (presses[id] != x) {
                    presses[id] = x
                    prefs()?.edit()?.apply { for (k in vals.keys) remove(key(k)) }?.apply()
                    vals.clear()
                    region = null
                }
                Id.FOCUS_AGAIN -> if (presses[id] != x) { presses[id] = x; focusNow = true }
                Id.FOCUS_POINT -> if (v.size >= 3 && presses[id] != v[2]) {
                    presses[id] = v[2]
                    region = meteringAt(v[0], v[1])
                    focusNow = true
                }
                Id.AE_MODE -> {
                    // Manual starts where the automatic was (no jump in brightness).
                    if (x >= 0.5f && !aeManual()) {
                        if (liveIso > 0) store(Id.ISO, liveIso.toFloat())
                        if (liveExposureNs > 0) store(Id.SHUTTER, minOf(liveExposureNs / 1e9f, shutterMax()))
                    }
                    store(id, x)
                }
                Id.AF_MODE -> {
                    val modes = afModes()
                    if (modes.getOrNull(x.roundToInt())?.value == CameraMetadata.CONTROL_AF_MODE_OFF && afMode() != CameraMetadata.CONTROL_AF_MODE_OFF)
                        store(Id.FOCUS_DIST, liveFocus)
                    store(id, x)
                }
                else -> store(id, x)
            }
        }
        if (reopen) { settingsVersion++; loadCameras(); if (camera != null || opening) close() }
        else updateRequest(focusNow)
        describe()   // also confirms the settings to Hydra
    }

    /** A point of the upright picture (0..1) -> a metering rectangle on the sensor. */
    private fun meteringAt(u: Float, v: Float): MeteringRectangle? {
        val ch = chars() ?: return null
        val active = ch.get(CameraCharacteristics.SENSOR_INFO_ACTIVE_ARRAY_SIZE) ?: return null
        // Back from upright to the picture as sent (rotation quarter turns clockwise, as fitNv12 in PhoneCamera.cpp).
        val (sx, sy) = when (sentRotation and 3) {
            1 -> v to 1f - u
            2 -> 1f - u to 1f - v
            3 -> 1f - v to u
            else -> u to v
        }
        // The part of the sensor the stream shows: the zoomed field (the whole array with a zoom ratio, API 30+),
        // cut to the stream's aspect ratio in the middle.
        val field = if (Build.VERSION.SDK_INT >= 30) Rect(0, 0, active.width(), active.height()) else cropRegion(active)
        val aspect = if (size.height > 0) size.width.toFloat() / size.height else 16f / 9f
        var w = field.width().toFloat(); var h = field.height().toFloat()
        if (w / h > aspect) w = h * aspect else h = w / aspect
        val left = field.centerX() - w / 2; val top = field.centerY() - h / 2
        val cx = (left + sx * w).roundToInt(); val cy = (top + sy * h).roundToInt()
        val half = (w / 16).roundToInt().coerceAtLeast(16)
        val r = Rect(cx - half, cy - half, cx + half, cy + half)
        r.intersect(0, 0, active.width() - 1, active.height() - 1)
        return MeteringRectangle(r, MeteringRectangle.METERING_WEIGHT_MAX - 1)
    }

    /** Zoom without CONTROL_ZOOM_RATIO (before Android 11): the middle of the sensor. */
    private fun cropRegion(active: Rect): Rect {
        val z = value(Id.ZOOM, 1f).coerceIn(zoomRange().lower, zoomRange().upper).coerceAtLeast(1f)
        val w = (active.width() / z).roundToInt(); val h = (active.height() / z).roundToInt()
        val l = (active.width() - w) / 2; val t = (active.height() - h) / 2
        return Rect(l, t, l + w, t + h)
    }

    /** Colour temperature -> white balance gains (black body, Tanner Helland's fit; an approximation). */
    private fun gainsFor(kelvin: Float): RggbChannelVector {
        val t = kelvin / 100.0
        val r = if (t <= 66) 255.0 else 329.698727446 * (t - 60).pow(-0.1332047592)
        val g = if (t <= 66) 99.4708025861 * ln(t) - 161.1195681661 else 288.1221695283 * (t - 60).pow(-0.0755148492)
        val b = if (t >= 66) 255.0 else if (t <= 19) 1.0 else 138.5177312231 * ln(t - 10) - 305.0447927307
        val rr = r.coerceIn(1.0, 255.0); val gg = g.coerceIn(1.0, 255.0); val bb = b.coerceIn(1.0, 255.0)
        return RggbChannelVector((gg / rr).toFloat(), 1f, 1f, (gg / bb).toFloat())
    }

    private fun fpsRange(): Range<Int> =
        chars()?.get(CameraCharacteristics.CONTROL_AE_AVAILABLE_TARGET_FPS_RANGES)?.filter { it.upper == fps }?.maxByOrNull { it.lower } ?: Range(fps, fps)

    /** The repeating request with all settings. */
    private fun buildRequest(device: CameraDevice, target: Surface): CaptureRequest.Builder {
        val ch = chars()
        val r = device.createCaptureRequest(CameraDevice.TEMPLATE_RECORD)
        r.addTarget(target)
        r.set(CaptureRequest.CONTROL_AE_TARGET_FPS_RANGE, fpsRange())
        if (ch == null) return r
        val sc = sceneModes()
        val scene = sc[choice(Id.SCENE, sc.size)].value
        if (scene != CameraMetadata.CONTROL_SCENE_MODE_DISABLED) {
            r.set(CaptureRequest.CONTROL_MODE, CameraMetadata.CONTROL_MODE_USE_SCENE_MODE)
            r.set(CaptureRequest.CONTROL_SCENE_MODE, scene)
        } else r.set(CaptureRequest.CONTROL_MODE, CameraMetadata.CONTROL_MODE_AUTO)
        effects().let { if (it.isNotEmpty()) r.set(CaptureRequest.CONTROL_EFFECT_MODE, it[choice(Id.EFFECT, it.size)].value) }

        val z = zoomRange()
        if (z.upper > z.lower) {
            val zv = value(Id.ZOOM, 1f).coerceIn(z.lower, z.upper)
            if (Build.VERSION.SDK_INT >= 30) r.set(CaptureRequest.CONTROL_ZOOM_RATIO, zv)
            else ch.get(CameraCharacteristics.SENSOR_INFO_ACTIVE_ARRAY_SIZE)?.let { r.set(CaptureRequest.SCALER_CROP_REGION, cropRegion(it)) }
        }
        val st = stabilization()
        val s = st[choice(Id.STABILIZE, st.size, stabDefault().toInt())]
        r.set(CaptureRequest.CONTROL_VIDEO_STABILIZATION_MODE, if (s.eis) CameraMetadata.CONTROL_VIDEO_STABILIZATION_MODE_ON else CameraMetadata.CONTROL_VIDEO_STABILIZATION_MODE_OFF)
        if (st.any { it.ois }) r.set(CaptureRequest.LENS_OPTICAL_STABILIZATION_MODE, if (s.ois) CameraMetadata.LENS_OPTICAL_STABILIZATION_MODE_ON else CameraMetadata.LENS_OPTICAL_STABILIZATION_MODE_OFF)

        // Exposure
        if (aeManual()) {
            r.set(CaptureRequest.CONTROL_AE_MODE, CameraMetadata.CONTROL_AE_MODE_OFF)
            val iso = ch.get(CameraCharacteristics.SENSOR_INFO_SENSITIVITY_RANGE)!!
            r.set(CaptureRequest.SENSOR_SENSITIVITY, value(Id.ISO, 400f).roundToInt().coerceIn(iso.lower, iso.upper))
            val er = ch.get(CameraCharacteristics.SENSOR_INFO_EXPOSURE_TIME_RANGE)!!
            r.set(CaptureRequest.SENSOR_EXPOSURE_TIME, (value(Id.SHUTTER, 1f / 60) * 1e9).toLong().coerceIn(er.lower, maxOf(er.lower, (shutterMax() * 1e9).toLong())))
            r.set(CaptureRequest.SENSOR_FRAME_DURATION, 1_000_000_000L / fps)
        } else {
            r.set(CaptureRequest.CONTROL_AE_MODE, CameraMetadata.CONTROL_AE_MODE_ON)
            val step = evStep()
            if (step > 0f) {
                val range = ch.get(CameraCharacteristics.CONTROL_AE_COMPENSATION_RANGE)
                val idx = (value(Id.EV, 0f) / step).roundToInt()
                r.set(CaptureRequest.CONTROL_AE_EXPOSURE_COMPENSATION, if (range != null) idx.coerceIn(range.lower, range.upper) else idx)
            }
            if (ch.get(CameraCharacteristics.CONTROL_AE_LOCK_AVAILABLE) == true) r.set(CaptureRequest.CONTROL_AE_LOCK, value(Id.AE_LOCK, 0f) >= 0.5f)
        }
        if (ch.get(CameraCharacteristics.FLASH_INFO_AVAILABLE) == true)
            r.set(CaptureRequest.FLASH_MODE, if (value(Id.TORCH, 0f) >= 0.5f) CameraMetadata.FLASH_MODE_TORCH else CameraMetadata.FLASH_MODE_OFF)
        antibanding().let { if (it.size > 1) r.set(CaptureRequest.CONTROL_AE_ANTIBANDING_MODE, it[choice(Id.ANTIBANDING, it.size)].value) }
        region?.let { reg ->
            if ((ch.get(CameraCharacteristics.CONTROL_MAX_REGIONS_AE) ?: 0) > 0) r.set(CaptureRequest.CONTROL_AE_REGIONS, arrayOf(reg))
        }

        // Focus
        val af = afMode()
        if (af >= 0) {
            r.set(CaptureRequest.CONTROL_AF_MODE, af)
            if (af == CameraMetadata.CONTROL_AF_MODE_OFF) r.set(CaptureRequest.LENS_FOCUS_DISTANCE, value(Id.FOCUS_DIST, 0f).coerceIn(0f, minFocus()))
            else region?.let { reg -> if ((ch.get(CameraCharacteristics.CONTROL_MAX_REGIONS_AF) ?: 0) > 0) r.set(CaptureRequest.CONTROL_AF_REGIONS, arrayOf(reg)) }
        }

        // Colour
        val awb = awbMode()
        if (awb == CameraMetadata.CONTROL_AWB_MODE_OFF) {
            r.set(CaptureRequest.CONTROL_AWB_MODE, CameraMetadata.CONTROL_AWB_MODE_OFF)
            r.set(CaptureRequest.COLOR_CORRECTION_MODE, CameraMetadata.COLOR_CORRECTION_MODE_TRANSFORM_MATRIX)
            r.set(CaptureRequest.COLOR_CORRECTION_GAINS, gainsFor(value(Id.KELVIN, 5500f)))
            // The colour matrix the automatic used last (identity would wash the colours out).
            r.set(CaptureRequest.COLOR_CORRECTION_TRANSFORM, liveCcm ?: ColorSpaceTransform(intArrayOf(1, 1, 0, 1, 0, 1, 0, 1, 1, 1, 0, 1, 0, 1, 0, 1, 1, 1)))
        } else {
            r.set(CaptureRequest.CONTROL_AWB_MODE, awb)
            if (ch.get(CameraCharacteristics.CONTROL_AWB_LOCK_AVAILABLE) == true)
                r.set(CaptureRequest.CONTROL_AWB_LOCK, awb == CameraMetadata.CONTROL_AWB_MODE_AUTO && value(Id.AWB_LOCK, 0f) >= 0.5f)
        }
        processing(CameraCharacteristics.NOISE_REDUCTION_AVAILABLE_NOISE_REDUCTION_MODES, nrNames).let {
            val m = if (it.isEmpty()) -1 else it[choice(Id.NOISE, it.size)].value
            if (m >= 0) r.set(CaptureRequest.NOISE_REDUCTION_MODE, m)
        }
        processing(CameraCharacteristics.EDGE_AVAILABLE_EDGE_MODES, edgeNames).let {
            val m = if (it.isEmpty()) -1 else it[choice(Id.SHARPEN, it.size)].value
            if (m >= 0) r.set(CaptureRequest.EDGE_MODE, m)
        }
        return r
    }

    /** What the camera measures (ISO, shutter, focus, colour matrix of the automatic): shown in Hydra while on auto. */
    private val results = object : CameraCaptureSession.CaptureCallback() {
        override fun onCaptureCompleted(s: CameraCaptureSession, request: CaptureRequest, result: TotalCaptureResult) {
            result.get(CaptureResult.SENSOR_SENSITIVITY)?.let { liveIso = it }
            result.get(CaptureResult.SENSOR_EXPOSURE_TIME)?.let { liveExposureNs = it }
            result.get(CaptureResult.LENS_FOCUS_DISTANCE)?.let { liveFocus = it }
            if (result.get(CaptureResult.CONTROL_AWB_MODE) != CameraMetadata.CONTROL_AWB_MODE_OFF)
                result.get(CaptureResult.COLOR_CORRECTION_TRANSFORM)?.let { liveCcm = it }
        }
    }

    /** New settings on the running session; focusNow: trigger the autofocus once (tap / "Focus again"). */
    private fun updateRequest(focusNow: Boolean) {
        val s = session ?: return
        val d = camera ?: return
        val target = codecSurface ?: return
        try {
            val r = buildRequest(d, target)
            if (focusNow && afMode() >= 0 && afMode() != CameraMetadata.CONTROL_AF_MODE_OFF) {
                // Continuous: a cancel restarts the search (at the new point); single: start it.
                val trigger = if (afMode() == CameraMetadata.CONTROL_AF_MODE_AUTO) CameraMetadata.CONTROL_AF_TRIGGER_START else CameraMetadata.CONTROL_AF_TRIGGER_CANCEL
                r.set(CaptureRequest.CONTROL_AF_TRIGGER, CameraMetadata.CONTROL_AF_TRIGGER_CANCEL)
                s.capture(r.build(), null, handler)
                if (trigger == CameraMetadata.CONTROL_AF_TRIGGER_START) {
                    r.set(CaptureRequest.CONTROL_AF_TRIGGER, trigger)
                    s.capture(r.build(), null, handler)
                }
                r.set(CaptureRequest.CONTROL_AF_TRIGGER, CameraMetadata.CONTROL_AF_TRIGGER_IDLE)
            }
            s.setRepeatingRequest(r.build(), results, handler)
        } catch (e: Exception) {
            lastError = "Camera setting refused: " + (e.message ?: e.toString())
            Log.w(TAG, "request", e)
        }
    }

    // ---- camera + encoder ------------------------------------------------------------------------------------
    @SuppressLint("MissingPermission")   // checked by the app before switching the camera on
    private fun open(appHeight: Int) {
        val ctx = context ?: return
        val cm = ctx.getSystemService(CameraManager::class.java) ?: return
        if (cam == null) loadCameras()
        val c = cam ?: run { lastError = "No camera"; return }
        // As much as the settings allow, but not more than the app on the PC shows (saves Wi-Fi and battery).
        val wantHeight = camHeight()
        val height = if (appHeight > 0) minOf(wantHeight, maxOf(720, appHeight)) else wantHeight
        try {
            val ch = c.lens
            facingFront = c.front
            sensorDegrees = ch.get(CameraCharacteristics.SENSOR_ORIENTATION) ?: 90
            if (fps !in fpsChoices()) fps = if (30 in fpsChoices()) 30 else fpsChoices().last()
            val map = ch.get(CameraCharacteristics.SCALER_STREAM_CONFIGURATION_MAP) ?: c.ch.get(CameraCharacteristics.SCALER_STREAM_CONFIGURATION_MAP)
                ?: run { lastError = "Camera without video sizes"; return }
            val sizes = map.getOutputSizes(MediaCodec::class.java) ?: emptyArray()
            // 16:9 at the wanted height, else the largest 16:9 below it, else the closest size.
            val wide = sizes.filter { it.width * 9 == it.height * 16 }
            val pick = wide.firstOrNull { it.height == height } ?: wide.filter { it.height <= height }.maxByOrNull { it.height }
                ?: sizes.minByOrNull { abs(it.height - height) } ?: Size(1280, 720)
            size = pick

            startEncoder(pick)
            opening = true
            cm.openCamera(c.id, object : CameraDevice.StateCallback() {
                override fun onOpened(device: CameraDevice) {
                    opening = false
                    if (!enabled || codecSurface == null) { device.close(); return }
                    camera = device
                    startSession(device)
                }
                override fun onDisconnected(device: CameraDevice) { opening = false; device.close(); if (camera == device) camera = null; close() }
                override fun onError(device: CameraDevice, error: Int) {
                    opening = false
                    lastError = "Camera error $error (another app may use it)"
                    device.close()
                    if (camera == device) camera = null
                    close()
                }
            }, handler)
        } catch (e: Exception) {
            opening = false
            lastError = e.message ?: e.toString()
            Log.w(TAG, "open", e)
            close()
        }
    }

    @Suppress("DEPRECATION")   // createCaptureSession (list): the simple form, available since API 21
    private fun startSession(device: CameraDevice) {
        val target = codecSurface ?: return
        try {
            val callback = object : CameraCaptureSession.StateCallback() {
                override fun onConfigured(s: CameraCaptureSession) {
                    if (camera != device) { s.close(); return }
                    session = s
                    try {
                        s.setRepeatingRequest(buildRequest(device, target).build(), results, handler)
                    } catch (e: Exception) {
                        // A setting the camera does not take: start with its defaults, the error shows on the phone.
                        lastError = "Camera setting refused: " + (e.message ?: e.toString())
                        val r = device.createCaptureRequest(CameraDevice.TEMPLATE_RECORD).apply { addTarget(target); set(CaptureRequest.CONTROL_AE_TARGET_FPS_RANGE, fpsRange()) }
                        s.setRepeatingRequest(r.build(), results, handler)
                    }
                    sending = true
                    requestKeyFrame()
                    describe()
                }
                override fun onConfigureFailed(s: CameraCaptureSession) { lastError = "The camera refused the video size / frame rate"; close() }
            }
            val phys = cam?.physical
            if (phys != null && Build.VERSION.SDK_INT >= 28) {
                // One lens of a logical camera: its stream alone.
                val oc = android.hardware.camera2.params.OutputConfiguration(target).apply { setPhysicalCameraId(phys) }
                device.createCaptureSessionByOutputConfigurations(listOf(oc), callback, handler)
            } else device.createCaptureSession(listOf(target), callback, handler)
        } catch (e: Exception) {
            lastError = e.message ?: e.toString()
            close()
        }
    }

    private fun startEncoder(sz: Size) {
        encHeight = sz.height
        val bitrate = bitrate(sz.height)
        val usb = overUsb
        fun format(withProfile: Boolean) = MediaFormat.createVideoFormat(MediaFormat.MIMETYPE_VIDEO_AVC, sz.width, sz.height).apply {
            setInteger(MediaFormat.KEY_COLOR_FORMAT, MediaCodecInfo.CodecCapabilities.COLOR_FormatSurface)
            setInteger(MediaFormat.KEY_BIT_RATE, bitrate)
            setInteger(MediaFormat.KEY_FRAME_RATE, fps)
            setInteger(MediaFormat.KEY_I_FRAME_INTERVAL, 2)   // + a key frame whenever Hydra lost something
            setInteger(MediaFormat.KEY_PRIORITY, 0)            // realtime
            if (withProfile) {
                // Over USB: High profile (clearly better picture per bit; still no B frames = no added delay).
                setInteger(MediaFormat.KEY_PROFILE, if (usb) MediaCodecInfo.CodecProfileLevel.AVCProfileHigh else MediaCodecInfo.CodecProfileLevel.AVCProfileBaseline)
                setInteger(MediaFormat.KEY_LEVEL, when {
                    sz.height >= 2160 -> if (fps > 30) MediaCodecInfo.CodecProfileLevel.AVCLevel52 else MediaCodecInfo.CodecProfileLevel.AVCLevel51
                    sz.height >= 1440 -> if (fps > 30) MediaCodecInfo.CodecProfileLevel.AVCLevel51 else MediaCodecInfo.CodecProfileLevel.AVCLevel5
                    usb && bitrate > 25_000_000 -> MediaCodecInfo.CodecProfileLevel.AVCLevel51
                    sz.height >= 1080 || fps > 30 -> MediaCodecInfo.CodecProfileLevel.AVCLevel42
                    else -> MediaCodecInfo.CodecProfileLevel.AVCLevel31 })
                if (usb) setInteger(MediaFormat.KEY_BITRATE_MODE, MediaCodecInfo.EncoderCapabilities.BITRATE_MODE_VBR)
            }
            if (Build.VERSION.SDK_INT >= 29) setInteger(MediaFormat.KEY_MAX_B_FRAMES, 0)
            if (Build.VERSION.SDK_INT >= 30) setInteger(MediaFormat.KEY_LATENCY, 1)
        }
        // With profile / level hints first; some encoders refuse them, then without.
        val c = try { makeEncoder(sz, format(true)) } catch (e: Exception) { Log.w(TAG, "encoder with profile", e); makeEncoder(sz, format(false)) }
        codecSurface = c.createInputSurface()
        c.start()
        codec = c
    }

    private var encHeight = 720

    /** Automatic bit rate: Wi-Fi 4 / 8 Mbit/s (720p / 1080p), over the USB cable 15 / 30; 60 fps 1.5x. */
    private fun autoBitrate(height: Int): Int {
        val base = if (overUsb) when { height >= 2160 -> 60_000_000; height >= 1440 -> 45_000_000; height >= 1080 -> 30_000_000; else -> 15_000_000 }
                   else when { height >= 2160 -> 20_000_000; height >= 1440 -> 12_000_000; height >= 1080 -> 8_000_000; height >= 720 -> 4_000_000; else -> 2_000_000 }
        return minOf(80_000_000, if (fps > 40) base * 3 / 2 else base)
    }
    private fun bitrate(height: Int) = if (mbps > 0) mbps * 1_000_000 else autoBitrate(height)

    /** A new bit rate on the running encoder (no new key frame, no pause). */
    private fun applyBitrate() {
        try { codec?.setParameters(Bundle().apply { putInt(MediaCodec.PARAMETER_KEY_VIDEO_BITRATE, bitrate(encHeight)) }) }
        catch (e: Exception) { Log.w(TAG, "bit rate", e) }
    }

    private fun makeEncoder(sz: Size, f: MediaFormat): MediaCodec {
        val c = MediaCodec.createEncoderByType(MediaFormat.MIMETYPE_VIDEO_AVC)
        c.setCallback(object : MediaCodec.Callback() {
            override fun onInputBufferAvailable(codec: MediaCodec, index: Int) {}   // input is the surface
            override fun onOutputBufferAvailable(codec: MediaCodec, index: Int, info: MediaCodec.BufferInfo) {
                try {
                    val buf = codec.getOutputBuffer(index)
                    if (buf != null && info.size > 0) {
                        val config = (info.flags and MediaCodec.BUFFER_FLAG_CODEC_CONFIG) != 0
                        val key = (info.flags and MediaCodec.BUFFER_FLAG_KEY_FRAME) != 0
                        val rot = rotation()
                        if (!config) sentRotation = rot
                        NativeLink.pushVideo(buf, info.offset, info.size, key, config, sz.width, sz.height, rot, info.presentationTimeUs)
                    }
                    codec.releaseOutputBuffer(index, false)
                } catch (e: Exception) { Log.w(TAG, "output", e) }
            }
            override fun onOutputFormatChanged(codec: MediaCodec, format: MediaFormat) {
                // Some encoders give SPS / PPS only here (csd-0 / csd-1): Hydra needs them in front of key frames.
                val parts = listOfNotNull(format.getByteBuffer("csd-0"), format.getByteBuffer("csd-1"))
                if (parts.isEmpty()) return
                val all = java.nio.ByteBuffer.allocateDirect(parts.sumOf { it.remaining() })
                for (p in parts) all.put(p.duplicate())
                NativeLink.pushVideo(all, 0, all.capacity(), false, true, sz.width, sz.height, 0, 0)
            }
            override fun onError(codec: MediaCodec, e: MediaCodec.CodecException) {
                lastError = "Video encoder: " + (e.diagnosticInfo ?: e.toString())
                handler?.post { close() }
            }
        }, handler)
        try {
            c.configure(f, null, null, MediaCodec.CONFIGURE_FLAG_ENCODE)
        } catch (e: Exception) {
            c.release()
            throw e
        }
        return c
    }

    private fun close() {
        sending = false
        opening = false
        notWantedSince = 0L
        try { session?.close() } catch (_: Exception) {}
        session = null
        try { camera?.close() } catch (_: Exception) {}
        camera = null
        try { codec?.stop() } catch (_: Exception) {}
        try { codec?.release() } catch (_: Exception) {}
        codec = null
        codecSurface?.release()
        codecSurface = null
    }
}
