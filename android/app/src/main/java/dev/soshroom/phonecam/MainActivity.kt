package dev.soshroom.phonecam

import android.Manifest
import android.content.Context
import android.content.Intent
import android.content.pm.PackageManager
import android.hardware.camera2.CameraCharacteristics
import android.hardware.camera2.CameraManager
import android.os.Bundle
import android.widget.ArrayAdapter
import android.widget.Button
import android.widget.LinearLayout
import android.widget.SeekBar
import android.widget.Spinner
import android.widget.TextView
import androidx.activity.result.contract.ActivityResultContracts
import androidx.appcompat.app.AppCompatActivity
import androidx.core.content.ContextCompat
import java.net.Inet4Address
import java.net.NetworkInterface

class MainActivity : AppCompatActivity() {
    private lateinit var status: TextView
    private lateinit var cameraSpinner: Spinner
    private lateinit var resolutionSpinner: Spinner
    private lateinit var fpsSpinner: Spinner
    private lateinit var bitrateSpinner: Spinner
    private lateinit var zoomLabel: TextView
    private lateinit var zoom: SeekBar

    private val cameraPermission = registerForActivityResult(
        ActivityResultContracts.RequestPermission()
    ) { granted ->
        if (!granted) status.text = "Camera permission is required"
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        if (ContextCompat.checkSelfPermission(this, Manifest.permission.CAMERA) != PackageManager.PERMISSION_GRANTED) {
            cameraPermission.launch(Manifest.permission.CAMERA)
        }

        val padding = (20 * resources.displayMetrics.density).toInt()
        val root = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(padding, padding, padding, padding)
        }

        root.addView(TextView(this).apply {
            text = "PhoneCam ${BuildConfig.VERSION_NAME}"
            textSize = 24f
        })

        status = TextView(this)
        root.addView(status)

        val config = CameraConfig.load(this)
        val cameraIds = rearCameraIds()
        cameraSpinner = addSpinner(root, "Camera", if (cameraIds.isEmpty()) listOf("0") else cameraIds)
        resolutionSpinner = addSpinner(root, "Resolution", listOf("1280x720", "1920x1080", "2560x1440"))
        fpsSpinner = addSpinner(root, "FPS", listOf("5", "10", "15", "20", "24", "30"))
        bitrateSpinner = addSpinner(root, "Bitrate", listOf("2 Mbps", "3 Mbps", "5 Mbps", "8 Mbps", "12 Mbps"))

        zoomLabel = TextView(this)
        root.addView(zoomLabel)
        zoom = SeekBar(this).apply {
            max = 300
            progress = (((config.zoom.coerceIn(1f, 4f) - 1f) * 100f).toInt())
            setOnSeekBarChangeListener(object : SeekBar.OnSeekBarChangeListener {
                override fun onProgressChanged(seekBar: SeekBar?, progress: Int, fromUser: Boolean) {
                    zoomLabel.text = "Zoom: %.1fx".format(1f + progress / 100f)
                }
                override fun onStartTrackingTouch(seekBar: SeekBar?) = Unit
                override fun onStopTrackingTouch(seekBar: SeekBar?) = Unit
            })
        }
        root.addView(zoom)
        zoomLabel.text = "Zoom: %.1fx".format(config.zoom)

        select(cameraSpinner, if (config.cameraId.isBlank()) cameraIds.firstOrNull() ?: "0" else config.cameraId)
        select(resolutionSpinner, "${config.width}x${config.height}")
        select(fpsSpinner, config.fps.toString())
        select(bitrateSpinner, "${config.bitrate / 1_000_000} Mbps")

        val start = Button(this).apply {
            text = "Start camera"
            setOnClickListener {
                if (ContextCompat.checkSelfPermission(this@MainActivity, Manifest.permission.CAMERA) != PackageManager.PERMISSION_GRANTED) {
                    cameraPermission.launch(Manifest.permission.CAMERA)
                    return@setOnClickListener
                }
                saveConfig()
                ContextCompat.startForegroundService(
                    this@MainActivity,
                    Intent(this@MainActivity, CameraService::class.java).setAction(CameraService.ACTION_START)
                )
                updateStatus(true)
            }
        }
        root.addView(start)

        val apply = Button(this).apply {
            text = "Apply settings"
            setOnClickListener {
                saveConfig()
                ContextCompat.startForegroundService(
                    this@MainActivity,
                    Intent(this@MainActivity, CameraService::class.java).setAction(CameraService.ACTION_APPLY)
                )
                updateStatus(true)
            }
        }
        root.addView(apply)

        val stop = Button(this).apply {
            text = "Stop camera"
            setOnClickListener {
                startService(Intent(this@MainActivity, CameraService::class.java).setAction(CameraService.ACTION_STOP))
                updateStatus(false)
            }
        }
        root.addView(stop)

        setContentView(root)
        updateStatus(CameraService.running)
    }

    private fun addSpinner(root: LinearLayout, label: String, values: List<String>): Spinner {
        root.addView(TextView(this).apply { text = label })
        return Spinner(this).also { spinner ->
            spinner.adapter = ArrayAdapter(this, android.R.layout.simple_spinner_dropdown_item, values)
            root.addView(spinner)
        }
    }

    private fun saveConfig() {
        val resolution = resolutionSpinner.selectedItem.toString().split("x")
        CameraConfig(
            cameraId = cameraSpinner.selectedItem.toString(),
            width = resolution[0].toInt(),
            height = resolution[1].toInt(),
            fps = fpsSpinner.selectedItem.toString().toInt(),
            bitrate = bitrateSpinner.selectedItem.toString().substringBefore(" ").toInt() * 1_000_000,
            zoom = 1f + zoom.progress / 100f,
        ).save(this)
    }

    private fun rearCameraIds(): List<String> {
        val manager = getSystemService(Context.CAMERA_SERVICE) as CameraManager
        return manager.cameraIdList.filter { id ->
            manager.getCameraCharacteristics(id).get(CameraCharacteristics.LENS_FACING) == CameraCharacteristics.LENS_FACING_BACK
        }
    }

    private fun updateStatus(isRunning: Boolean) {
        val cfg = CameraConfig.load(this)
        val ip = lanAddress() ?: "PHONE_IP"
        status.text = if (isRunning) {
            "Running\nWeb: http://$ip:${cfg.httpPort}\nStream: $ip:${cfg.streamPort}"
        } else {
            "Stopped"
        }
    }

    private fun lanAddress(): String? = try {
        NetworkInterface.getNetworkInterfaces().toList()
            .flatMap { it.inetAddresses.toList() }
            .firstOrNull { it is Inet4Address && !it.isLoopbackAddress && it.isSiteLocalAddress }
            ?.hostAddress
    } catch (_: Exception) { null }

    private fun select(spinner: Spinner, value: String) {
        val adapter = spinner.adapter
        for (i in 0 until adapter.count) {
            if (adapter.getItem(i).toString() == value) {
                spinner.setSelection(i)
                return
            }
        }
    }
}
