package dev.soshroom.phonecam

import android.Manifest
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.Service
import android.content.Context
import android.content.Intent
import android.content.pm.PackageManager
import android.graphics.ImageFormat
import android.graphics.Rect
import android.hardware.camera2.*
import android.media.ImageReader
import android.media.MediaCodec
import android.media.MediaCodecInfo
import android.media.MediaFormat
import android.os.*
import android.util.Range
import android.util.Size
import android.view.Surface
import androidx.core.app.NotificationCompat
import androidx.core.content.ContextCompat
import java.util.concurrent.atomic.AtomicBoolean
import java.util.concurrent.atomic.AtomicReference

class CameraService : Service() {
    private lateinit var manager: CameraManager
    private lateinit var cameraThread: HandlerThread
    private lateinit var cameraHandler: Handler
    private var camera: CameraDevice? = null
    private var session: CameraCaptureSession? = null
    private var encoder: MediaCodec? = null
    private var encoderSurface: Surface? = null
    private var encoderThread: Thread? = null
    private var imageReader: ImageReader? = null
    private var h264Server: H264Server? = null
    private var controlServer: ControlServer? = null
    private var wakeLock: PowerManager.WakeLock? = null
    private var currentConfig: CameraConfig? = null
    private val latestJpeg = AtomicReference<ByteArray?>(null)
    private val draining = AtomicBoolean(false)
    private var repeatingBuilder: CaptureRequest.Builder? = null
    private var sensorRect: Rect? = null
    private var startedAt = 0L
    @Volatile private var codecConfig: List<ByteArray> = emptyList()
    @Volatile private var selectedFpsRange = "unknown"
    @Volatile private var encoderFps = 0.0
    private var encoderFrameCount = 0L
    private var encoderFpsStartedAt = 0L

    override fun onCreate() {
        super.onCreate()
        manager = getSystemService(Context.CAMERA_SERVICE) as CameraManager
        cameraThread = HandlerThread("phonecam-camera").also { it.start() }
        cameraHandler = Handler(cameraThread.looper)
        createNotificationChannel()
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        when (intent?.action) {
            ACTION_STOP -> stopEverything()
            ACTION_APPLY -> applyConfig(CameraConfig.load(this))
            else -> startEverything()
        }
        return START_STICKY
    }

    override fun onBind(intent: Intent?) = null

    private fun startEverything() {
        if (running) return
        val cfg = CameraConfig.load(this)
        startForeground(
            NOTIFICATION_ID,
            NotificationCompat.Builder(this, CHANNEL_ID)
                .setSmallIcon(android.R.drawable.presence_video_online)
                .setContentTitle("PhoneCam")
                .setContentText("Camera streaming on LAN")
                .setOngoing(true)
                .build()
        )
        wakeLock = (getSystemService(Context.POWER_SERVICE) as PowerManager)
            .newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "PhoneCam::stream").apply { acquire() }

        h264Server = H264Server(cfg.streamPort) {
            runCatching {
                encoder?.setParameters(Bundle().apply {
                    putInt(MediaCodec.PARAMETER_KEY_REQUEST_SYNC_FRAME, 0)
                })
            }
        }.also { it.start() }
        startedAt = SystemClock.elapsedRealtime()
        controlServer = ControlServer(
            context = this,
            port = cfg.httpPort,
            snapshot = latestJpeg,
            clients = { h264Server?.clientCount() ?: 0 },
            uptime = { SystemClock.elapsedRealtime() - startedAt },
            encoderFps = { encoderFps },
            fpsRange = { selectedFpsRange },
            applySettings = { applyConfig(it) },
            stopCamera = { stopEverything() },
        ).also { it.start(5000, false) }

        running = true
        restartCamera(cfg)
    }

    private fun applyConfig(next: CameraConfig) {
        if (!running) {
            startEverything()
            return
        }
        val old = currentConfig
        if (old == null || old.cameraId != next.cameraId || old.width != next.width || old.height != next.height || old.fps != next.fps) {
            restartCamera(next)
            return
        }
        currentConfig = next
        if (old.bitrate != next.bitrate) {
            runCatching {
                encoder?.setParameters(Bundle().apply {
                    putInt(MediaCodec.PARAMETER_KEY_VIDEO_BITRATE, next.bitrate)
                })
            }
        }
        rebuildRepeating(next)
    }

    private fun restartCamera(cfg: CameraConfig) {
        closeCameraPipeline()
        if (ContextCompat.checkSelfPermission(this, Manifest.permission.CAMERA) != PackageManager.PERMISSION_GRANTED) return
        val cameraId = chooseCamera(cfg.cameraId)
        currentConfig = cfg.copy(cameraId = cameraId).also { it.save(this) }
        val characteristics = manager.getCameraCharacteristics(cameraId)
        sensorRect = characteristics.get(CameraCharacteristics.SENSOR_INFO_ACTIVE_ARRAY_SIZE)
        setupEncoder(currentConfig!!)
        setupPreviewReader(currentConfig!!)

        manager.openCamera(cameraId, object : CameraDevice.StateCallback() {
            override fun onOpened(device: CameraDevice) {
                camera = device
                createSession(currentConfig!!)
            }
            override fun onDisconnected(device: CameraDevice) { device.close(); camera = null }
            override fun onError(device: CameraDevice, error: Int) { device.close(); camera = null }
        }, cameraHandler)
    }

    private fun setupEncoder(cfg: CameraConfig) {
        codecConfig = emptyList()
        h264Server?.setCodecConfig(emptyList())
        encoderFrameCount = 0
        encoderFps = 0.0
        encoderFpsStartedAt = SystemClock.elapsedRealtime()
        val format = MediaFormat.createVideoFormat(MediaFormat.MIMETYPE_VIDEO_AVC, cfg.width, cfg.height).apply {
            setInteger(MediaFormat.KEY_COLOR_FORMAT, MediaCodecInfo.CodecCapabilities.COLOR_FormatSurface)
            setInteger(MediaFormat.KEY_BIT_RATE, cfg.bitrate)
            setInteger(MediaFormat.KEY_FRAME_RATE, cfg.fps)
            setInteger(MediaFormat.KEY_I_FRAME_INTERVAL, 2)
            setInteger(MediaFormat.KEY_PREPEND_HEADER_TO_SYNC_FRAMES, 1)
            setInteger(MediaFormat.KEY_MAX_B_FRAMES, 0)
            setInteger(MediaFormat.KEY_BITRATE_MODE, MediaCodecInfo.EncoderCapabilities.BITRATE_MODE_CBR)
            setInteger(MediaFormat.KEY_PRIORITY, 0)
            setInteger(MediaFormat.KEY_OPERATING_RATE, cfg.fps)
        }
        encoder = MediaCodec.createEncoderByType(MediaFormat.MIMETYPE_VIDEO_AVC).also { codec ->
            codec.configure(format, null, null, MediaCodec.CONFIGURE_FLAG_ENCODE)
            encoderSurface = codec.createInputSurface()
            codec.start()
        }
        draining.set(true)
        encoderThread = Thread({ drainEncoder() }, "phonecam-encoder").also { it.start() }
    }

    private fun setupPreviewReader(cfg: CameraConfig) {
        val preview = if (cfg.width >= 1280) Size(640, 360) else Size(480, 270)
        imageReader = ImageReader.newInstance(preview.width, preview.height, ImageFormat.JPEG, 2).apply {
            setOnImageAvailableListener({ reader ->
                reader.acquireLatestImage()?.use { image ->
                    val buffer = image.planes[0].buffer
                    val bytes = ByteArray(buffer.remaining())
                    buffer.get(bytes)
                    latestJpeg.set(bytes)
                }
            }, cameraHandler)
        }
    }

    private fun createSession(cfg: CameraConfig) {
        val device = camera ?: return
        val videoSurface = encoderSurface ?: return
        val previewSurface = imageReader?.surface ?: return
        device.createCaptureSession(listOf(videoSurface, previewSurface), object : CameraCaptureSession.StateCallback() {
            override fun onConfigured(captureSession: CameraCaptureSession) {
                session = captureSession
                repeatingBuilder = device.createCaptureRequest(CameraDevice.TEMPLATE_RECORD).apply {
                    addTarget(videoSurface)
                    set(CaptureRequest.CONTROL_AF_MODE, CaptureRequest.CONTROL_AF_MODE_CONTINUOUS_VIDEO)
                    set(CaptureRequest.CONTROL_AE_MODE, CaptureRequest.CONTROL_AE_MODE_ON)
                }
                rebuildRepeating(cfg)
                schedulePreview(device, previewSurface)
            }
            override fun onConfigureFailed(captureSession: CameraCaptureSession) = Unit
        }, cameraHandler)
    }

    private fun chooseFpsRange(cameraId: String, requestedFps: Int): Range<Int> {
        val ranges = manager.getCameraCharacteristics(cameraId)
            .get(CameraCharacteristics.CONTROL_AE_AVAILABLE_TARGET_FPS_RANGES)
            ?.toList()
            .orEmpty()

        val exact = ranges.firstOrNull { it.lower == requestedFps && it.upper == requestedFps }
        if (exact != null) return exact

        return ranges
            .filter { requestedFps in it.lower..it.upper }
            .minWithOrNull(compareBy<Range<Int>>({ it.upper - it.lower }, { -it.lower }))
            ?: ranges.maxByOrNull { it.upper }
            ?: Range(requestedFps, requestedFps)
    }

    private fun rebuildRepeating(cfg: CameraConfig) {
        val builder = repeatingBuilder ?: return
        val captureSession = session ?: return
        val range = chooseFpsRange(currentConfig?.cameraId ?: cfg.cameraId, cfg.fps)
        selectedFpsRange = "${range.lower}-${range.upper}"
        builder.set(CaptureRequest.CONTROL_AE_TARGET_FPS_RANGE, range)
        sensorRect?.let { active ->
            val zoom = cfg.zoom.coerceAtLeast(1f)
            val w = (active.width() / zoom).toInt()
            val h = (active.height() / zoom).toInt()
            val left = active.centerX() - w / 2
            val top = active.centerY() - h / 2
            builder.set(CaptureRequest.SCALER_CROP_REGION, Rect(left, top, left + w, top + h))
        }
        runCatching { captureSession.setRepeatingRequest(builder.build(), null, cameraHandler) }
    }

    private fun schedulePreview(device: CameraDevice, previewSurface: Surface) {
        cameraHandler.post(object : Runnable {
            override fun run() {
                if (!running || session == null) return
                runCatching {
                    val request = device.createCaptureRequest(CameraDevice.TEMPLATE_STILL_CAPTURE).apply {
                        addTarget(previewSurface)
                        set(CaptureRequest.JPEG_QUALITY, 75.toByte())
                        set(CaptureRequest.CONTROL_AF_MODE, CaptureRequest.CONTROL_AF_MODE_CONTINUOUS_PICTURE)
                    }.build()
                    session?.capture(request, null, cameraHandler)
                }
                cameraHandler.postDelayed(this, 1000)
            }
        })
    }

    private fun updateCodecConfig(config: List<ByteArray>) {
        val nonEmpty = config.filter { it.isNotEmpty() }
        if (nonEmpty.isEmpty()) return
        codecConfig = nonEmpty.map { it.copyOf() }
        h264Server?.setCodecConfig(codecConfig)
    }

    private fun updateEncoderFps() {
        ++encoderFrameCount
        val now = SystemClock.elapsedRealtime()
        val elapsed = now - encoderFpsStartedAt
        if (elapsed >= 2000) {
            encoderFps = encoderFrameCount * 1000.0 / elapsed
            encoderFrameCount = 0
            encoderFpsStartedAt = now
        }
    }

    private fun drainEncoder() {
        val info = MediaCodec.BufferInfo()
        while (draining.get()) {
            val codec = encoder ?: break
            try {
                when (val index = codec.dequeueOutputBuffer(info, 10_000)) {
                    MediaCodec.INFO_OUTPUT_FORMAT_CHANGED -> {
                        val format = codec.outputFormat
                        val config = mutableListOf<ByteArray>()
                        listOf("csd-0", "csd-1").forEach { key ->
                            format.getByteBuffer(key)?.duplicate()?.let { csd ->
                                val bytes = ByteArray(csd.remaining())
                                csd.get(bytes)
                                if (bytes.isNotEmpty()) config += bytes
                            }
                        }
                        updateCodecConfig(config)
                        config.forEach { h264Server?.broadcast(it) }
                    }
                    else -> if (index >= 0) {
                        codec.getOutputBuffer(index)?.let { buffer ->
                            buffer.position(info.offset)
                            buffer.limit(info.offset + info.size)
                            val bytes = ByteArray(info.size)
                            buffer.get(bytes)

                            val isCodecConfig = (info.flags and MediaCodec.BUFFER_FLAG_CODEC_CONFIG) != 0
                            if (isCodecConfig && bytes.isNotEmpty()) {
                                updateCodecConfig(listOf(bytes))
                                h264Server?.broadcast(bytes)
                            } else {
                                if (bytes.isNotEmpty()) updateEncoderFps()
                                if ((info.flags and MediaCodec.BUFFER_FLAG_KEY_FRAME) != 0) {
                                    codecConfig.forEach { h264Server?.broadcast(it) }
                                }
                                if (bytes.isNotEmpty()) h264Server?.broadcast(bytes)
                            }
                        }
                        codec.releaseOutputBuffer(index, false)
                    }
                }
            } catch (_: Exception) {
                break
            }
        }
    }

    private fun chooseCamera(requested: String): String {
        if (requested.isNotBlank() && manager.cameraIdList.contains(requested)) return requested
        return manager.cameraIdList.firstOrNull { id ->
            manager.getCameraCharacteristics(id).get(CameraCharacteristics.LENS_FACING) == CameraCharacteristics.LENS_FACING_BACK
        } ?: manager.cameraIdList.first()
    }

    private fun closeCameraPipeline() {
        session?.close(); session = null
        camera?.close(); camera = null
        imageReader?.close(); imageReader = null
        draining.set(false)
        encoderThread?.interrupt(); encoderThread = null
        encoder?.let { runCatching { it.stop() }; runCatching { it.release() } }
        encoder = null
        encoderSurface?.release(); encoderSurface = null
        repeatingBuilder = null
        codecConfig = emptyList()
        h264Server?.setCodecConfig(emptyList())
        encoderFps = 0.0
        selectedFpsRange = "unknown"
    }

    private fun stopEverything() {
        running = false
        closeCameraPipeline()
        controlServer?.stop(); controlServer = null
        h264Server?.stop(); h264Server = null
        wakeLock?.let { if (it.isHeld) it.release() }; wakeLock = null
        stopForeground(STOP_FOREGROUND_REMOVE)
        stopSelf()
    }

    override fun onDestroy() {
        if (running) stopEverything()
        cameraThread.quitSafely()
        super.onDestroy()
    }

    private fun createNotificationChannel() {
        val notificationManager = getSystemService(NotificationManager::class.java)
        notificationManager.createNotificationChannel(
            NotificationChannel(CHANNEL_ID, "PhoneCam streaming", NotificationManager.IMPORTANCE_LOW)
        )
    }

    companion object {
        const val ACTION_START = "dev.soshroom.phonecam.START"
        const val ACTION_STOP = "dev.soshroom.phonecam.STOP"
        const val ACTION_APPLY = "dev.soshroom.phonecam.APPLY"
        @Volatile var running = false
        private const val CHANNEL_ID = "phonecam_stream"
        private const val NOTIFICATION_ID = 1
    }
}
