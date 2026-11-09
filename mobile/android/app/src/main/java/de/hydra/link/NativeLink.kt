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

    /** Starts the endpoint (announces the phone). Returns an error text or "". */
    @JvmStatic external fun start(name: String, port: Int): String
    @JvmStatic external fun stop()
    /** Empty stream = stop listening. */
    @JvmStatic external fun listen(stream: String, sender: String): String
    @JvmStatic external fun send(host: String, port: Int, stream: String, codec: Int, kbps: Int, channels: Int, preset: Int): String
    @JvmStatic external fun stopSending()
    @JvmStatic external fun setGains(play: Float, mic: Float)
    /** JSON: running, name, peers[], streams[], rx{}, tx{}. */
    @JvmStatic external fun state(): String
}
