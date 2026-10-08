#include <windows.h>
#include <mfapi.h>
#include <mferror.h>
#include <atomic>
#include <cstdint>
#include <sstream>
#include <string>
#include <vector>

#include "FrameBridge.h"
#include "H264Decoder.h"
#include "NetworkReceiver.h"
#include "VirtualCamera.h"

namespace {
constexpr wchar_t APP_VERSION[] = L"0.1.1";
constexpr int IDC_ADDRESS = 1001;
constexpr int IDC_CONNECT = 1002;
constexpr int IDC_STATUS = 1003;
constexpr UINT WM_CONNECTION_STATUS = WM_APP + 1;
constexpr UINT WM_DECODE_ERROR = WM_APP + 2;

NetworkReceiver gReceiver;
H264Decoder gDecoder;
FrameBridge gBridge;
VirtualCamera gCamera;
HWND gStatus = nullptr;
HRESULT gCameraStartHr = S_OK;
std::wstring gCameraStartStage;

struct StreamStats {
    std::atomic<std::uint64_t> packets{0};
    std::atomic<std::uint64_t> bytes{0};
    std::atomic<std::uint64_t> sps{0};
    std::atomic<std::uint64_t> pps{0};
    std::atomic<std::uint64_t> idr{0};
    std::atomic<std::uint64_t> vcl{0};
    std::atomic<std::uint64_t> decoded{0};
    std::atomic<std::uint64_t> avccConfig{0};
    std::atomic<int> lastNal{-1};

    void Reset() {
        packets = 0;
        bytes = 0;
        sps = 0;
        pps = 0;
        idr = 0;
        vcl = 0;
        decoded = 0;
        avccConfig = 0;
        lastNal = -1;
    }
} gStats;

void setStatus(const std::wstring& text) {
    if (gStatus) SetWindowTextW(gStatus, text.c_str());
}

void showCameraStartError() {
    wchar_t text[512]{};
    wsprintfW(
        text,
        L"Virtual camera failed at %s: 0x%08X. H.264 receiver is working.",
        gCameraStartStage.empty() ? L"unknown stage" : gCameraStartStage.c_str(),
        static_cast<unsigned>(gCameraStartHr)
    );
    setStatus(text);
}

std::size_t startCodeLength(const std::vector<std::uint8_t>& data, std::size_t pos) {
    if (pos + 4 <= data.size() && data[pos] == 0 && data[pos + 1] == 0 && data[pos + 2] == 0 && data[pos + 3] == 1) {
        return 4;
    }
    if (pos + 3 <= data.size() && data[pos] == 0 && data[pos + 1] == 0 && data[pos + 2] == 1) {
        return 3;
    }
    return 0;
}

void countNalType(int type) {
    gStats.lastNal = type;
    if (type == 7) ++gStats.sps;
    if (type == 8) ++gStats.pps;
    if (type == 5) ++gStats.idr;
    if (type >= 1 && type <= 5) ++gStats.vcl;
}

bool inspectAvcDecoderConfig(const std::vector<std::uint8_t>& packet) {
    if (packet.size() < 7 || packet[0] != 1) return false;

    std::size_t pos = 5;
    const std::uint8_t spsCount = packet[pos++] & 0x1f;
    if (spsCount == 0) return false;

    for (std::uint8_t i = 0; i < spsCount; ++i) {
        if (pos + 2 > packet.size()) return false;
        const std::size_t length =
            (static_cast<std::size_t>(packet[pos]) << 8) |
            static_cast<std::size_t>(packet[pos + 1]);
        pos += 2;
        if (length == 0 || pos + length > packet.size()) return false;
        countNalType(packet[pos] & 0x1f);
        pos += length;
    }

    if (pos >= packet.size()) return false;
    const std::uint8_t ppsCount = packet[pos++];
    if (ppsCount == 0) return false;
    for (std::uint8_t i = 0; i < ppsCount; ++i) {
        if (pos + 2 > packet.size()) return false;
        const std::size_t length =
            (static_cast<std::size_t>(packet[pos]) << 8) |
            static_cast<std::size_t>(packet[pos + 1]);
        pos += 2;
        if (length == 0 || pos + length > packet.size()) return false;
        countNalType(packet[pos] & 0x1f);
        pos += length;
    }

    ++gStats.avccConfig;
    return true;
}

void inspectPacket(const std::vector<std::uint8_t>& packet) {
    ++gStats.packets;
    gStats.bytes += packet.size();
    if (packet.empty()) return;

    bool sawAnnexB = false;
    for (std::size_t pos = 0; pos < packet.size();) {
        const auto code = startCodeLength(packet, pos);
        if (!code) {
            ++pos;
            continue;
        }
        sawAnnexB = true;
        const auto payload = pos + code;
        if (payload < packet.size()) countNalType(packet[payload] & 0x1f);
        pos = payload + 1;
    }
    if (sawAnnexB) return;

    if (inspectAvcDecoderConfig(packet)) return;

    std::size_t pos = 0;
    bool parsedLengthPrefixed = false;
    while (pos + 4 <= packet.size()) {
        const std::uint32_t length =
            (static_cast<std::uint32_t>(packet[pos]) << 24) |
            (static_cast<std::uint32_t>(packet[pos + 1]) << 16) |
            (static_cast<std::uint32_t>(packet[pos + 2]) << 8) |
            static_cast<std::uint32_t>(packet[pos + 3]);
        pos += 4;
        if (length == 0 || pos + length > packet.size()) {
            parsedLengthPrefixed = false;
            break;
        }
        parsedLengthPrefixed = true;
        countNalType(packet[pos] & 0x1f);
        pos += length;
    }
    if (parsedLengthPrefixed && pos == packet.size()) return;

    countNalType(packet[0] & 0x1f);
}

std::wstring streamStatusText(bool decodedNow) {
    std::wostringstream out;
    out << L"PhoneCam " << APP_VERSION << L"\r\n";
    if (decodedNow || gStats.decoded.load() > 0) {
        out << L"Connected. H.264 decoded; virtual camera is receiving frames.";
    } else {
        out << L"Connected to phone. Waiting for first decoded video frame...";
    }
    out << L"\r\nWindows pipeline: 1920x1080 @ 30 FPS";
    out << L"\r\nPackets: " << gStats.packets.load()
        << L", bytes: " << gStats.bytes.load();
    out << L"\r\nNAL: SPS=" << gStats.sps.load()
        << L" PPS=" << gStats.pps.load()
        << L" IDR=" << gStats.idr.load()
        << L" VCL=" << gStats.vcl.load()
        << L" last=" << gStats.lastNal.load();
    out << L"\r\navcC config packets: " << gStats.avccConfig.load();
    out << L"\r\nDecoded NV12 frames: " << gStats.decoded.load();
    return out.str();
}

bool splitAddress(const std::wstring& value, std::wstring& host, std::uint16_t& port) {
    const auto pos = value.rfind(L':');
    if (pos == std::wstring::npos) {
        host = value;
        port = 8554;
        return !host.empty();
    }
    host = value.substr(0, pos);
    try {
        const auto parsed = std::stoul(value.substr(pos + 1));
        if (parsed == 0 || parsed > 65535) return false;
        port = static_cast<std::uint16_t>(parsed);
        return !host.empty();
    } catch (...) {
        return false;
    }
}

LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE: {
        CreateWindowW(L"STATIC", L"Phone stream address", WS_CHILD | WS_VISIBLE,
            16, 18, 150, 22, hwnd, nullptr, nullptr, nullptr);
        CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"192.168.1.42:8554",
            WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
            16, 44, 270, 28, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_ADDRESS)), nullptr, nullptr);
        CreateWindowW(L"BUTTON", L"Connect", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            298, 44, 90, 28, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_CONNECT)), nullptr, nullptr);
        gStatus = CreateWindowW(L"STATIC", L"PhoneCam 0.1.1\r\nDisconnected", WS_CHILD | WS_VISIBLE,
            16, 88, 420, 190, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_STATUS)), nullptr, nullptr);
        return 0;
    }
    case WM_COMMAND:
        if (LOWORD(wParam) == IDC_CONNECT) {
            wchar_t buffer[256]{};
            GetWindowTextW(GetDlgItem(hwnd, IDC_ADDRESS), buffer, 256);
            std::wstring host;
            std::uint16_t port{};
            if (!splitAddress(buffer, host, port)) {
                setStatus(L"Invalid address. Example: 192.168.1.42:8554");
                return 0;
            }

            gReceiver.Stop();
            gDecoder.Reset();
            gStats.Reset();
            gCameraStartHr = S_OK;
            gCameraStartStage.clear();

            if (!gBridge.Start()) {
                setStatus(L"Cannot start local virtual-camera bridge on 127.0.0.1:8765.");
                return 0;
            }

            HRESULT hr = gDecoder.Initialize(1920, 1080, 30, [hwnd](std::vector<std::uint8_t> frame) {
                ++gStats.decoded;
                gBridge.Publish(frame);
                PostMessageW(hwnd, WM_CONNECTION_STATUS, 2, 0);
            });
            if (FAILED(hr)) {
                wchar_t text[256]{};
                wsprintfW(text, L"Cannot initialize H.264 decoder (0x%08X).", static_cast<unsigned>(hr));
                setStatus(text);
                return 0;
            }

            hr = gCamera.Start();
            if (FAILED(hr)) {
                gCameraStartHr = hr;
                gCameraStartStage = gCamera.LastStage();
                showCameraStartError();
            } else {
                setStatus(L"PhoneCam 0.1.1\r\nVirtual camera started. Connecting to phone H.264 stream...");
            }

            gReceiver.Start(host, port, [hwnd](std::vector<std::uint8_t> packet) {
                inspectPacket(packet);
                const HRESULT decodeHr = gDecoder.Push(packet);
                if (FAILED(decodeHr) && decodeHr != MF_E_TRANSFORM_NEED_MORE_INPUT) {
                    PostMessageW(hwnd, WM_DECODE_ERROR, static_cast<WPARAM>(decodeHr), 0);
                } else {
                    PostMessageW(hwnd, WM_CONNECTION_STATUS, 1, 0);
                }
            });
            return 0;
        }
        break;
    case WM_CONNECTION_STATUS:
        if (FAILED(gCameraStartHr)) {
            showCameraStartError();
        } else if (gCamera.IsStarted()) {
            setStatus(streamStatusText(wParam == 2));
        } else {
            setStatus(L"Receiving H.264, but virtual camera is not active.");
        }
        return 0;
    case WM_DECODE_ERROR: {
        if (FAILED(gCameraStartHr)) {
            showCameraStartError();
            return 0;
        }
        std::wostringstream out;
        out << L"PhoneCam " << APP_VERSION;
        out << L"\r\nH.264 decode error: 0x" << std::hex << static_cast<unsigned>(wParam) << std::dec;
        out << L"\r\nPackets: " << gStats.packets.load();
        out << L" SPS=" << gStats.sps.load()
            << L" PPS=" << gStats.pps.load()
            << L" IDR=" << gStats.idr.load()
            << L" VCL=" << gStats.vcl.load()
            << L" last=" << gStats.lastNal.load();
        out << L" avcC=" << gStats.avccConfig.load();
        out << L"\r\nDecoded NV12 frames: " << gStats.decoded.load();
        setStatus(out.str());
        return 0;
    }
    case WM_DESTROY:
        gReceiver.Stop();
        gDecoder.Reset();
        gBridge.Stop();
        gCamera.Stop();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) return 1;
    if (FAILED(MFStartup(MF_VERSION))) {
        CoUninitialize();
        return 1;
    }

    WNDCLASSW wc{};
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = instance;
    wc.lpszClassName = L"PhoneCamWindow";
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    RegisterClassW(&wc);

    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"PhoneCam 0.1.1",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        CW_USEDEFAULT, CW_USEDEFAULT, 470, 360,
        nullptr, nullptr, instance, nullptr);
    if (!hwnd) {
        MFShutdown();
        CoUninitialize();
        return 1;
    }

    ShowWindow(hwnd, show);
    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    MFShutdown();
    CoUninitialize();
    return static_cast<int>(msg.wParam);
}
