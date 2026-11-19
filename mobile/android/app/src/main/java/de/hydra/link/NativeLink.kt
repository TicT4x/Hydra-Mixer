package de.hydra.link

/** The native Hydra Link endpoint (app/src/main/cpp/jni.cpp): one per process. */
object NativeLink {
    init { System.loadLibrary("hydralink") }

    const val PORT = 6980

    /** Codec ids as in the packet format (net::Codec). */
    const val CODEC_PCM24 = 2
    const val CODEC_OPUS = 4

    /** AAudio input presets. */
    const val PRESET_VOICE_COMMUNICATION = 7   // echo cancellation, noise suppression
    const val PRESET_VOICE_PERFORMANCE = 10    // no processing, lowest latency

    /** Pairing states of a peer ("pair" in the state), as lite::PairState. */
    const val PAIR_NONE = 0
    const val PAIR_ASKING = 1
    const val PAIR_ALLOWED = 2
    const val PAIR_DENIED = 3
    const val PAIR_NO_ANSWER = 4

    /** Starts the endpoint (announces the phone). deviceId: 16 hex digits; trusted: "id|name" per line.
        Returns an error text or "". */
    @JvmStatic external fun start(name: String, port: Int, deviceId: String, trusted: String): String
    @JvmStatic external fun stop()
    /** "*" = whatever this sender sends to the phone; empty stream = stop listening. */
    @JvmStatic external fun listen(stream: String, sender: String): String
    @JvmStatic external fun send(host: String, port: Int, stream: String, codec: Int, kbps: Int, channels: Int, preset: Int): String
    @JvmStatic external fun stopSending()
    @JvmStatic external fun setGains(play: Float, mic: Float)
    @JvmStatic external fun setMuted(play: Boolean, mic: Boolean)
    /** Pairing: ask a Hydra by name / answer a Hydra that asks / unpair. */
    @JvmStatic external fun connect(hydra: String)
    @JvmStatic external fun answer(id: String, allow: Boolean)
    @JvmStatic external fun forget(id: String)
    /** Camera as Hydra's webcam: offer it to a Hydra (by name). The encoder runs only while videoWish() says so. */
    @JvmStatic external fun startVideo(host: String, port: Int, width: Int, height: Int, fps: Int): String
    @JvmStatic external fun stopVideo()
    /** Bit 0 = Hydra wants pictures, bit 1 = send a key frame now, bit 2 = over the USB cable, bits 8.. = the height the app chose. */
    @JvmStatic external fun videoWish(): Int
    /** One encoder output buffer (MediaCodec's direct ByteBuffer). rotation: quarter turns clockwise to upright. */
    @JvmStatic external fun pushVideo(buffer: java.nio.ByteBuffer, offset: Int, size: Int, key: Boolean, config: Boolean,
                                      width: Int, height: Int, rotation: Int, timeUs: Long)
    /** The camera's settings for Hydra (control list as in engine/NetLink.cpp, CamCaps). Call it after applying
        cameraSettings(), even when nothing changed: that confirms them to Hydra. */
    @JvmStatic external fun cameraControls(list: ByteArray)
    /** What Hydra changed since the last call (u8 count, per entry u8 id, u8 values, f32 each, little endian) or null. */
    @JvmStatic external fun cameraSettings(): ByteArray?
    /** JSON: running, name, trustVersion, peers[], trusted[], requests[], streams[], rx{}, tx{}, video{}. */
    @JvmStatic external fun state(): String
}
