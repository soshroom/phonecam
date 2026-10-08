package dev.soshroom.phonecam

import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.os.BatteryManager
import fi.iki.elonen.NanoHTTPD
import java.io.ByteArrayInputStream
import java.io.DataOutputStream
import java.net.ServerSocket
import java.net.Socket
import java.util.concurrent.ConcurrentHashMap
import java.util.concurrent.Executors
import java.util.concurrent.atomic.AtomicReference

class H264Server(
    private val port: Int,
    private val onClientConnected: (() -> Unit)? = null,
) {
    private var server: ServerSocket? = null
    private val clients = ConcurrentHashMap.newKeySet<Socket>()
    private val executor = Executors.newCachedThreadPool()
    @Volatile private var codecConfig: List<ByteArray> = emptyList()

    fun start() {
        server = ServerSocket(port)
        executor.execute {
            while (!server!!.isClosed) {
                try {
                    val socket = server!!.accept().apply {
                        tcpNoDelay = true
                        keepAlive = true
                    }
                    clients.add(socket)
                    codecConfig.forEach { sendPacket(socket, it) }
                    onClientConnected?.invoke()
                } catch (_: Exception) {
                    break
                }
            }
        }
    }

    fun setCodecConfig(config: List<ByteArray>) {
        codecConfig = config.map { it.copyOf() }
    }

    private fun sendPacket(socket: Socket, data: ByteArray) {
        if (data.isEmpty()) return
        synchronized(socket) {
            val out = DataOutputStream(socket.getOutputStream())
            out.writeInt(data.size)
            out.write(data)
            out.flush()
        }
    }

    fun broadcast(data: ByteArray) {
        if (data.isEmpty()) return
        clients.toList().forEach { socket ->
            try {
                sendPacket(socket, data)
            } catch (_: Exception) {
                clients.remove(socket)
                runCatching { socket.close() }
            }
        }
    }

    fun clientCount(): Int = clients.size

    fun stop() {
        runCatching { server?.close() }
        clients.forEach { runCatching { it.close() } }
        clients.clear()
        codecConfig = emptyList()
        executor.shutdownNow()
    }
}

class ControlServer(
    private val context: Context,
    port: Int,
    private val snapshot: AtomicReference<ByteArray?>,
    private val clients: () -> Int,
    private val uptime: () -> Long,
    private val encoderFps: () -> Double,
    private val encoderOutputBufferFps: () -> Double,
    private val partialBuffers: () -> Long,
    private val fpsRange: () -> String,
    private val applySettings: (CameraConfig) -> Unit,
    private val stopCamera: () -> Unit,
) : NanoHTTPD(port) {

    override fun serve(session: IHTTPSession): Response {
        return when {
            session.uri == "/" -> newFixedLengthResponse(Response.Status.OK, "text/html; charset=utf-8", PAGE)
            session.uri == "/preview.jpg" -> preview()
            session.uri == "/api/status" -> status()
            session.uri == "/api/settings" && session.method == Method.POST -> update(session)
            session.uri == "/api/stop" && session.method == Method.POST -> {
                Thread { Thread.sleep(100); stopCamera() }.start()
                newFixedLengthResponse(Response.Status.OK, MIME_PLAINTEXT, "stopping")
            }
            else -> newFixedLengthResponse(Response.Status.NOT_FOUND, MIME_PLAINTEXT, "not found")
        }.apply {
            addHeader("Cache-Control", "no-store, no-cache, must-revalidate")
            addHeader("Pragma", "no-cache")
        }
    }

    private fun preview(): Response {
        val data = snapshot.get()
            ?: return newFixedLengthResponse(Response.Status.SERVICE_UNAVAILABLE, MIME_PLAINTEXT, "preview unavailable")
        return newFixedLengthResponse(Response.Status.OK, "image/jpeg", ByteArrayInputStream(data), data.size.toLong())
    }

    private fun status(): Response {
        val cfg = CameraConfig.load(context)
        val battery = context.registerReceiver(null, IntentFilter(Intent.ACTION_BATTERY_CHANGED))
        val level = battery?.getIntExtra(BatteryManager.EXTRA_LEVEL, -1) ?: -1
        val scale = battery?.getIntExtra(BatteryManager.EXTRA_SCALE, 100) ?: 100
        val temperature = (battery?.getIntExtra(BatteryManager.EXTRA_TEMPERATURE, 0) ?: 0) / 10.0
        val percent = if (level >= 0) level * 100 / scale.coerceAtLeast(1) else -1
        val json = """{
            "running":true,
            "version":"${escape(BuildConfig.VERSION_NAME)}",
            "cameraId":"${escape(cfg.cameraId)}",
            "width":${cfg.width},
            "height":${cfg.height},
            "fps":${cfg.fps},
            "encoderFps":${"%.2f".format(java.util.Locale.US, encoderFps())},
            "encoderOutputBufferFps":${"%.2f".format(java.util.Locale.US, encoderOutputBufferFps())},
            "partialBuffers":${partialBuffers()},
            "fpsRange":"${escape(fpsRange())}",
            "bitrate":${cfg.bitrate},
            "zoom":${cfg.zoom},
            "clients":${clients()},
            "uptimeSeconds":${uptime() / 1000},
            "batteryPercent":$percent,
            "batteryTemperatureC":$temperature
        }""".trimIndent()
        return newFixedLengthResponse(Response.Status.OK, "application/json", json)
    }

    private fun update(session: IHTTPSession): Response {
        return try {
            session.parseBody(HashMap())
            val current = CameraConfig.load(context)
            val p = session.parms
            val cfg = current.copy(
                cameraId = p["cameraId"] ?: current.cameraId,
                width = p["width"]?.toIntOrNull() ?: current.width,
                height = p["height"]?.toIntOrNull() ?: current.height,
                fps = p["fps"]?.toIntOrNull()?.coerceIn(5, 30) ?: current.fps,
                bitrate = p["bitrate"]?.toIntOrNull()?.coerceIn(500_000, 20_000_000) ?: current.bitrate,
                zoom = p["zoom"]?.toFloatOrNull()?.coerceIn(1f, 4f) ?: current.zoom,
            )
            cfg.save(context)
            applySettings(cfg)
            newFixedLengthResponse(Response.Status.OK, "application/json", "{\"ok\":true}")
        } catch (e: Exception) {
            newFixedLengthResponse(Response.Status.BAD_REQUEST, MIME_PLAINTEXT, e.message ?: "bad request")
        }
    }

    private fun escape(value: String) = value.replace("\\", "\\\\").replace("\"", "\\\"")

    companion object {
        private val PAGE = """
<!doctype html>
<html>
<head>
<meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>PhoneCam ${BuildConfig.VERSION_NAME}</title>
<style>
body{font:15px system-ui,sans-serif;max-width:820px;margin:30px auto;padding:0 16px;background:#111;color:#eee}
.header{display:flex;align-items:baseline;gap:10px}.version{color:#999;font-size:14px;font-weight:400}
.card{background:#1c1c1c;border:1px solid #333;border-radius:10px;padding:16px;margin-bottom:14px}
img{width:100%;max-width:640px;background:#000;border-radius:8px}label{display:block;margin:9px 0 3px}
input{width:180px;padding:7px;background:#111;color:#eee;border:1px solid #444;border-radius:5px}
button{padding:8px 14px;margin:12px 6px 0 0}pre{white-space:pre-wrap}
</style>
</head><body>
<div class="header"><h1>PhoneCam</h1><span class="version">v${BuildConfig.VERSION_NAME}</span></div>
<div class="card"><img id="preview" alt="preview"></div>
<div class="card"><pre id="statusText">loading status...</pre></div>
<div class="card">
<label>Camera ID</label><input id="cameraId">
<label>Width</label><input id="width" type="number">
<label>Height</label><input id="height" type="number">
<label>FPS</label><input id="fps" type="number" min="5" max="30">
<label>Bitrate</label><input id="bitrate" type="number">
<label>Zoom</label><input id="zoom" type="number" min="1" max="4" step="0.1">
<br><button onclick="applySettings()">Apply</button><button onclick="stopCamera()">Stop camera</button>
</div>
<script>
const statusEl=document.getElementById('statusText');
const previewEl=document.getElementById('preview');
let initialized=false;
async function refresh(){
 try{
  const r=await fetch('/api/status?t='+Date.now(),{cache:'no-store'});
  if(!r.ok) throw new Error('HTTP '+r.status);
  const s=await r.json();
  statusEl.textContent='PhoneCam '+s.version+'\n'+s.width+'x'+s.height+' @ '+s.fps+' FPS requested\nEncoder frames: '+Number(s.encoderFps).toFixed(1)+' FPS\nEncoder buffers: '+Number(s.encoderOutputBufferFps).toFixed(1)+' FPS, partial buffers: '+s.partialBuffers+'\nAE range '+s.fpsRange+'\n'+(s.bitrate/1000000).toFixed(1)+' Mbps, zoom '+s.zoom+'x\nWindows clients: '+s.clients+'\nUptime: '+s.uptimeSeconds+'s\nBattery: '+s.batteryPercent+'%\nBattery temp: '+s.batteryTemperatureC+' C';
  if(!initialized){['cameraId','width','height','fps','bitrate','zoom'].forEach(k=>document.getElementById(k).value=s[k]);initialized=true;}
 }catch(e){statusEl.textContent='Status error: '+e.message;}
 previewEl.src='/preview.jpg?t='+Date.now();
}
async function applySettings(){
 const body=new URLSearchParams(); ['cameraId','width','height','fps','bitrate','zoom'].forEach(k=>body.set(k,document.getElementById(k).value));
 await fetch('/api/settings',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body}); initialized=false; refresh();
}
async function stopCamera(){await fetch('/api/stop',{method:'POST'});}
setInterval(refresh,1000);refresh();
</script></body></html>
        """.trimIndent()
    }
}
