# PhoneCam

PhoneCam turns an Android 12+ phone into a low-overhead LAN webcam for Windows 11.

## Goals

- hardware H.264 encoding on Android via `MediaCodec`
- camera continues streaming with the screen off
- simple Android control panel
- runtime configuration without rebuilding
- browser status/control page with lightweight preview
- Windows 11 receiver with a minimal UI
- Windows virtual camera through Media Foundation
- no audio, no cloud, no authentication, LAN only

## Architecture

```text
Android Camera2
   |-- MediaCodec H.264 --> TCP :8554 --> Windows receiver --> Media Foundation virtual camera
   |
   `-- low-rate JPEG --> HTTP :8080 --> browser preview/status/control
```

The TCP protocol is deliberately small. Every H.264 access unit is sent as:

```text
4-byte big-endian payload length
N-byte encoded H.264 payload
```

Codec configuration buffers from MediaCodec are sent through the same stream. The Windows receiver is expected to keep SPS/PPS and feed the resulting elementary H.264 stream into Media Foundation.

## Android defaults

- Android 12+ / API 31+
- back camera only, with selectable physical camera ID
- 1920x1080
- 15 FPS
- 5 Mbps H.264 AVC
- autofocus
- digital zoom
- foreground camera service + partial wake lock
- HTTP control port 8080
- H.264 stream port 8554

FPS, bitrate and zoom can be applied while streaming. Resolution or camera ID changes restart the camera pipeline automatically.

## Repository

- `android/` Android application
- `windows/` Windows 11 receiver and virtual-camera host
- `.github/workflows/` CI builds

## Current MVP scope

The Android side contains the complete basic capture/server pipeline. The Windows side is intentionally kept native and small. It contains the H.264 network receiver and Media Foundation bootstrap; the virtual-camera media source is isolated behind `VirtualCamera` so the more verbose Media Foundation source implementation does not leak into networking/UI code.

## Android usage

1. Build/install the APK.
2. Grant camera permission.
3. Choose camera settings and press Start.
4. Keep the phone and PC on the same LAN.
5. Open `http://PHONE_IP:8080` for status and preview.
6. Use `PHONE_IP:8554` in the Windows client.

The foreground service keeps the camera active when the app is backgrounded or the display is turned off.

## Windows usage

Build with Visual Studio 2022 / MSVC and the Windows 11 SDK. The receiver accepts an address such as `192.168.1.42:8554` and continuously reconnects after disconnects.

The installer is built with WiX in CI. Windows virtual cameras require a supported Windows 11 SDK/runtime and are registered by the application through Media Foundation rather than a legacy kernel driver.

## CI

- Android workflow builds a debug APK on pushes and pull requests.
- Windows workflow builds x64 Release artifacts.

Release signing is intentionally not configured yet. Add Android keystore and Windows code-signing secrets before public distribution.
