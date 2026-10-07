#include <windows.h>
#include <mfapi.h>
#include <string>
#include <vector>

#include "FrameBridge.h"
#include "H264Decoder.h"
#include "NetworkReceiver.h"
#include "VirtualCamera.h"

namespace {
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

void setStatus(const std::wstring& text) {
    if (gStatus) SetWindowTextW(gStatus, text.c_str());
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
        gStatus = CreateWindowW(L"STATIC", L"Disconnected", WS_CHILD | WS_VISIBLE,
            16, 88, 372, 58, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_STATUS)), nullptr, nullptr);
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
            if (!gBridge.Start()) {
                setStatus(L"Cannot start local virtual-camera bridge on 127.0.0.1:8765.");
                return 0;
            }

            HRESULT hr = gDecoder.Initialize(1920, 1080, 15, [hwnd](std::vector<std::uint8_t> frame) {
                gBridge.Publish(frame);
                PostMessageW(hwnd, WM_CONNECTION_STATUS, 2, 0);
            });
            if (FAILED(hr)) {
                wchar_t text[256]{};
                wsprintfW(text, L"Cannot initialize H.264 decoder (0x%08X).", static_cast<unsigned>(hr));
                setStatus(text);
                return 0;
            }

            setStatus(L"Connecting to phone H.264 stream...");
            gReceiver.Start(host, port, [hwnd](std::vector<std::uint8_t> packet) {
                const HRESULT decodeHr = gDecoder.Push(packet);
                if (FAILED(decodeHr) && decodeHr != MF_E_TRANSFORM_NEED_MORE_INPUT) {
                    PostMessageW(hwnd, WM_DECODE_ERROR, static_cast<WPARAM>(decodeHr), 0);
                } else {
                    PostMessageW(hwnd, WM_CONNECTION_STATUS, 1, 0);
                }
            });

            hr = gCamera.Start();
            if (FAILED(hr)) {
                wchar_t text[256]{};
                wsprintfW(text, L"Receiver started, but virtual camera registration failed (0x%08X). Reinstall the latest setup.", static_cast<unsigned>(hr));
                setStatus(text);
            }
            return 0;
        }
        break;
    case WM_CONNECTION_STATUS:
        if (wParam == 2 && gCamera.IsStarted()) {
            setStatus(L"Connected. H.264 decoded and PhoneCam virtual camera is receiving frames.");
        } else if (gCamera.IsStarted()) {
            setStatus(L"Connected to phone. Waiting for first decoded video frame...");
        } else {
            setStatus(L"Receiving H.264, but virtual camera is not active.");
        }
        return 0;
    case WM_DECODE_ERROR: {
        wchar_t text[256]{};
        wsprintfW(text, L"H.264 decode error: 0x%08X. Phone must stream 1920x1080 @ 15 FPS for this MVP.", static_cast<unsigned>(wParam));
        setStatus(text);
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

    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"PhoneCam",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        CW_USEDEFAULT, CW_USEDEFAULT, 420, 200,
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
