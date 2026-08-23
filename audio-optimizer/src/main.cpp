// =============================================================================
// Warzone Audio Optimizer
// -----------------------------------------------------------------------------
// Panel de control para el procesamiento de audio del juego.
//
// La app NO procesa audio: escribe la configuracion de Equalizer APO, que es
// quien lo hace dentro del motor de audio de Windows y por tanto sin delay.
// Cada slider se traduce a filtros; Equalizer APO detecta el cambio en el
// archivo y lo aplica al instante.
//
// (Versiones anteriores procesaban aqui mismo, capturando el audio por un
// cable virtual. Funcionaba, pero ese camino anadia unas decenas de ms de
// latencia imposibles de recortar: dos relojes de hardware distintos y el
// periodo del motor compartido contado dos veces.)
// =============================================================================
#include "AppVersion.h"
#include "EqualizerApoConfig.h"

#include <Windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <winhttp.h>
#include <memory>
#include <string>
#include <thread>
#include <vector>
#include <algorithm>
#include <cmath>

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

// -----------------------------------------------------------------------------
// Sliders. Dibujados a mano para que encajen con la UI oscura.
// -----------------------------------------------------------------------------
enum SliderId {
    kSliderFootsteps = 0,
    kSliderGunshots,
    kSliderVehicles,
    kSliderAir,
    kSliderBandStart,
    kSliderBandEnd,
    kSliderVolume,
    kSliderCount
};

enum SliderFormat { kFormatDb = 0, kFormatHz };

struct SliderSpec {
    const wchar_t* label;
    const wchar_t* hint;
    // Clave en el archivo de ajustes. NO renombrar: los ajustes guardados
    // del usuario se perderian en silencio.
    const wchar_t* settingsKey;
    float minValue;
    float maxValue;
    float defaultValue;
    SliderFormat format;
    // Escala logaritmica, obligatoria para frecuencias: el oido las percibe
    // asi, y en escala lineal medio recorrido se gastaria entre 7k y 12k.
    bool logarithmic;
};

const SliderSpec kSliders[kSliderCount] = {
    {L"REALCE DE PASOS",        L"Sube pasos, recargas y ropa",     L"footstepBoostDb",
      0.0f,  14.0f,  8.0f,  kFormatDb, false},
    {L"REDUCIR DISPAROS",       L"Baja el cuerpo del arma (700-1500 Hz)", L"gunshotCutDb",
      0.0f, -12.0f,  -3.0f, kFormatDb, false},
    {L"REDUCIR MOTORES/BOMBAS", L"Agacha vehiculos y explosiones",  L"vehicleThresholdDb",
    -15.0f, -45.0f, -40.0f, kFormatDb, false},
    {L"REDUCIR BRILLO/CAJAS",   L"Baja el tintineo metalico agudo", L"airGainDb",
      0.0f, -15.0f,  -4.0f, kFormatDb, false},
    {L"BANDA PASOS: EMPIEZA",   L"Debajo = motores y explosiones",  L"lowCrossoverHz",
    200.0f, 1500.0f, 700.0f, kFormatHz, true},
    {L"BANDA PASOS: TERMINA",   L"Encima = brillo, cajas, metal",   L"airCrossoverHz",
   3000.0f, 12000.0f, 5000.0f, kFormatHz, true},
    {L"VOLUMEN DE SALIDA",      L"Ganancia final",                  L"outputTrimDb",
    -12.0f,   6.0f,  0.0f,  kFormatDb, false},
};

// Interruptor de encendido, entre el estado y los sliders: es lo primero
// que se busca para comparar como suena con y sin procesamiento.
constexpr int kToggleY = 182;
constexpr int kToggleHeight = 44;

constexpr int kSliderX = 44;
constexpr int kSliderWidth = 512;
constexpr int kSliderFirstY = 262;
constexpr int kSliderSpacing = 58;
constexpr int kSliderTrackHeight = 6;
constexpr int kSliderThumbWidth = 12;
constexpr int kSliderThumbHeight = 20;
// Zona sensible al mouse mas generosa que el track dibujado, para no tener
// que apuntar al pixel.
constexpr int kSliderHitPadding = 12;

constexpr int kButtonsY = kSliderFirstY + kSliderCount * kSliderSpacing + 16;
constexpr int kWindowHeight = kButtonsY + 32 + 64;

struct AppState {
    HWND toggleButton = nullptr;
    HWND updateButton = nullptr;
    bool checkingUpdate = false;
    float sliderValues[kSliderCount]{};
    int draggingSlider = -1;

    // Equalizer APO es el motor: la app solo escribe su configuracion.
    bool equalizerApoReady = false;

    // Apagado = se escribe una configuracion vacia, asi que el audio pasa
    // sin tocar. Sirve para comparar A/B sin desinstalar nada.
    bool enabled = true;
};

AppState* getState(HWND window) {
    return reinterpret_cast<AppState*>(GetWindowLongPtrW(window, GWLP_USERDATA));
}

int sliderTrackY(int index) {
    return kSliderFirstY + index * kSliderSpacing + 26;
}

float valueToNorm(const SliderSpec& spec, float value) {
    if (spec.logarithmic) {
        const float ratio = spec.maxValue / spec.minValue;
        if (ratio <= 1.0f) return 0.0f;
        return std::clamp(std::log(value / spec.minValue) / std::log(ratio), 0.0f, 1.0f);
    }
    const float span = spec.maxValue - spec.minValue;
    if (span == 0.0f) return 0.0f;
    return std::clamp((value - spec.minValue) / span, 0.0f, 1.0f);
}

float normToValue(const SliderSpec& spec, float norm) {
    const float clamped = std::clamp(norm, 0.0f, 1.0f);
    if (spec.logarithmic) {
        return spec.minValue * std::pow(spec.maxValue / spec.minValue, clamped);
    }
    return spec.minValue + clamped * (spec.maxValue - spec.minValue);
}

std::wstring formatSliderValue(const SliderSpec& spec, float value) {
    wchar_t buffer[64]{};
    if (spec.format == kFormatHz) {
        if (value >= 1000.0f) {
            swprintf(buffer, std::size(buffer), L"%.2f kHz", value / 1000.0f);
        } else {
            swprintf(buffer, std::size(buffer), L"%d Hz", static_cast<int>(value + 0.5f));
        }
    } else {
        swprintf(buffer, std::size(buffer), L"%+.1f dB", value);
    }
    return buffer;
}

// -----------------------------------------------------------------------------
// Ajustes. Viven en ProgramData y no en AppData por herencia de cuando los
// leia un APO propio desde audiodg.exe (proceso de SYSTEM, que no ve el
// AppData del usuario). Se mantiene ahi para no perder los ajustes de quien
// ya tuviera la app configurada.
// -----------------------------------------------------------------------------
constexpr wchar_t kSettingsSection[] = L"Ajustes";

std::wstring settingsFilePath(bool createDirectory) {
    const std::wstring directory = L"C:\\ProgramData\\WarzoneAudioOptimizer";
    if (createDirectory) CreateDirectoryW(directory.c_str(), nullptr);
    return directory + L"\\settings.ini";
}

void loadSettings(AppState& state) {
    const std::wstring path = settingsFilePath(false);
    for (int i = 0; i < kSliderCount; ++i) {
        const SliderSpec& spec = kSliders[i];
        float value = spec.defaultValue;

        wchar_t buffer[64]{};
        // El sentinel vacio distingue "clave ausente" de "clave con valor 0",
        // que es legitimo para varios de estos sliders.
        GetPrivateProfileStringW(kSettingsSection, spec.settingsKey, L"", buffer,
                                  static_cast<DWORD>(std::size(buffer)), path.c_str());
        if (buffer[0] != L'\0') {
            wchar_t* end = nullptr;
            const double parsed = wcstod(buffer, &end);
            if (end && *end == L'\0') {
                // Clamp por si el rango cambio entre versiones: un ajuste
                // viejo fuera de rango no debe colarse.
                const float low = std::min(spec.minValue, spec.maxValue);
                const float high = std::max(spec.minValue, spec.maxValue);
                value = std::clamp(static_cast<float>(parsed), low, high);
            }
        }
        state.sliderValues[i] = value;
    }

    // Por defecto encendido: es lo que espera quien abre la app.
    state.enabled = GetPrivateProfileIntW(kSettingsSection, L"enabled", 1,
                                           path.c_str()) != 0;
}

// Escribe los ajustes y, si Equalizer APO esta instalado, su configuracion.
// Devuelve false si no se pudo escribir la configuracion de Equalizer APO,
// que es lo unico que afecta al sonido.
bool applySettings(const AppState& state) {
    const std::wstring path = settingsFilePath(true);
    for (int i = 0; i < kSliderCount; ++i) {
        wchar_t buffer[64]{};
        swprintf(buffer, std::size(buffer), L"%.4f", state.sliderValues[i]);
        WritePrivateProfileStringW(kSettingsSection, kSliders[i].settingsKey,
                                    buffer, path.c_str());
    }
    // Windows cachea los archivos INI por proceso; esta llamada lo vuelca.
    WritePrivateProfileStringW(nullptr, nullptr, nullptr, path.c_str());

    // El estado del interruptor tambien se guarda: al reabrir la app debe
    // encontrarse como se dejo.
    WritePrivateProfileStringW(kSettingsSection, L"enabled",
                                state.enabled ? L"1" : L"0", path.c_str());

    if (!state.equalizerApoReady) return false;

    if (!state.enabled) {
        // Configuracion vacia: Equalizer APO sigue en la cadena pero no
        // aplica nada, asi que el audio pasa igual que sin la app.
        return audiopt::writeEqualizerApoConfig(
            "# Warzone Audio Optimizer -- procesamiento APAGADO\r\n"
            "# El audio pasa sin modificar. Vuelve a encenderlo desde la app.\r\n");
    }

    return audiopt::writeEqualizerApoConfig(audiopt::buildEqualizerApoConfig(
        state.sliderValues[kSliderFootsteps],
        state.sliderValues[kSliderGunshots],
        state.sliderValues[kSliderVehicles],
        state.sliderValues[kSliderAir],
        state.sliderValues[kSliderBandStart],
        state.sliderValues[kSliderBandEnd],
        /*stereoWidth (sin efecto en Equalizer APO)*/ 1.0f,
        state.sliderValues[kSliderVolume]));
}

// Avisa una sola vez por sesion: repetirlo en cada slider seria insufrible.
void warnIfCannotApply(HWND window, const AppState& state) {
    static bool warned = false;
    if (warned || !state.equalizerApoReady) return;
    if (applySettings(state)) return;

    warned = true;
    MessageBoxW(window,
        L"No se pudo escribir la configuracion de Equalizer APO.\n\n"
        L"El archivo esta en:\n"
        L"C:\\Program Files\\EqualizerAPO\\config\\config.txt\n\n"
        L"Esta aplicacion necesita permiso de escritura sobre esa carpeta. "
        L"Sin el, los sliders no tendran efecto.",
        L"Warzone Audio Optimizer", MB_OK | MB_ICONWARNING);
}

// -----------------------------------------------------------------------------
// Dibujo
// -----------------------------------------------------------------------------
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

void drawTextRight(HDC dc, const wchar_t* text, int right, int y, int size,
                   COLORREF color, bool bold = false) {
    HFONT font = CreateFontW(
        -size, 0, 0, 0, bold ? FW_SEMIBOLD : FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
    const HFONT previous = static_cast<HFONT>(SelectObject(dc, font));
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, color);
    RECT bounds{right - 200, y, right, y + size + 8};
    DrawTextW(dc, text, -1, &bounds, DT_RIGHT | DT_TOP | DT_SINGLELINE);
    SelectObject(dc, previous);
    DeleteObject(font);
}

void fillBar(HDC dc, int left, int top, int right, int bottom, COLORREF color) {
    HBRUSH brush = CreateSolidBrush(color);
    RECT rect{left, top, right, bottom};
    FillRect(dc, &rect, brush);
    DeleteObject(brush);
}

void drawSliders(HDC dc, const AppState& state) {
    for (int i = 0; i < kSliderCount; ++i) {
        const SliderSpec& spec = kSliders[i];
        const int labelY = kSliderFirstY + i * kSliderSpacing;
        const int trackY = sliderTrackY(i);

        drawText(dc, spec.label, kSliderX, labelY, 12, kText, true);
        drawTextRight(dc, formatSliderValue(spec, state.sliderValues[i]).c_str(),
                      kSliderX + kSliderWidth, labelY - 1, 13, kAccent, true);
        drawText(dc, spec.hint, kSliderX, labelY + 14, 11, kMuted);

        const float norm = valueToNorm(spec, state.sliderValues[i]);
        const int thumbX = kSliderX + static_cast<int>(norm * static_cast<float>(kSliderWidth));
        fillBar(dc, kSliderX, trackY, kSliderX + kSliderWidth,
                trackY + kSliderTrackHeight, RGB(38, 46, 58));
        fillBar(dc, kSliderX, trackY, thumbX, trackY + kSliderTrackHeight, kAccent);

        const int thumbLeft = std::clamp(thumbX - kSliderThumbWidth / 2,
                                          kSliderX, kSliderX + kSliderWidth - kSliderThumbWidth);
        const int thumbTop = trackY + kSliderTrackHeight / 2 - kSliderThumbHeight / 2;
        fillBar(dc, thumbLeft, thumbTop, thumbLeft + kSliderThumbWidth,
                thumbTop + kSliderThumbHeight,
                state.draggingSlider == i ? kText : RGB(190, 214, 236));
    }
}

int sliderHitTest(int x, int y) {
    for (int i = 0; i < kSliderCount; ++i) {
        const int trackY = sliderTrackY(i);
        if (x >= kSliderX - kSliderThumbWidth && x <= kSliderX + kSliderWidth + kSliderThumbWidth &&
            y >= trackY - kSliderHitPadding &&
            y <= trackY + kSliderTrackHeight + kSliderHitPadding) {
            return i;
        }
    }
    return -1;
}

void updateSliderFromMouse(AppState& state, int index, int mouseX) {
    const float norm = static_cast<float>(mouseX - kSliderX) / static_cast<float>(kSliderWidth);
    state.sliderValues[index] = normToValue(kSliders[index], norm);
}

void paintWindow(HWND window, HDC dc) {
    AppState* state = getState(window);
    RECT client{};
    GetClientRect(window, &client);

    HBRUSH background = CreateSolidBrush(kBackground);
    FillRect(dc, &client, background);
    DeleteObject(background);

    drawText(dc, L"WARZONE AUDIO OPTIMIZER", 36, 28, 24, kText, true);
    const std::wstring subtitle = std::wstring(L"v") + audiopt::kAppVersion;
    drawText(dc, subtitle.c_str(), 38, 62, 13, kMuted);

    RECT statusPanel{36, 102, client.right - 36, 166};
    HBRUSH panel = CreateSolidBrush(kPanel);
    FillRect(dc, &statusPanel, panel);
    DeleteObject(panel);

    // Solo hay dos estados: Equalizer APO instalado (los sliders mandan) o
    // no instalado (la app no puede hacer nada).
    const bool ready = state && state->equalizerApoReady;
    const COLORREF statusColor = ready ? kOnline : kWarning;

    HBRUSH statusBrush = CreateSolidBrush(statusColor);
    RECT statusDot{56, 124, 68, 136};
    FillRect(dc, &statusDot, statusBrush);
    DeleteObject(statusBrush);

    if (ready) {
        drawText(dc, L"ACTIVO  \u00b7  SIN DELAY", 82, 116, 16, statusColor, true);
        drawText(dc, L"Procesando dentro de Windows a traves de Equalizer APO",
                 82, 142, 12, kMuted);
    } else {
        drawText(dc, L"EQUALIZER APO NO ENCONTRADO", 82, 116, 16, statusColor, true);
        drawText(dc, L"Instalalo y elige tu dispositivo de salida para que esto funcione",
                 82, 142, 12, kMuted);
    }

    if (!state) return;

    drawText(dc, state->enabled
                 ? L"Mueve un slider y escucha: los cambios entran al instante"
                 : L"Procesamiento apagado: el audio pasa sin modificar",
             44, 236, 11, kMuted);

    drawSliders(dc, *state);
}

// -----------------------------------------------------------------------------
// Chequeo de actualizaciones: descarga docs/version.json y compara. Manual,
// no autoinstala nada: solo informa y abre la pagina si el usuario quiere.
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
        result.error = L"No se pudo interpretar la URL del manifiesto de version.";
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
                        L"Revisa tu conexion a internet.";
        return result;
    }

    const std::wstring body(raw.begin(), raw.end());
    result.remoteVersion = extractJsonString(body, L"version");
    result.notes = extractJsonString(body, L"notes");
    if (result.remoteVersion.empty()) {
        result.error = L"La respuesta del servidor no tiene el formato esperado.";
        return result;
    }

    result.success = true;
    result.updateAvailable = isVersionNewer(result.remoteVersion, audiopt::kAppVersion);
    return result;
}

// -----------------------------------------------------------------------------
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
                0, L"BUTTON", L"", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                kSliderX, kToggleY, kSliderWidth, kToggleHeight, window,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(kToggleControl)),
                GetModuleHandleW(nullptr), nullptr);
            state->updateButton = CreateWindowExW(
                0, L"BUTTON", L"BUSCAR ACTUALIZACIONES",
                WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                36, kButtonsY, 548, 32, window,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(kUpdateCheckControl)),
                GetModuleHandleW(nullptr), nullptr);
            SetTimer(window, kRefreshTimer, 2000, nullptr);
            return 0;

        case WM_LBUTTONDOWN: {
            if (!state) break;
            const int hit = sliderHitTest(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
            if (hit >= 0) {
                state->draggingSlider = hit;
                SetCapture(window);
                updateSliderFromMouse(*state, hit, GET_X_LPARAM(lParam));
                InvalidateRect(window, nullptr, FALSE);
                return 0;
            }
            break;
        }

        case WM_MOUSEMOVE:
            if (state && state->draggingSlider >= 0) {
                updateSliderFromMouse(*state, state->draggingSlider, GET_X_LPARAM(lParam));
                InvalidateRect(window, nullptr, FALSE);
                return 0;
            }
            break;

        case WM_LBUTTONUP:
            if (state && state->draggingSlider >= 0) {
                state->draggingSlider = -1;
                ReleaseCapture();
                // Se aplica al soltar, no en cada movimiento: serian cientos
                // de escrituras a disco por arrastre.
                warnIfCannotApply(window, *state);
                applySettings(*state);
                InvalidateRect(window, nullptr, FALSE);
                return 0;
            }
            break;

        case WM_COMMAND:
            if (LOWORD(wParam) == kToggleControl && HIWORD(wParam) == BN_CLICKED) {
                if (state) {
                    state->enabled = !state->enabled;
                    applySettings(*state);
                    InvalidateRect(window, nullptr, FALSE);
                }
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
            if (wParam != 0) {
                if (MessageBoxW(window, resultText->c_str(), L"Warzone Audio Optimizer",
                                MB_YESNO | MB_ICONINFORMATION) == IDYES) {
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

            if (draw->CtlID == kToggleControl) {
                const bool on = state && state->enabled;
                // Verde solido encendido, gris apagado: el estado tiene que
                // leerse de un vistazo mientras se compara a oido.
                HBRUSH fill = CreateSolidBrush(on ? RGB(32, 96, 68) : RGB(44, 52, 64));
                FillRect(draw->hDC, &draw->rcItem, fill);
                DeleteObject(fill);
                HBRUSH border = CreateSolidBrush(on ? kOnline : RGB(70, 80, 96));
                FrameRect(draw->hDC, &draw->rcItem, border);
                DeleteObject(border);

                SetBkMode(draw->hDC, TRANSPARENT);
                SetTextColor(draw->hDC, on ? kOnline : kMuted);
                HFONT font = CreateFontW(-15, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                                         DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                         CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
                HFONT previous = static_cast<HFONT>(SelectObject(draw->hDC, font));
                DrawTextW(draw->hDC,
                          on ? L"PROCESAMIENTO ENCENDIDO   (clic para apagar)"
                             : L"PROCESAMIENTO APAGADO   (clic para encender)",
                          -1, &draw->rcItem, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                SelectObject(draw->hDC, previous);
                DeleteObject(font);
                return TRUE;
            }

            if (draw->CtlID != kUpdateCheckControl) break;
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

        case WM_TIMER:
            // Equalizer APO puede instalarse o desinstalarse con la app
            // abierta: se relee para no mostrar un estado obsoleto.
            if (state) {
                const bool ready = audiopt::isEqualizerApoInstalled();
                if (ready != state->equalizerApoReady) {
                    state->equalizerApoReady = ready;
                    if (ready) applySettings(*state);
                    InvalidateRect(window, nullptr, FALSE);
                }
            }
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
            if (state) applySettings(*state);
            PostQuitMessage(0);
            return 0;
    }

    return DefWindowProcW(window, message, wParam, lParam);
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int showCommand) {
    AppState state;
    state.equalizerApoReady = audiopt::isEqualizerApoInstalled();

    loadSettings(state);
    applySettings(state);

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
        CW_USEDEFAULT, CW_USEDEFAULT, 620, kWindowHeight,
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
