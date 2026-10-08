# PhoneCam

[![Android](https://github.com/soshroom/phonecam/actions/workflows/android.yml/badge.svg)](https://github.com/soshroom/phonecam/actions/workflows/android.yml)
[![Windows](https://github.com/soshroom/phonecam/actions/workflows/windows.yml/badge.svg)](https://github.com/soshroom/phonecam/actions/workflows/windows.yml)

PhoneCam turns an Android phone into a low-overhead webcam for a Windows 11 PC over the local network.

The phone captures video with Camera2, encodes it to H.264 in hardware through Android `MediaCodec`, and sends the encoded stream directly to the Windows client over TCP. Windows decodes the stream with Media Foundation and exposes the decoded frames as a system virtual camera named `PhoneCam`.

The project is intentionally local-first. There is no cloud relay, account, audio path, authentication layer, or telemetry service.

> **Project status:** early alpha. The core Android -> LAN -> Windows -> virtual camera pipeline works, but compatibility across Android devices, camera modes and Windows applications is still being improved.

## Requirements

### Android

- Android 12 or newer, API 31+
- a device with a camera and H.264/AVC hardware encoder support
- phone and PC connected to the same local network

### Windows

- 64-bit Windows 11
- a current Windows 11 Media Foundation runtime
- administrator rights for installation, because the installer registers the Media Foundation camera source

For the current Windows pipeline, use **1920x1080 at 30 FPS** on Android. The Android app exposes other resolutions and frame rates for testing, but the Windows virtual camera source currently advertises a fixed 1920x1080 NV12 stream at 30 FPS.

## Download and quick start

Automated development builds are published in [GitHub Releases](https://github.com/soshroom/phonecam/releases).

Release assets currently include:

- `PhoneCam-android-debug.apk` - Android development APK
- `PhoneCamSetup-x64.exe` - recommended Windows installer
- `PhoneCam-windows-x64.exe` - standalone Windows executable
- `PhoneCamSource-x64.dll` - Media Foundation virtual camera source used by the installer

The current builds are **not production-signed**. Android releases use a debug build and Windows binaries are not code-signed, so Windows SmartScreen or Android installation warnings are expected.

### 1. Install and start the Android app

Install the APK on an Android 12+ phone and grant camera permission.

In PhoneCam, select:

- camera
- resolution
- FPS
- bitrate
- zoom

For the Windows client, start with:

```text
Resolution: 1920x1080
FPS:        30
Bitrate:    5 Mbps
Zoom:       1.0x
```

Press **Start camera**.

The app shows the phone's LAN address and the two listening ports. By default:

```text
Web control: http://PHONE_IP:8080
Video:       PHONE_IP:8554
```

The camera runs inside an Android foreground service and holds a partial wake lock, so streaming can continue while the PhoneCam activity is in the background or the screen is off.

### 2. Optional browser control page

Open this address from another device on the same network:

```text
http://PHONE_IP:8080
```

The page provides:

- requested resolution and FPS
- measured encoder FPS
- active camera AE FPS range
- bitrate and zoom
- connected Windows client count
- uptime
- battery percentage and battery temperature
- runtime settings
- stop control

There is intentionally no browser video/JPEG preview. The camera session is dedicated to the H.264 stream so browser monitoring cannot introduce extra Camera2 capture requests or disturb the video pipeline. Live video should be checked through the Windows virtual camera.

Changing camera ID, resolution or FPS restarts the Android camera/encoder pipeline. Bitrate and zoom can be applied without a full restart.

### 3. Install and connect the Windows client

Run `PhoneCamSetup-x64.exe`. The installer places `PhoneCam.exe` and `PhoneCamSource.dll` under Program Files and registers the Media Foundation camera source.

Start **PhoneCam**, enter the stream address shown by the Android app, for example:

```text
192.168.1.42:8554
```

Then press **Connect**.

The Windows client will:

1. connect to the Android TCP stream
2. receive H.264 codec configuration and access units
3. decode H.264 with Media Foundation
4. convert the decoded video into the NV12 frame stream expected by the virtual camera source
5. start the Windows software virtual camera named `PhoneCam`
6. reconnect automatically if the phone stream temporarily disappears

After that, applications using the Windows camera stack should be able to see **PhoneCam** as a camera source.

## How it works

```text
                       Android phone

     Camera2
        |
        v
 MediaCodec H.264
 hardware encoder
        |
        | TCP :8554
        |
        +--------------- LAN ----------------+
                                             |
                                             v
                                        Windows client
                                             |
                                      NetworkReceiver
                                             |
                                             v
                                    Media Foundation
                                      H.264 decoder
                                             |
                                      decoded NV12
                                             |
                                             v
                                    local FrameBridge
                                      127.0.0.1:8765
                                             |
                                             v
                                   PhoneCamSource.dll
                               Media Foundation media source
                                             |
                                             v
                                   Windows virtual camera
                                         "PhoneCam"
                                             |
                                             v
                                  browser / call / video app

 Android HTTP control/status server remains available separately on :8080.
```

### Android capture path

`CameraService` owns the Android camera pipeline.

The Camera2 session has a single output target: the `MediaCodec` input surface used for the H.264 video stream. The browser preview and JPEG `ImageReader` path were removed so periodic snapshot captures cannot interrupt the recording pipeline or retrigger camera AF/AE behavior.

The encoder is configured for AVC/H.264, constant bitrate, no B-frames and a two-second keyframe interval. When a Windows client connects, the service requests a fresh sync frame so startup does not have to wait for the next normal keyframe.

The Android service also keeps the latest codec configuration buffers, such as SPS/PPS, and sends them to newly connected clients before normal video packets.

### Phone -> Windows transport

PhoneCam currently uses a deliberately small custom TCP framing protocol rather than RTSP, WebRTC or another media stack.

Each packet is encoded as:

```text
+-------------------------------+
| 4-byte big-endian payload len |
+-------------------------------+
| N bytes of H.264 payload      |
+-------------------------------+
```

Codec configuration buffers travel through the same framing protocol as encoded video access units.

The Windows receiver enables TCP keepalive and `TCP_NODELAY`, detects stalled/disconnected streams and reconnects automatically.

### Windows decode and virtual camera path

The Windows process contains two separate pieces:

- `PhoneCam.exe` receives and decodes the phone stream
- `PhoneCamSource.dll` implements the Media Foundation source used by Windows Frame Server

`PhoneCam.exe` decodes the H.264 stream and publishes the newest NV12 frame to a loopback-only bridge on `127.0.0.1:8765`.

`PhoneCamSource.dll` connects to that bridge and provides those frames through a Media Foundation virtual camera registered with `MFCreateVirtualCamera`.

The loopback bridge is intentionally bound only to localhost. It is not exposed to the LAN.

## Ports

| Port | Side | Purpose | Exposure |
| --- | --- | --- | --- |
| `8080/tcp` | Android | browser status and controls | LAN |
| `8554/tcp` | Android | length-prefixed H.264 stream | LAN |
| `8765/tcp` | Windows | decoded NV12 bridge between EXE and camera source DLL | localhost only |

The Android ports are configurable in `CameraConfig`, although the current app UI keeps the default values.

## HTTP API

The Android control server currently exposes a very small API.

### `GET /api/status`

Returns current stream state and diagnostics, including configuration, measured encoder rates, client count, uptime and battery information.

### `POST /api/settings`

Accepts `application/x-www-form-urlencoded` fields:

```text
cameraId
width
height
fps
bitrate
zoom
```

Current input limits enforced by the control server:

- FPS: 5-30
- bitrate: 500 Kbps-20 Mbps
- zoom: 1x-4x

### `POST /api/stop`

Stops the Android camera service.

## Security model

PhoneCam is designed for a trusted local network.

The Android HTTP control server and H.264 TCP stream currently have:

- no authentication
- no TLS/encryption
- no access-control list

Anyone who can reach the phone on ports 8080 or 8554 may be able to access the video stream or change camera settings.

**Do not expose these ports to the public internet.** Do not configure router port forwarding for PhoneCam. Use it only on a network you trust.

The Windows decoded-frame bridge is safer by design because it listens only on `127.0.0.1`.

## Current limitations

- Android 12+ only
- Windows 11 x64 only
- video only, no microphone/audio support
- Windows virtual camera currently fixed to 1920x1080 at 30 FPS
- no automatic device discovery, the phone address is entered manually
- no authentication or encrypted transport
- no production code signing yet
- Android release artifact is currently a debug APK
- vendor-specific Android power management may affect very long background sessions
- camera/FPS/resolution support still depends on the physical Android device and its Camera2/MediaCodec implementation

## Repository layout

```text
.
├── android/
│   └── app/src/main/java/dev/soshroom/phonecam/
│       ├── CameraConfig.kt     persisted camera/network settings
│       ├── CameraService.kt    Camera2 + MediaCodec capture pipeline
│       ├── MainActivity.kt     minimal Android control UI
│       └── Servers.kt          H.264 TCP and HTTP control servers
│
├── windows/
│   ├── src/
│   │   ├── main.cpp            Windows UI and pipeline orchestration
│   │   ├── NetworkReceiver.*   phone TCP receiver and reconnect loop
│   │   ├── H264Decoder.*       Media Foundation H.264 decoder
│   │   ├── FrameBridge.*       localhost decoded-frame bridge
│   │   └── VirtualCamera.*     virtual camera registration/startup
│   ├── source/
│   │   ├── PhoneCamFrameServerSource.cpp
│   │   └── FrameClient.*       Media Foundation camera source DLL
│   └── installer/
│       └── PhoneCam.iss        Inno Setup installer
│
└── .github/workflows/
    ├── android.yml
    └── windows.yml
```

## Building from source

### Android

Requirements:

- JDK 17
- Android SDK 35
- Gradle 8.10.x

Build the debug APK:

```bash
cd android
gradle :app:assembleDebug
```

Output:

```text
android/app/build/outputs/apk/debug/app-debug.apk
```

### Windows

Requirements:

- Visual Studio 2022 with Desktop development with C++
- Windows 11 SDK with Media Foundation virtual camera APIs
- CMake 3.25+
- C++20 compiler

Configure and build:

```powershell
cmake -S windows -B build/windows -A x64
cmake --build build/windows --config Release --parallel
```

Expected outputs:

```text
build/windows/Release/PhoneCam.exe
build/windows/Release/PhoneCamSource.dll
```

To build the installer, install Inno Setup 6 and run:

```powershell
& "C:\Program Files (x86)\Inno Setup 6\ISCC.exe" windows\installer\PhoneCam.iss
```

Installer output:

```text
windows/installer/output/PhoneCamSetup.exe
```

## CI and releases

GitHub Actions builds both platforms.

- `android.yml` builds the Android debug APK
- `windows.yml` builds the Windows x64 executable, source DLL and Inno Setup installer
- pushes to `main` publish build artifacts to an automated release tagged with the commit SHA
- pull requests run the relevant build without publishing release assets

## Troubleshooting

### Windows cannot connect to the phone

Check that:

- both devices are on the same LAN
- Android PhoneCam says `Running`
- the IP address entered in Windows matches the address shown by Android
- port 8554 is not blocked by Wi-Fi client isolation or another firewall rule

### Browser control page does not open

Try `http://PHONE_IP:8080` from another device on the same network. Some guest Wi-Fi networks block traffic between clients.

### Windows connects but the virtual camera does not appear

Use the installer rather than only copying `PhoneCam.exe`. The installer also installs and registers `PhoneCamSource.dll`, which is required by the Media Foundation virtual camera.

Close and reopen the application that should use the webcam after PhoneCam has started the virtual camera.

### Video connects but decoding does not start

Start with Android configured for 1920x1080, 30 FPS and 5 Mbps. The Windows pipeline is currently built around that exact output mode.

The Windows status panel shows packet counts, SPS/PPS/IDR information, measured input FPS and decoded-frame counts, which can help distinguish a network problem from a decoder problem.

## Contributing

Bug reports and focused pull requests are welcome. When reporting a problem, include:

- Android device model and Android version
- selected camera ID, resolution, FPS and bitrate
- Windows version
- PhoneCam build/release identifier
- Windows status text or relevant GitHub Actions logs
- exact application in which the virtual camera was tested, when relevant

For security-sensitive reports, see [`SECURITY.md`](SECURITY.md).
