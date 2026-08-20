#include "WasapiLowLatencyEngine.h"
#include "AppVersion.h"
#include <Windows.h>
#include <shellapi.h>
#include <winhttp.h>
#include <memory>
#include <string>
#include <thread>
#include <vector>
#include <algorithm>

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "shell32.lib")

namespace {

constexpr wchar_t kWindowClass[] = L"WarzoneAudioOptimizerWindow";
constexpr int kToggleControl = 1001;
constexpr int kUpdateCheckControl = 1002;
constexpr UINT_PTR kRefreshTimer = 1;
constexpr UINT kUpdateResultMessage = WM_APP + 1;

const COLORREF kBackground = RGB(18, 22, 29);
const COLORREF kPanel = RGB(27, 33, 43);
const COLORREF kText = RGB(235, 240, 246);
const COLORREF kMuted = RGB(148, 160, 175);
const COLORREF kAccent = RGB(76, 178, 255);
const COLORREF kOnline = RGB(70, 210, 145);
const COLORREF kWarning = RGB(246, 187, 78);

// Ventana de gracia tras activar: los primeros ~1.5s pueden traer un
// resync/insured-frame normal mientras el ring buffer llega a régimen
// estable (por ejemplo, el primer "tick" del reloj del dispositivo antes de
// que el hilo de captura se haya programado). No es un problema real, así
// que no debe teñir el estado de "REVISAR BUFFER" para el resto de la
// sesión -- comparamos contra una línea base tomada al final de esa ventana.
constexpr ULONGLONG kHealthGraceMs = 1500;

struct AppState {
    std::unique_ptr<audiopt::WasapiLowLatencyEngine> engine;
    HWND toggleButton = nullptr;
    HWND updateButton = nullptr;
    bool active = false;
    ULONGLONG activatedAtMs = 0;
    bool baselineCaptured = false;
    uint64_t insuredBaseline = 0;
    uint64_t resyncBaseline = 0;
    bool checkingUpdate = false;
};

AppState* getState(HWND window) {
    return reinterpret_cast<AppState*>(GetWindowLongPtrW(window, GWLP_USERDATA));
}

void drawText(HDC dc, const wchar_t* text, int x, int y, int size,
              COLORREF color, bool bold = false) {
    HFONT font = CreateFontW(
        -size, 0, 0, 0, bold ? FW_SEMIBOLD : FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
    const HFONT previous = static_cast<HFONT>(SelectObject(dc, font));
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, color);
    TextOutW(dc, x, y, text, lstrlenW(text));
    SelectObject(dc, previous);
    DeleteObject(font);
}

void drawMetric(HDC dc, const wchar_t* label, const std::wstring& value,
                int x, int y) {
    drawText(dc, label, x, y, 12, kMuted);
    drawText(dc, value.c_str(), x, y + 22, 18, kText, true);
}

// -----------------------------------------------------------------------------
// Chequeo de actualizaciones: descarga docs/version.json desde la página del
// proyecto y compara contra la versión compilada. Manual (no autoinstala
// nada) -- solo informa y, si el usuario quiere, abre la página de descarga.
// -----------------------------------------------------------------------------
struct UpdateCheckResult {
    bool success = false;
    bool updateAvailable = false;
    std::wstring remoteVersion;
    std::wstring notes;
    std::wstring error;
};

std::wstring extractJsonString(const std::wstring& json, const wchar_t* key) {
    const std::wstring needle = std::wstring(L"\"") + key + L"\"";
    size_t pos = json.find(needle);
    if (pos == std::wstring::npos) return L"";
    pos = json.find(L':', pos + needle.size());
    if (pos == std::wstring::npos) return L"";
    pos = json.find(L'"', pos);
    if (pos == std::wstring::npos) return L"";
    const size_t end = json.find(L'"', pos + 1);
    if (end == std::wstring::npos) return L"";
    return json.substr(pos + 1, end - pos - 1);
}

bool isVersionNewer(const std::wstring& remote, const std::wstring& local) {
    auto toParts = [](const std::wstring& v) {
        std::vector<int> nums;
        size_t start = 0;
        while (start <= v.size()) {
            const size_t dot = v.find(L'.', start);
            const std::wstring token = v.substr(start, dot == std::wstring::npos
                ? std::wstring::npos : dot - start);
            nums.push_back(token.empty() ? 0 : _wtoi(token.c_str()));
            if (dot == std::wstring::npos) break;
            start = dot + 1;
        }
        return nums;
    };
    const auto a = toParts(remote);
    const auto b = toParts(local);
    for (size_t i = 0; i < std::max(a.size(), b.size()); ++i) {
        const int av = i < a.size() ? a[i] : 0;
        const int bv = i < b.size() ? b[i] : 0;
        if (av != bv) return av > bv;
    }
    return false;
}

UpdateCheckResult checkForUpdates() {
    UpdateCheckResult result;

    URL_COMPONENTSW components{};
    components.dwStructSize = sizeof(components);
    wchar_t host[256]{};
    wchar_t path[1024]{};
    components.lpszHostName = host;
    components.dwHostNameLength = static_cast<DWORD>(std::size(host));
    components.lpszUrlPath = path;
    components.dwUrlPathLength = static_cast<DWORD>(std::size(path));
    if (!WinHttpCrackUrl(audiopt::kVersionManifestUrl,
                          static_cast<DWORD>(lstrlenW(audiopt::kVersionManifestUrl)),
                          0, &components)) {
        result.error = L"No se pudo interpretar la URL del manifiesto de versión.";
        return result;
    }

    HINTERNET session = WinHttpOpen(L"WarzoneAudioOptimizer-UpdateCheck/1.0",
                                     WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                     WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    HINTERNET connection = session ? WinHttpConnect(session, host, components.nPort, 0) : nullptr;
    HINTERNET request = connection ? WinHttpOpenRequest(
        connection, L"GET", path, nullptr, WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES,
        components.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0) : nullptr;

    std::string raw;
    const bool sent = request &&
        WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                            WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
        WinHttpReceiveResponse(request, nullptr);
    if (sent) {
        DWORD available = 0;
        do {
            available = 0;
            if (!WinHttpQueryDataAvailable(request, &available) || available == 0) break;
            std::vector<char> buffer(available);
            DWORD read = 0;
            if (!WinHttpReadData(request, buffer.data(), available, &read)) break;
            raw.append(buffer.data(), read);
        } while (available > 0);
    }

    if (request) WinHttpCloseHandle(request);
    if (connection) WinHttpCloseHandle(connection);
    if (session) WinHttpCloseHandle(session);

    if (!sent || raw.empty()) {
        result.error = L"No se pudo contactar el servidor de actualizaciones. "
                        L"Revisa tu conexión a internet.";
        return result;
    }

    const std::wstring body(raw.begin(), raw.end()); // JSON esperado en ASCII/UTF-8 simple
    result.remoteVersion = extractJsonString(body, L"version");
    result.notes = extractJsonString(body, L"notes");
    if (result.remoteVersion.empty()) {
        result.error = L"La respuesta del servidor de actualizaciones no tiene el "
                        L"formato esperado.";
        return result;
    }

    result.success = true;
    result.updateAvailable = isVersionNewer(result.remoteVersion, audiopt::kAppVersion);
    return result;
}

void updateToggleButton(AppState& state) {
    SetWindowTextW(state.toggleButton,
                   state.active ? L"DETENER PROCESAMIENTO" : L"ACTIVAR PROCESAMIENTO");
    InvalidateRect(state.toggleButton, nullptr, TRUE);
}

void showInitializationError(HWND window, const std::wstring& error) {
    MessageBoxW(window, error.c_str(), L"Warzone Audio Optimizer", MB_OK | MB_ICONERROR);
}

void toggleEngine(HWND window) {
    AppState* state = getState(window);
    if (!state || !state->engine) return;

    if (state->active) {
        state->engine->stop();
        state->active = false;
        updateToggleButton(*state);
        InvalidateRect(window, nullptr, FALSE);
        return;
    }

    if (!state->engine->start()) {
        MessageBoxW(window, L"No se pudo iniciar el motor de audio.",
                    L"Warzone Audio Optimizer", MB_OK | MB_ICONERROR);
        return;
    }

    state->active = true;
    state->activatedAtMs = GetTickCount64();
    state->baselineCaptured = false;
    updateToggleButton(*state);
    InvalidateRect(window, nullptr, FALSE);
}

void paintWindow(HWND window, HDC dc) {
    AppState* state = getState(window);
    RECT client{};
    GetClientRect(window, &client);

    HBRUSH background = CreateSolidBrush(kBackground);
    FillRect(dc, &client, background);
    DeleteObject(background);

    drawText(dc, L"WARZONE AUDIO OPTIMIZER", 36, 28, 24, kText, true);
    const std::wstring subtitle = std::wstring(L"Post-render audio control  |  v") + audiopt::kAppVersion;
    drawText(dc, subtitle.c_str(), 38, 62, 13, kMuted);

    RECT statusPanel{36, 102, client.right - 36, 166};
    HBRUSH panel = CreateSolidBrush(kPanel);
    FillRect(dc, &statusPanel, panel);
    DeleteObject(panel);

    const COLORREF statusColor = state && state->active ? kOnline : kMuted;
    HBRUSH statusBrush = CreateSolidBrush(statusColor);
    RECT statusDot{56, 124, 68, 136};
    FillRect(dc, &statusDot, statusBrush);
    DeleteObject(statusBrush);
    drawText(dc, state && state->active ? L"PROCESAMIENTO ACTIVO" : L"PROCESAMIENTO DETENIDO",
             82, 116, 16, statusColor, true);
    drawText(dc, state && state->active
                 ? L"El audio del sistema esta pasando por la cadena DSP"
                 : L"El audio del sistema se mantiene sin modificar",
             82, 142, 12, kMuted);

    if (!state || !state->engine) return;

    std::wstring sampleRate = std::to_wstring(state->engine->sampleRate()) + L" Hz";
    std::wstring channels = std::to_wstring(state->engine->numChannels());
    std::wstring underruns = std::to_wstring(state->engine->insuredFrameEvents());
    std::wstring resyncs = std::to_wstring(state->engine->resyncEvents());
    drawMetric(dc, L"FRECUENCIA", sampleRate, 40, 200);
    drawMetric(dc, L"CANALES", channels, 160, 200);
    drawMetric(dc, L"INSURED FRAMES", underruns, 268, 200);
    drawMetric(dc, L"RESYNCS", resyncs, 420, 200);

    // Durante la ventana de gracia tras activar, un resync/insured-frame es
    // normal (arranque del reloj del dispositivo) y no cuenta como problema.
    // Pasada esa ventana, fijamos una línea base una sola vez y comparamos
    // contra ella, para que un evento aislado del arranque no deje el
    // estado en rojo el resto de la sesión.
    const bool inGrace = state->active &&
        (GetTickCount64() - state->activatedAtMs) < kHealthGraceMs;
    if (state->active && !inGrace && !state->baselineCaptured) {
        state->insuredBaseline = state->engine->insuredFrameEvents();
        state->resyncBaseline = state->engine->resyncEvents();
        state->baselineCaptured = true;
    }

    const wchar_t* statusText = L"ESTADO ESTABLE";
    COLORREF healthColor = kOnline;
    if (state->active && inGrace) {
        statusText = L"INICIANDO...";
        healthColor = kAccent;
    } else if (state->active && state->baselineCaptured) {
        const bool healthy =
            (state->engine->insuredFrameEvents() - state->insuredBaseline) <= 1 &&
            (state->engine->resyncEvents() - state->resyncBaseline) == 0;
        statusText = healthy ? L"ESTADO ESTABLE" : L"REVISAR BUFFER";
        healthColor = healthy ? kOnline : kWarning;
    }
    drawText(dc, statusText, 44, 260, 12, healthColor, true);
    const std::wstring modeLine = std::wstring(L"Loopback WASAPI  |  Render ") +
        (state->engine->renderModeExclusive() ? L"EXCLUSIVE" : L"SHARED") +
        L"  |  EQ pasos multibanda + control de motores/explosiones";
    drawText(dc, modeLine.c_str(), 44, 282, 12, kMuted);
}

LRESULT CALLBACK windowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_NCCREATE) {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        SetWindowLongPtrW(window, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(create->lpCreateParams));
    }

    AppState* state = getState(window);
    switch (message) {
        case WM_CREATE:
            state = getState(window);
            state->toggleButton = CreateWindowExW(
                0, L"BUTTON", L"ACTIVAR PROCESAMIENTO",
                WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                36, 326, 548, 54, window,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(kToggleControl)),
                GetModuleHandleW(nullptr), nullptr);
            state->updateButton = CreateWindowExW(
                0, L"BUTTON", L"BUSCAR ACTUALIZACIONES",
                WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                36, 392, 548, 32, window,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(kUpdateCheckControl)),
                GetModuleHandleW(nullptr), nullptr);
            SetTimer(window, kRefreshTimer, 1000, nullptr);
            return 0;

        case WM_COMMAND:
            if (LOWORD(wParam) == kToggleControl && HIWORD(wParam) == BN_CLICKED) {
                toggleEngine(window);
                return 0;
            }
            if (LOWORD(wParam) == kUpdateCheckControl && HIWORD(wParam) == BN_CLICKED) {
                if (state && !state->checkingUpdate) {
                    state->checkingUpdate = true;
                    EnableWindow(state->updateButton, FALSE);
                    SetWindowTextW(state->updateButton, L"BUSCANDO...");
                    std::thread([window]() {
                        const UpdateCheckResult result = checkForUpdates();
                        std::wstring resultText;
                        if (!result.success) {
                            resultText = L"No se pudo buscar actualizaciones.\n\n" + result.error;
                        } else if (result.updateAvailable) {
                            resultText = L"Hay una version nueva disponible: " + result.remoteVersion +
                                       L"\n\n" + result.notes +
                                       L"\n\nQuieres abrir la pagina de descarga?";
                        } else {
                            resultText = L"Ya tienes la ultima version (" +
                                       std::wstring(audiopt::kAppVersion) + L").";
                        }
                        PostMessageW(window, kUpdateResultMessage,
                                     static_cast<WPARAM>(result.success && result.updateAvailable),
                                     reinterpret_cast<LPARAM>(new std::wstring(resultText)));
                    }).detach();
                }
                return 0;
            }
            break;

        case kUpdateResultMessage: {
            std::unique_ptr<std::wstring> resultText(reinterpret_cast<std::wstring*>(lParam));
            if (state) {
                state->checkingUpdate = false;
                EnableWindow(state->updateButton, TRUE);
                SetWindowTextW(state->updateButton, L"BUSCAR ACTUALIZACIONES");
            }
            const bool updateAvailable = wParam != 0;
            if (updateAvailable) {
                const int choice = MessageBoxW(window, resultText->c_str(),
                                                L"Warzone Audio Optimizer",
                                                MB_YESNO | MB_ICONINFORMATION);
                if (choice == IDYES) {
                    ShellExecuteW(window, L"open", audiopt::kDownloadPageUrl,
                                  nullptr, nullptr, SW_SHOWNORMAL);
                }
            } else {
                MessageBoxW(window, resultText->c_str(), L"Warzone Audio Optimizer",
                            MB_OK | MB_ICONINFORMATION);
            }
            return 0;
        }

        case WM_DRAWITEM: {
            auto* draw = reinterpret_cast<DRAWITEMSTRUCT*>(lParam);
            if (draw->CtlID == kUpdateCheckControl) {
                HBRUSH brush = CreateSolidBrush(kPanel);
                FillRect(draw->hDC, &draw->rcItem, brush);
                DeleteObject(brush);
                SetBkMode(draw->hDC, TRANSPARENT);
                SetTextColor(draw->hDC, kAccent);
                HFONT font = CreateFontW(-13, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                                         DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                         CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
                HFONT previous = static_cast<HFONT>(SelectObject(draw->hDC, font));
                wchar_t buttonText[64]{};
                GetWindowTextW(draw->hwndItem, buttonText, static_cast<int>(std::size(buttonText)));
                DrawTextW(draw->hDC, buttonText, -1, &draw->rcItem,
                          DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                SelectObject(draw->hDC, previous);
                DeleteObject(font);
                return TRUE;
            }
            if (draw->CtlID != kToggleControl) break;
            const bool active = state && state->active;
            const COLORREF fillColor = active ? RGB(190, 75, 82) : kAccent;
            HBRUSH brush = CreateSolidBrush(fillColor);
            FillRect(draw->hDC, &draw->rcItem, brush);
            DeleteObject(brush);
            SetBkMode(draw->hDC, TRANSPARENT);
            SetTextColor(draw->hDC, RGB(255, 255, 255));
            HFONT font = CreateFontW(-15, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                                     DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                     CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
            HFONT previous = static_cast<HFONT>(SelectObject(draw->hDC, font));
            DrawTextW(draw->hDC, active ? L"DETENER PROCESAMIENTO" : L"ACTIVAR PROCESAMIENTO",
                      -1, &draw->rcItem, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            SelectObject(draw->hDC, previous);
            DeleteObject(font);
            return TRUE;
        }

        case WM_TIMER:
            InvalidateRect(window, nullptr, FALSE);
            return 0;

        case WM_PAINT: {
            PAINTSTRUCT paint{};
            HDC dc = BeginPaint(window, &paint);
            paintWindow(window, dc);
            EndPaint(window, &paint);
            return 0;
        }

        case WM_DESTROY:
            KillTimer(window, kRefreshTimer);
            if (state && state->engine) state->engine->stop();
            PostQuitMessage(0);
            return 0;
    }

    return DefWindowProcW(window, message, wParam, lParam);
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int showCommand) {
    using namespace audiopt;

    AppState state;
    state.engine = std::make_unique<WasapiLowLatencyEngine>();

    EngineConfig config;
    config.bufferFrames = 64;
    config.sampleRate = 44100;
    config.numChannels = 2;
    config.useLoopbackCapture = true;
    config.captureDeviceName = L"CABLE Input";
    config.renderDeviceName = L"Altavoces (Realtek(R) Audio)";

    std::wstring error;
    if (!state.engine->initialize(config, error)) {
        showInitializationError(nullptr, error);
        return 1;
    }
    state.engine->dspChain().setOutputTrimDb(-3.0f);
    state.engine->dspChain().setBypass(false);

    WNDCLASSEXW windowClass{sizeof(WNDCLASSEXW)};
    windowClass.hInstance = instance;
    windowClass.lpfnWndProc = windowProc;
    windowClass.lpszClassName = kWindowClass;
    windowClass.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    windowClass.hbrBackground = CreateSolidBrush(kBackground);
    windowClass.style = CS_HREDRAW | CS_VREDRAW;
    RegisterClassExW(&windowClass);

    HWND window = CreateWindowExW(
        0, kWindowClass, L"Warzone Audio Optimizer",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        CW_USEDEFAULT, CW_USEDEFAULT, 620, 480,
        nullptr, nullptr, instance, &state);
    if (!window) return 1;

    ShowWindow(window, showCommand);
    UpdateWindow(window);

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }

    return static_cast<int>(message.wParam);
}
