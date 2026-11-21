#include "control_panel.h"
#include "asio_registry.h"

#include <commctrl.h>
#include <cstdio>
#include <objbase.h>
#include <vector>

namespace {

constexpr int IDC_COMBO_INPUT   = 101;
constexpr int IDC_BTN_IN_PANEL  = 102;
constexpr int IDC_COMBO_OUTPUT  = 103;
constexpr int IDC_BTN_OUT_PANEL = 104;
constexpr int IDC_COMBO_MARGIN  = 105;
constexpr int IDC_STATUS_TEXT   = 106;
constexpr int IDC_BTN_RESET_XR  = 107;
constexpr int IDC_BTN_APPLY     = 108;
constexpr int IDC_BTN_CLOSE     = 109;

constexpr UINT_PTR kStatusTimerId = 1;

constexpr int kMarginOptions[] = { 0, 128, 192, 256, 384, 512, 768, 1024 };

std::wstring utf8ToWide(const std::string& s) {
    if (s.empty()) return {};
    int len = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (len <= 0) return {};
    std::wstring out(static_cast<size_t>(len), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), len);
    return out;
}

std::string wideToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    if (len <= 0) return {};
    std::string out(static_cast<size_t>(len), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), out.data(), len, nullptr, nullptr);
    return out;
}

struct PanelState {
    ControlPanelHost* host = nullptr;
    HFONT font = nullptr;
    HFONT monoFont = nullptr;
    HWND comboInput = nullptr;
    HWND comboOutput = nullptr;
    HWND comboMargin = nullptr;
    HWND statusEdit = nullptr;
    std::vector<AsioDriverEntry> drivers;
};

void updateStatusDisplay(PanelState* st) {
    if (!st || !st->host || !st->statusEdit) return;
    BridgeStatusSnapshot s = st->host->getStatus();

    wchar_t buf[1024]{};
    const wchar_t* stateStr = s.streaming ? (s.ring.primed ? L"Streaming (Locked)" : L"Streaming (Priming...)") : L"Idle";
    double latencyMs = (s.sampleRate > 0.0) ? (s.ring.fillFrames * 1000.0 / s.sampleRate) : 0.0;

    std::swprintf(buf, std::size(buf),
        L"State:        %s\r\n"
        L"Sample Rate:  %.0f Hz\r\n"
        L"Buffers:      In %ld / Out %ld frames\r\n"
        L"Ring Fill:    %.1f / %.0f frames (%.2f ms)\r\n"
        L"Clock Drift:  %+.1f ppm\r\n"
        L"Xruns:        %llu underruns, %llu overruns",
        stateStr,
        s.sampleRate,
        s.inputBufferSize,
        s.outputBufferSize,
        s.ring.fillFrames,
        s.ring.targetFillFrames,
        latencyMs,
        s.ring.driftPpm,
        static_cast<unsigned long long>(s.ring.underruns),
        static_cast<unsigned long long>(s.ring.overruns)
    );
    SetWindowTextW(st->statusEdit, buf);
}

std::string getComboSelection(HWND combo, const std::vector<AsioDriverEntry>& drivers) {
    LRESULT idx = SendMessageW(combo, CB_GETCURSEL, 0, 0);
    if (idx >= 0 && static_cast<size_t>(idx) < drivers.size()) {
        return drivers[static_cast<size_t>(idx)].name;
    }
    return {};
}

LRESULT CALLBACK panelWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto* st = reinterpret_cast<PanelState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    switch (msg) {
        case WM_CREATE: {
            auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
            st = reinterpret_cast<PanelState*>(cs->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(st));

            st->font = CreateFontW(-14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
            st->monoFont = CreateFontW(-13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                FIXED_PITCH | FF_MODERN, L"Consolas");

            auto setFont = [&](HWND h, HFONT f) {
                SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(f), TRUE);
            };

            const int pad = 14;
            int y = pad;

            // Input ASIO Device
            HWND lblIn = CreateWindowExW(0, L"STATIC", L"Input ASIO Device (Guitar / Mic):",
                WS_CHILD | WS_VISIBLE, pad, y, 300, 20, hwnd, nullptr, nullptr, nullptr);
            setFont(lblIn, st->font);
            y += 22;

            st->comboInput = CreateWindowExW(0, L"COMBOBOX", nullptr,
                WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP,
                pad, y, 290, 200, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_COMBO_INPUT)), nullptr, nullptr);
            setFont(st->comboInput, st->font);

            HWND btnInPanel = CreateWindowExW(0, L"BUTTON", L"Device Panel...",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                pad + 300, y - 1, 115, 25, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_BTN_IN_PANEL)), nullptr, nullptr);
            setFont(btnInPanel, st->font);
            y += 36;

            // Output ASIO Device
            HWND lblOut = CreateWindowExW(0, L"STATIC", L"Output ASIO Device (Master Clock / Playback):",
                WS_CHILD | WS_VISIBLE, pad, y, 320, 20, hwnd, nullptr, nullptr, nullptr);
            setFont(lblOut, st->font);
            y += 22;

            st->comboOutput = CreateWindowExW(0, L"COMBOBOX", nullptr,
                WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP,
                pad, y, 290, 200, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_COMBO_OUTPUT)), nullptr, nullptr);
            setFont(st->comboOutput, st->font);

            HWND btnOutPanel = CreateWindowExW(0, L"BUTTON", L"Device Panel...",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                pad + 300, y - 1, 115, 25, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_BTN_OUT_PANEL)), nullptr, nullptr);
            setFont(btnOutPanel, st->font);
            y += 36;

            // Ring Buffer Margin
            HWND lblMargin = CreateWindowExW(0, L"STATIC", L"Bridge Input Buffer Margin:",
                WS_CHILD | WS_VISIBLE, pad, y + 3, 180, 20, hwnd, nullptr, nullptr, nullptr);
            setFont(lblMargin, st->font);

            st->comboMargin = CreateWindowExW(0, L"COMBOBOX", nullptr,
                WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP,
                pad + 190, y, 225, 200, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_COMBO_MARGIN)), nullptr, nullptr);
            setFont(st->comboMargin, st->font);
            y += 36;

            // Status Group Box
            HWND lblStatus = CreateWindowExW(0, L"STATIC", L"Live Bridge Telemetry:",
                WS_CHILD | WS_VISIBLE, pad, y, 200, 20, hwnd, nullptr, nullptr, nullptr);
            setFont(lblStatus, st->font);
            y += 22;

            st->statusEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_READONLY,
                pad, y, 415, 116, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_STATUS_TEXT)), nullptr, nullptr);
            setFont(st->statusEdit, st->monoFont);
            y += 126;

            // Action Buttons
            HWND btnReset = CreateWindowExW(0, L"BUTTON", L"Reset Xruns",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                pad, y, 105, 28, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_BTN_RESET_XR)), nullptr, nullptr);
            setFont(btnReset, st->font);

            HWND btnApply = CreateWindowExW(0, L"BUTTON", L"Save && Apply",
                WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON | WS_TABSTOP,
                pad + 205, y, 115, 28, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_BTN_APPLY)), nullptr, nullptr);
            setFont(btnApply, st->font);

            HWND btnClose = CreateWindowExW(0, L"BUTTON", L"Close",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                pad + 330, y, 85, 28, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_BTN_CLOSE)), nullptr, nullptr);
            setFont(btnClose, st->font);

            // Populate drivers
            st->drivers = enumerateAsioDrivers(true);
            BridgeConfig cfg = st->host ? st->host->getConfig() : loadBridgeConfig();

            int inSel = 0;
            int outSel = 0;
            for (size_t i = 0; i < st->drivers.size(); ++i) {
                std::wstring nameW = utf8ToWide(st->drivers[i].name);
                SendMessageW(st->comboInput, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(nameW.c_str()));
                SendMessageW(st->comboOutput, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(nameW.c_str()));
                if (st->drivers[i].name == cfg.inputDriver) inSel = static_cast<int>(i);
                if (st->drivers[i].name == cfg.outputDriver) outSel = static_cast<int>(i);
            }
            if (!st->drivers.empty()) {
                SendMessageW(st->comboInput, CB_SETCURSEL, inSel, 0);
                SendMessageW(st->comboOutput, CB_SETCURSEL, outSel, 0);
            }

            // Populate margin options
            int marginSel = 0;
            for (size_t i = 0; i < std::size(kMarginOptions); ++i) {
                wchar_t label[64]{};
                if (kMarginOptions[i] == 0) {
                    wcscpy_s(label, L"Auto (Lowest Safe Latency)");
                } else {
                    std::swprintf(label, std::size(label), L"%d frames", kMarginOptions[i]);
                }
                SendMessageW(st->comboMargin, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label));
                if (kMarginOptions[i] == cfg.ringBufferFrames) {
                    marginSel = static_cast<int>(i);
                }
            }
            SendMessageW(st->comboMargin, CB_SETCURSEL, marginSel, 0);

            updateStatusDisplay(st);
            SetTimer(hwnd, kStatusTimerId, 250, nullptr);
            return 0;
        }

        case WM_TIMER: {
            if (wParam == kStatusTimerId) {
                updateStatusDisplay(st);
            }
            return 0;
        }

        case WM_COMMAND: {
            int id = LOWORD(wParam);
            if (!st || !st->host) break;

            if (id == IDC_BTN_IN_PANEL) {
                st->host->openInputDriverPanel();
            } else if (id == IDC_BTN_OUT_PANEL) {
                st->host->openOutputDriverPanel();
            } else if (id == IDC_BTN_RESET_XR) {
                st->host->resetXruns();
                updateStatusDisplay(st);
            } else if (id == IDC_BTN_APPLY) {
                BridgeConfig cfg;
                cfg.inputDriver = getComboSelection(st->comboInput, st->drivers);
                cfg.outputDriver = getComboSelection(st->comboOutput, st->drivers);
                LRESULT mIdx = SendMessageW(st->comboMargin, CB_GETCURSEL, 0, 0);
                if (mIdx >= 0 && static_cast<size_t>(mIdx) < std::size(kMarginOptions)) {
                    cfg.ringBufferFrames = kMarginOptions[mIdx];
                }
                if (cfg.inputDriver == cfg.outputDriver && !cfg.inputDriver.empty()) {
                    MessageBoxW(hwnd, L"Input and Output ASIO devices should be different devices.",
                        L"SplitASIO", MB_OK | MB_ICONWARNING);
                } else {
                    saveBridgeConfig(cfg);
                    st->host->applyConfig(cfg);
                    MessageBoxW(hwnd, L"Configuration saved. If your DAW does not automatically reload the driver, re-select SplitASIO in your DAW's audio settings.",
                        L"SplitASIO", MB_OK | MB_ICONINFORMATION);
                }
            } else if (id == IDC_BTN_CLOSE) {
                DestroyWindow(hwnd);
            }
            return 0;
        }

        case WM_CLOSE: {
            DestroyWindow(hwnd);
            return 0;
        }

        case WM_DESTROY: {
            KillTimer(hwnd, kStatusTimerId);
            if (st) {
                if (st->font) DeleteObject(st->font);
                if (st->monoFont) DeleteObject(st->monoFont);
            }
            PostQuitMessage(0);
            return 0;
        }
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

} // namespace

ControlPanel::~ControlPanel() {
    close();
}

void ControlPanel::show(ControlPanelHost* host, HWND /*ownerHwnd*/) {
    std::lock_guard<std::mutex> lock(mutex_);
    HWND existing = hwnd_.load();
    if (existing && IsWindow(existing)) {
        ShowWindow(existing, SW_RESTORE);
        SetForegroundWindow(existing);
        return;
    }

    if (uiThread_.joinable()) {
        uiThread_.join();
    }

    running_.store(true);
    uiThread_ = std::thread(&ControlPanel::uiThreadMain, this, host);
}

void ControlPanel::runModal(ControlPanelHost* host) {
    running_.store(true);
    uiThreadMain(host);
}

void ControlPanel::close() {
    HWND h = hwnd_.load();
    if (h && IsWindow(h)) {
        PostMessageW(h, WM_CLOSE, 0, 0);
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (uiThread_.joinable()) {
        uiThread_.join();
    }
    hwnd_.store(nullptr);
    running_.store(false);
}

void ControlPanel::uiThreadMain(ControlPanelHost* host) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    static const wchar_t* kClassName = L"SplitAsioControlPanelWnd";
    HINSTANCE hInst = GetModuleHandleW(nullptr);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = panelWndProc;
    wc.hInstance = hInst;
    // Win32's ANSI/Unicode macro roulette: IDC_ARROW expands to MAKEINTRESOURCEA(32512)
    // unless UNICODE is defined globally, which makes LoadCursorW() throw a type tantrum.
    // Docs: https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-loadcursorw
    wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512)); // IDC_ARROW
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    wc.lpszClassName = kClassName;
    RegisterClassExW(&wc);

    PanelState state{};
    state.host = host;

    RECT rc{ 0, 0, 444, 382 };
    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    AdjustWindowRect(&rc, style, FALSE);

    HWND hwnd = CreateWindowExW(
        0,
        kClassName,
        L"SplitASIO - Control Panel",
        style,
        CW_USEDEFAULT, CW_USEDEFAULT,
        rc.right - rc.left, rc.bottom - rc.top,
        nullptr, nullptr, hInst, &state);

    hwnd_.store(hwnd);

    if (hwnd) {
        ShowWindow(hwnd, SW_SHOW);
        UpdateWindow(hwnd);

        MSG msg{};
        while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
            if (!IsDialogMessageW(hwnd, &msg)) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
        }
    }

    hwnd_.store(nullptr);
    running_.store(false);
    CoUninitialize();
}
