package dev.soshroom.phonecam

import android.content.Context

data class CameraConfig(
    val cameraId: String = "",
    val width: Int = 1920,
    val height: Int = 1080,
    val fps: Int = 15,
    val bitrate: Int = 5_000_000,
    val zoom: Float = 1.0f,
    val httpPort: Int = 8080,
    val streamPort: Int = 8554,
) {
    fun save(context: Context) {
        context.getSharedPreferences(PREFS, Context.MODE_PRIVATE).edit()
            .putString("cameraId", cameraId)
            .putInt("width", width)
            .putInt("height", height)
            .putInt("fps", fps)
            .putInt("bitrate", bitrate)
            .putFloat("zoom", zoom)
            .putInt("httpPort", httpPort)
            .putInt("streamPort", streamPort)
            .apply()
    }

    companion object {
        private const val PREFS = "phonecam"

        fun load(context: Context): CameraConfig {
            val p = context.getSharedPreferences(PREFS, Context.MODE_PRIVATE)
            return CameraConfig(
                cameraId = p.getString("cameraId", "") ?: "",
                width = p.getInt("width", 1920),
                height = p.getInt("height", 1080),
                fps = p.getInt("fps", 15),
                bitrate = p.getInt("bitrate", 5_000_000),
                zoom = p.getFloat("zoom", 1.0f),
                httpPort = p.getInt("httpPort", 8080),
                streamPort = p.getInt("streamPort", 8554),
            )
        }
    }
}
