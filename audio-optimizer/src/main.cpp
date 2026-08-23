#include "WasapiLowLatencyEngine.h"
#include "AppVersion.h"
#include "ApoStatus.h"
#include <Windows.h>
#include <windowsx.h>
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
constexpr int kApoControl = 1003;
constexpr int kDeviceComboControl = 1004;
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

// -----------------------------------------------------------------------------
// Sliders de ajuste manual. Son dibujados a mano (no controles nativos) para
// que encajen con el resto de la UI oscura. Cada uno mapea su posición 0..1 a
// un rango de valor real y lo empuja a la cadena DSP en vivo.
// -----------------------------------------------------------------------------
enum SliderId {
    kSliderFootsteps = 0,
    kSliderVehicles,
    kSliderAir,
    kSliderBandStart,
    kSliderBandEnd,
    kSliderStereoWidth,
    kSliderVolume,
    kSliderCount
};

enum SliderFormat {
    kFormatDb = 0,
    kFormatPercent,
    kFormatHz,
};

struct SliderSpec {
    const wchar_t* label;
    const wchar_t* hint;
    // Clave estable en el archivo de ajustes. NO renombrar sin migrar: si
    // cambia, los ajustes guardados del usuario se pierden silenciosamente.
    const wchar_t* settingsKey;
    float minValue;
    float maxValue;
    float defaultValue;
    SliderFormat format;
    // Escala logarítmica: obligatoria para frecuencias. El oído percibe la
    // frecuencia de forma logarítmica, así que un slider lineal gastaría
    // medio recorrido entre 5k y 10k (que apenas se distinguen) y dejaría
    // sin resolución la zona grave, donde cada 100Hz sí importan.
    bool logarithmic;
};

// Nota sobre "MOTORES/EXPLOSIONES": el valor es el umbral del compresor de la
// banda baja, y va al revés que la intuición (más negativo = agacha más). Por
// eso el slider se dibuja invertido: a la derecha = más reducción.
const SliderSpec kSliders[kSliderCount] = {
    {L"REALCE DE PASOS",        L"Sube pasos, recargas y ropa",     L"footstepBoostDb",
      0.0f,  14.0f,  7.0f,  kFormatDb, false},
    {L"REDUCIR MOTORES/BOMBAS", L"Agacha vehiculos y explosiones",  L"vehicleThresholdDb",
    -15.0f, -45.0f, -40.0f, kFormatDb, false},
    {L"REDUCIR BRILLO/CAJAS",   L"Baja el tintineo metalico agudo", L"airGainDb",
      0.0f, -15.0f,  -4.0f, kFormatDb, false},
    {L"BANDA PASOS: EMPIEZA",   L"Debajo = motores y explosiones",  L"lowCrossoverHz",
    200.0f, 1500.0f, 700.0f, kFormatHz, true},
    {L"BANDA PASOS: TERMINA",   L"Encima = brillo, cajas, metal",   L"airCrossoverHz",
   3000.0f, 12000.0f, 5000.0f, kFormatHz, true},
    {L"AMPLITUD ESTEREO",       L"Direccion mas nitida",            L"stereoWidth",
      1.0f,   2.0f,  1.3f,  kFormatPercent, false},
    {L"VOLUMEN DE SALIDA",      L"Ganancia final",                  L"outputTrimDb",
    -12.0f,   6.0f,  0.0f,  kFormatDb, false},
};

// Selector del dispositivo por el que se escucha. Va sobre los sliders
// porque es la decision que condiciona todo lo demas: si el APO no esta en
// el dispositivo correcto, ajustar el sonido no sirve de nada.
constexpr int kDeviceLabelY = 302;
constexpr int kDeviceComboY = 322;
constexpr int kDeviceComboHeight = 30;

constexpr int kSliderX = 44;
constexpr int kSliderWidth = 512;
constexpr int kSliderFirstY = 372;
constexpr int kSliderSpacing = 58;
constexpr int kSliderTrackHeight = 6;
constexpr int kSliderThumbWidth = 12;
constexpr int kSliderThumbHeight = 20;
// Alto de la zona sensible al mouse alrededor del track (más generosa que el
// track dibujado, para que no haya que apuntar al pixel).
constexpr int kSliderHitPadding = 12;

// Los botones van debajo de la última fila de sliders: activar (54px),
// modo APO (38px) y buscar actualizaciones (32px), con sus separaciones.
constexpr int kButtonsY = kSliderFirstY + kSliderCount * kSliderSpacing + 12;
constexpr int kWindowHeight = kButtonsY + 108 + 32 + 58;

struct AppState {
    std::unique_ptr<audiopt::WasapiLowLatencyEngine> engine;
    HWND toggleButton = nullptr;
    HWND updateButton = nullptr;
    HWND apoButton = nullptr;
    HWND deviceCombo = nullptr;
    std::vector<audiopt::RenderDevice> devices;
    bool active = false;
    ULONGLONG activatedAtMs = 0;
    bool baselineCaptured = false;
    uint64_t insuredBaseline = 0;
    uint64_t resyncBaseline = 0;
    bool checkingUpdate = false;
    float sliderValues[kSliderCount]{};
    int draggingSlider = -1;

    // Estado del APO. Cuando esta asociado a un dispositivo, la app NO
    // procesa audio: seria procesarlo dos veces. Pasa a ser su mando.
    audiopt::ApoStatus apo;
};

// Con el APO puesto, el motor local sobra: la app es solo el panel de control.
bool isApoMode(const AppState& state) { return state.apo.attached; }

// Y del track de un slider dado.
int sliderTrackY(int index) {
    return kSliderFirstY + index * kSliderSpacing + 26;
}

// Convierte un valor real a posición normalizada 0..1 dentro de su rango.
float valueToNorm(const SliderSpec& spec, float value) {
    if (spec.logarithmic) {
        // Requiere min/max > 0, garantizado para los sliders de frecuencia.
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

// -----------------------------------------------------------------------------
// Persistencia de ajustes: %APPDATA%\WarzoneAudioOptimizer\settings.ini
// Se usa la API de INI de Win32 en vez de un formato propio para no tener que
// escribir (ni depurar) un parser. Si el archivo no existe o una clave está
// corrupta, cada slider cae a su valor por defecto -- nunca a un valor basura.
// -----------------------------------------------------------------------------
constexpr wchar_t kSettingsSection[] = L"Ajustes";

// Los ajustes viven en ProgramData, no en AppData del usuario, porque el APO
// los lee desde audiodg.exe, que corre como SYSTEM: alli "la carpeta del
// usuario" es el perfil de SYSTEM y jamas veria este archivo.
std::wstring settingsFilePath(bool createDirectory) {
    const std::wstring directory = L"C:\\ProgramData\\WarzoneAudioOptimizer";
    if (createDirectory) {
        // Falla silenciosamente si ya existe, que es el caso normal.
        CreateDirectoryW(directory.c_str(), nullptr);
    }
    return directory + L"\\settings.ini";
}

// Ubicacion anterior (AppData del usuario). Solo se usa para no perder los
// ajustes de quien ya tenia la app configurada.
std::wstring legacySettingsFilePath() {
    wchar_t appData[MAX_PATH]{};
    const DWORD length = GetEnvironmentVariableW(L"APPDATA", appData,
                                                  static_cast<DWORD>(std::size(appData)));
    if (length == 0 || length >= std::size(appData)) return L"";
    return std::wstring(appData) + L"\\WarzoneAudioOptimizer\\settings.ini";
}

// Devuelve false si no se pudo escribir. Importa: un guardado que falla en
// silencio se manifiesta como "los sliders no hacen nada", sin ninguna pista
// de que el problema son los permisos del archivo.
bool saveSettings(const AppState& state) {
    const std::wstring path = settingsFilePath(/*createDirectory*/ true);
    if (path.empty()) return false;

    bool allOk = true;
    for (int i = 0; i < kSliderCount; ++i) {
        wchar_t buffer[64]{};
        swprintf(buffer, std::size(buffer), L"%.4f", state.sliderValues[i]);
        if (!WritePrivateProfileStringW(kSettingsSection, kSliders[i].settingsKey,
                                         buffer, path.c_str())) {
            allOk = false;
        }
    }
    return allOk;
}

// Avisa una sola vez por sesion: repetirlo en cada slider seria insufrible.
void warnIfSettingsUnwritable(HWND window, const AppState& state) {
    static bool warned = false;
    if (warned) return;
    if (saveSettings(state)) return;

    warned = true;
    MessageBoxW(window,
        L"No se pudieron guardar los ajustes.\n\n"
        L"El archivo C:\\ProgramData\\WarzoneAudioOptimizer\\settings.ini "
        L"pertenece al administrador y esta aplicacion no puede escribirlo, "
        L"asi que los cambios de los sliders no llegan al APO.\n\n"
        L"Solucion: vuelve a instalar el modo APO desde el boton de abajo. "
        L"El instalador corrige los permisos.",
        L"Warzone Audio Optimizer", MB_OK | MB_ICONWARNING);
}

// Fecha de ultima modificacion de un archivo; devuelve false si no existe.
bool fileWriteTime(const std::wstring& path, FILETIME& out) {
    if (path.empty()) return false;
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data)) return false;
    out = data.ftLastWriteTime;
    return true;
}

void loadSettings(AppState& state) {
    std::wstring path = settingsFilePath(/*createDirectory*/ false);

    // Al cambiar de ubicacion pueden quedar dos archivos a la vez: el nuevo
    // (que lee el APO) y el antiguo, que versiones previas de la app seguian
    // escribiendo. Gana el MAS RECIENTE, no simplemente el nuevo: si no, un
    // archivo viejo en la ubicacion nueva descartaria en silencio los
    // ajustes que el usuario acaba de hacer.
    const std::wstring legacy = legacySettingsFilePath();
    FILETIME currentTime{}, legacyTime{};
    const bool hasCurrent = fileWriteTime(path, currentTime);
    const bool hasLegacy = fileWriteTime(legacy, legacyTime);

    if (hasLegacy && (!hasCurrent || CompareFileTime(&legacyTime, &currentTime) > 0)) {
        path = legacy;
    }

    for (int i = 0; i < kSliderCount; ++i) {
        const SliderSpec& spec = kSliders[i];
        float value = spec.defaultValue;

        if (!path.empty()) {
            wchar_t buffer[64]{};
            // El sentinel vacío distingue "clave ausente" de "clave presente
            // con valor 0", que es legítimo para varios de estos sliders.
            GetPrivateProfileStringW(kSettingsSection, spec.settingsKey, L"",
                                      buffer, static_cast<DWORD>(std::size(buffer)),
                                      path.c_str());
            if (buffer[0] != L'\0') {
                wchar_t* end = nullptr;
                const double parsed = wcstod(buffer, &end);
                // Solo aceptamos el valor si la cadena entera era numérica.
                if (end && *end == L'\0') {
                    // Clamp por si el rango del slider cambió entre versiones:
                    // un ajuste viejo fuera de rango no debe colarse al DSP.
                    const float low = std::min(spec.minValue, spec.maxValue);
                    const float high = std::max(spec.minValue, spec.maxValue);
                    value = std::clamp(static_cast<float>(parsed), low, high);
                }
            }
        }

        state.sliderValues[i] = value;
    }
}

std::wstring formatSliderValue(const SliderSpec& spec, float value) {
    wchar_t buffer[64]{};
    switch (spec.format) {
        case kFormatPercent:
            swprintf(buffer, std::size(buffer), L"%d%%",
                     static_cast<int>(value * 100.0f + 0.5f));
            break;
        case kFormatHz:
            if (value >= 1000.0f) {
                swprintf(buffer, std::size(buffer), L"%.2f kHz", value / 1000.0f);
            } else {
                swprintf(buffer, std::size(buffer), L"%d Hz",
                         static_cast<int>(value + 0.5f));
            }
            break;
        case kFormatDb:
        default:
            swprintf(buffer, std::size(buffer), L"%+.1f dB", value);
            break;
    }
    return buffer;
}

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

void fillRoundedBar(HDC dc, int left, int top, int right, int bottom, COLORREF color) {
    HBRUSH brush = CreateSolidBrush(color);
    RECT rect{left, top, right, bottom};
    FillRect(dc, &rect, brush);
    DeleteObject(brush);
}

// Empuja el valor de un slider a la cadena DSP. Los setters de la cadena son
// seguros de llamar desde el hilo de UI mientras el audio corre (guardan el
// valor en un atómico que el hilo de audio aplica en el siguiente bloque).
void pushSliderToDsp(AppState& state, int index) {
    // En modo APO el procesamiento no vive aqui: el valor viaja por el
    // archivo de ajustes, que el APO relee en caliente. Escribir tambien en
    // la cadena local seria inofensivo (no esta procesando), pero dejarlo
    // explicito evita confusion al leer el codigo.
    if (isApoMode(state)) return;
    if (!state.engine) return;
    auto& chain = state.engine->dspChain();
    const float value = state.sliderValues[index];
    switch (index) {
        case kSliderFootsteps: chain.setFootstepBoostDb(value); break;
        case kSliderVehicles:  chain.setVehicleThresholdDb(value); break;
        case kSliderAir:       chain.setAirGainDb(value); break;
        case kSliderBandStart: chain.setLowCrossoverHz(value); break;
        case kSliderBandEnd:   chain.setAirCrossoverHz(value); break;
        case kSliderStereoWidth: chain.setStereoWidth(value); break;
        case kSliderVolume:    chain.setOutputTrimDb(value); break;
        default: break;
    }
}

// -----------------------------------------------------------------------------
// Lanzar los scripts del APO. Necesitan permisos de administrador (tocan el
// registro del motor de audio), asi que se lanzan con "runas": Windows pide la
// confirmacion de UAC. La app en si nunca corre elevada -- no lo necesita para
// nada mas, y pedirlo de mas seria peor.
// -----------------------------------------------------------------------------
std::wstring findApoScript(const wchar_t* scriptName) {
    wchar_t exePath[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exePath, static_cast<DWORD>(std::size(exePath)));
    std::wstring dir(exePath);
    const size_t slash = dir.find_last_of(L'\\');
    if (slash != std::wstring::npos) dir.resize(slash);

    // De la instalacion normal a la carpeta de desarrollo (build-gui/..).
    const std::wstring candidates[] = {
        dir + L"\\apo\\install\\" + scriptName,
        dir + L"\\" + scriptName,
        dir + L"\\..\\..\\apo\\install\\" + scriptName,
        dir + L"\\..\\apo\\install\\" + scriptName,
    };
    for (const auto& candidate : candidates) {
        if (GetFileAttributesW(candidate.c_str()) != INVALID_FILE_ATTRIBUTES) {
            return candidate;
        }
    }
    return L"";
}

// Texto de una entrada del selector. El adaptador es imprescindible: sin el,
// tres dispositivos llamados "Altavoces" son indistinguibles.
std::wstring describeDevice(const audiopt::RenderDevice& device) {
    std::wstring text = device.name;
    if (!device.adapter.empty() && device.adapter != device.name) {
        text += L"  ·  " + device.adapter;
    }
    if (device.hasApo)    text += L"   [ACTIVO]";
    else if (device.isDefault) text += L"   (predeterminado)";
    return text;
}

void populateDeviceCombo(AppState& state) {
    if (!state.deviceCombo) return;

    state.devices = audiopt::enumerateRenderDevices();

    SendMessageW(state.deviceCombo, CB_RESETCONTENT, 0, 0);
    int selected = -1;
    for (size_t i = 0; i < state.devices.size(); ++i) {
        const std::wstring text = describeDevice(state.devices[i]);
        SendMessageW(state.deviceCombo, CB_ADDSTRING, 0,
                     reinterpret_cast<LPARAM>(text.c_str()));
        if (state.devices[i].hasApo) selected = static_cast<int>(i);
    }
    if (selected >= 0) {
        SendMessageW(state.deviceCombo, CB_SETCURSEL, selected, 0);
    }
}

bool runApoScript(HWND window, const wchar_t* scriptName,
                  const std::wstring& extraArgs = L"") {
    const std::wstring script = findApoScript(scriptName);
    if (script.empty()) {
        MessageBoxW(window,
                    L"No se encontro el script del APO.\n\n"
                    L"Deberia estar en la carpeta apo\\install junto a la aplicacion.",
                    L"Warzone Audio Optimizer", MB_OK | MB_ICONERROR);
        return false;
    }

    std::wstring params =
        L"-ExecutionPolicy Bypass -NoProfile -File \"" + script + L"\"";
    if (!extraArgs.empty()) params += L" " + extraArgs;

    SHELLEXECUTEINFOW info{};
    info.cbSize = sizeof(info);
    info.fMask = SEE_MASK_NOCLOSEPROCESS;
    info.hwnd = window;
    info.lpVerb = L"runas";          // eleva: el script toca el registro
    info.lpFile = L"powershell.exe";
    info.lpParameters = params.c_str();
    info.nShow = SW_SHOWNORMAL;

    if (!ShellExecuteExW(&info)) {
        // 1223 = el usuario cancelo el dialogo de UAC. No es un error que
        // merezca un mensaje de alarma.
        if (GetLastError() != ERROR_CANCELLED) {
            MessageBoxW(window, L"No se pudo lanzar el script del APO.",
                        L"Warzone Audio Optimizer", MB_OK | MB_ICONERROR);
        }
        return false;
    }

    if (info.hProcess) {
        // Esperamos a que termine para poder releer el estado con el
        // resultado ya aplicado.
        WaitForSingleObject(info.hProcess, INFINITE);
        CloseHandle(info.hProcess);
    }
    return true;
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

        // Track de fondo + porción "llena" hasta el thumb.
        const float norm = valueToNorm(spec, state.sliderValues[i]);
        const int thumbX = kSliderX + static_cast<int>(norm * static_cast<float>(kSliderWidth));
        fillRoundedBar(dc, kSliderX, trackY, kSliderX + kSliderWidth,
                       trackY + kSliderTrackHeight, RGB(38, 46, 58));
        fillRoundedBar(dc, kSliderX, trackY, thumbX, trackY + kSliderTrackHeight, kAccent);

        // Thumb.
        const int thumbLeft = std::clamp(thumbX - kSliderThumbWidth / 2,
                                          kSliderX, kSliderX + kSliderWidth - kSliderThumbWidth);
        const int thumbTop = trackY + kSliderTrackHeight / 2 - kSliderThumbHeight / 2;
        fillRoundedBar(dc, thumbLeft, thumbTop, thumbLeft + kSliderThumbWidth,
                       thumbTop + kSliderThumbHeight,
                       state.draggingSlider == i ? kText : RGB(190, 214, 236));
    }
}

// Devuelve el índice del slider bajo el punto dado, o -1.
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
    pushSliderToDsp(state, index);
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
    if (isApoMode(state)) {
        // Con el APO puesto no hay nada que activar: el audio ya pasa por la
        // cadena dentro de Windows, incluso con la app cerrada.
        SetWindowTextW(state.toggleButton, L"PROCESANDO EN EL MOTOR DE WINDOWS");
        EnableWindow(state.toggleButton, FALSE);
    } else {
        EnableWindow(state.toggleButton, TRUE);
        SetWindowTextW(state.toggleButton,
                       state.active ? L"DETENER PROCESAMIENTO" : L"ACTIVAR PROCESAMIENTO");
    }
    InvalidateRect(state.toggleButton, nullptr, TRUE);
}

void updateApoButton(AppState& state) {
    if (!state.apoButton) return;
    SetWindowTextW(state.apoButton,
                   state.apo.attached ? L"DESINSTALAR MODO APO (SIN DELAY)"
                                      : L"INSTALAR MODO APO (SIN DELAY)");
    InvalidateRect(state.apoButton, nullptr, TRUE);
}

// Relee el estado del APO y adapta la UI. Se llama al arrancar y despues de
// instalar/desinstalar.
void refreshApoStatus(HWND window, AppState& state) {
    const bool wasApo = state.apo.attached;
    state.apo = audiopt::queryApoStatus();

    // Si el APO acaba de aparecer y el motor local estaba procesando, hay que
    // pararlo: dos copias de la misma cadena sobre el mismo audio suena
    // sobreprocesado y devuelve el delay que el APO viene a quitar.
    if (state.apo.attached && !wasApo && state.active) {
        if (state.engine) state.engine->stop();
        state.active = false;
    }

    updateToggleButton(state);
    updateApoButton(state);
    InvalidateRect(window, nullptr, FALSE);
}

void showInitializationError(HWND window, const std::wstring& error) {
    MessageBoxW(window, error.c_str(), L"Warzone Audio Optimizer", MB_OK | MB_ICONERROR);
}

void toggleEngine(HWND window) {
    AppState* state = getState(window);
    if (!state || !state->engine) return;
    // Salvaguarda: con el APO activo este boton esta deshabilitado, pero un
    // atajo de teclado podria llegar igual hasta aqui.
    if (isApoMode(*state)) return;

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

    // El panel de estado dice tres cosas distintas segun el modo, porque son
    // situaciones que el usuario necesita distinguir de un vistazo.
    const bool apoMode = state && isApoMode(*state);
    const bool processing = apoMode || (state && state->active);
    const COLORREF statusColor = processing ? kOnline : kMuted;

    HBRUSH statusBrush = CreateSolidBrush(statusColor);
    RECT statusDot{56, 124, 68, 136};
    FillRect(dc, &statusDot, statusBrush);
    DeleteObject(statusBrush);

    const wchar_t* headline;
    std::wstring detail;
    if (apoMode) {
        headline = L"MODO APO ACTIVO  ·  SIN DELAY";
        detail = L"Procesando dentro de Windows en " + state->apo.deviceName +
                 L"  (" + state->apo.slotName + L")";
    } else if (state && state->active) {
        headline = L"PROCESAMIENTO ACTIVO";
        detail = L"Pasando por la cadena DSP via cable virtual (con algo de delay)";
    } else {
        headline = L"PROCESAMIENTO DETENIDO";
        detail = L"El audio del sistema se mantiene sin modificar";
    }
    drawText(dc, headline, 82, 116, 16, statusColor, true);
    drawText(dc, detail.c_str(), 82, 142, 12, kMuted);

    if (!state) return;

    if (apoMode) {
        // Las metricas del motor local (insured frames, resyncs, modo de
        // render) miden el camino por cable virtual, que aqui no se usa:
        // mostrarlas seria enganoso. Se sustituyen por lo que si aplica.
        drawMetric(dc, L"MODO", L"APO", 40, 200);
        drawMetric(dc, L"LATENCIA AÑADIDA", L"~0 ms", 160, 200);
        drawMetric(dc, L"CABLE VIRTUAL", L"NO USADO", 340, 200);

        drawText(dc, L"AJUSTES EN VIVO", 44, 260, 12, kOnline, true);
        drawText(dc,
                 L"Los sliders se aplican solos en 1 segundo, sin reiniciar nada",
                 44, 282, 12, kMuted);
        drawText(dc, L"DISPOSITIVO POR EL QUE ESCUCHAS", kSliderX, kDeviceLabelY,
                 12, kText, true);
        drawSliders(dc, *state);
        return;
    }

    if (!state->engine) return;

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

    // Sin APO el selector no cambia nada (el motor local usa su propia
    // configuracion), asi que se rotula como informativo para no prometer
    // un control que aqui no existe.
    drawText(dc, L"DISPOSITIVOS DE SALIDA  (elige uno para instalar el APO ahi)",
             kSliderX, kDeviceLabelY, 12, kMuted, true);

    drawSliders(dc, *state);
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
                36, kButtonsY, 548, 54, window,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(kToggleControl)),
                GetModuleHandleW(nullptr), nullptr);
            state->deviceCombo = CreateWindowExW(
                0, L"COMBOBOX", nullptr,
                WS_CHILD | WS_VISIBLE | WS_VSCROLL |
                CBS_DROPDOWNLIST | CBS_OWNERDRAWFIXED | CBS_HASSTRINGS,
                kSliderX, kDeviceComboY, kSliderWidth, kDeviceComboHeight * 9,
                window,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(kDeviceComboControl)),
                GetModuleHandleW(nullptr), nullptr);
            state->apoButton = CreateWindowExW(
                0, L"BUTTON", L"INSTALAR MODO APO (SIN DELAY)",
                WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                36, kButtonsY + 62, 548, 38, window,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(kApoControl)),
                GetModuleHandleW(nullptr), nullptr);
            state->updateButton = CreateWindowExW(
                0, L"BUTTON", L"BUSCAR ACTUALIZACIONES",
                WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                36, kButtonsY + 108, 548, 32, window,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(kUpdateCheckControl)),
                GetModuleHandleW(nullptr), nullptr);
            refreshApoStatus(window, *state);
            populateDeviceCombo(*state);
            SetTimer(window, kRefreshTimer, 1000, nullptr);
            return 0;

        case WM_MEASUREITEM: {
            auto* measure = reinterpret_cast<MEASUREITEMSTRUCT*>(lParam);
            if (measure->CtlID == kDeviceComboControl) {
                measure->itemHeight = 26;
                return TRUE;
            }
            break;
        }

        case WM_LBUTTONDOWN: {
            if (!state) break;
            const int mouseX = GET_X_LPARAM(lParam);
            const int mouseY = GET_Y_LPARAM(lParam);
            const int hit = sliderHitTest(mouseX, mouseY);
            if (hit >= 0) {
                state->draggingSlider = hit;
                SetCapture(window);
                updateSliderFromMouse(*state, hit, mouseX);
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
                // Guardamos al soltar (no en cada WM_MOUSEMOVE, que serían
                // cientos de escrituras a disco por arrastre).
                warnIfSettingsUnwritable(window, *state);
                InvalidateRect(window, nullptr, FALSE);
                return 0;
            }
            break;

        case WM_COMMAND:
            if (LOWORD(wParam) == kToggleControl && HIWORD(wParam) == BN_CLICKED) {
                toggleEngine(window);
                return 0;
            }
            if (LOWORD(wParam) == kDeviceComboControl && HIWORD(wParam) == CBN_SELCHANGE) {
                if (!state) return 0;
                const int selection = static_cast<int>(
                    SendMessageW(state->deviceCombo, CB_GETCURSEL, 0, 0));
                if (selection < 0 || selection >= static_cast<int>(state->devices.size())) {
                    return 0;
                }
                // COPIAS, no referencias: populateDeviceCombo() reconstruye
                // state->devices mas abajo, y una referencia al elemento
                // quedaria colgando justo antes de comprobar el resultado.
                const std::wstring targetGuid = state->devices[selection].guid;
                const std::wstring targetLabel = describeDevice(state->devices[selection]);
                const bool targetHadApo = state->devices[selection].hasApo;

                // Elegir el que ya tiene el APO no es un cambio: no molestamos
                // con UAC para no hacer nada.
                if (targetHadApo) return 0;

                const std::wstring question =
                    (state->apo.attached ? L"Mover el procesamiento a:\n\n    "
                                         : L"Instalar el procesamiento en:\n\n    ") +
                    targetLabel +
                    L"\n\nSe pedira permiso de administrador.";
                if (MessageBoxW(window, question.c_str(), L"Cambiar dispositivo",
                                MB_OKCANCEL | MB_ICONQUESTION) != IDOK) {
                    // Devolver el selector a donde estaba: dejarlo marcando un
                    // dispositivo que no es el activo seria mentir al usuario.
                    populateDeviceCombo(*state);
                    return 0;
                }

                const std::wstring args =
                    L"-DeviceGuid \"" + targetGuid + L"\" -Unattended";
                runApoScript(window, L"install-apo.ps1", args);

                refreshApoStatus(window, *state);
                populateDeviceCombo(*state);

                // Si no llego a moverse, decirlo: el caso tipico es un
                // dispositivo sin slots libres, que el script rechaza a
                // proposito para no apagar un efecto del fabricante.
                if (!state->apo.attached ||
                    _wcsicmp(state->apo.deviceGuid.c_str(), targetGuid.c_str()) != 0) {
                    MessageBoxW(window,
                        L"No se pudo mover el procesamiento a ese dispositivo.\n\n"
                        L"Suele pasar cuando el dispositivo no tiene ningun hueco "
                        L"de efectos libre: instalar ahi apagaria un efecto del "
                        L"fabricante (por ejemplo, el que crea el 7.1 virtual).\n\n"
                        L"Para hacerlo de todas formas, ejecuta install-apo.ps1 "
                        L"a mano y confirma el reemplazo.",
                        L"Cambiar dispositivo", MB_OK | MB_ICONWARNING);
                }
                return 0;
            }
            if (LOWORD(wParam) == kApoControl && HIWORD(wParam) == BN_CLICKED) {
                if (!state) return 0;
                if (state->apo.attached) {
                    runApoScript(window, L"uninstall-apo.ps1");
                } else {
                    const int choice = MessageBoxW(window,
                        L"El modo APO procesa el audio dentro de Windows, sin cable "
                        L"virtual: es lo que elimina el delay.\n\n"
                        L"Para instalarlo hay que desactivar la verificacion de firma "
                        L"de APOs de Windows (DisableProtectedAudioDG). No se toca "
                        L"Secure Boot, pero es un cambio real en una proteccion del "
                        L"sistema, y no se puede garantizar como reacciona un "
                        L"anti-cheat de kernel.\n\n"
                        L"Se abrira una ventana que pedira permisos de administrador.\n\n"
                        L"Continuar?",
                        L"Instalar modo APO", MB_YESNO | MB_ICONWARNING);
                    if (choice != IDYES) return 0;
                    runApoScript(window, L"install-apo.ps1");
                }
                refreshApoStatus(window, *state);
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
            if (draw->CtlID == kDeviceComboControl) {
                if (static_cast<int>(draw->itemID) < 0) return TRUE;

                const bool selected = (draw->itemState & ODS_SELECTED) != 0;
                HBRUSH brush = CreateSolidBrush(selected ? RGB(38, 52, 70) : kPanel);
                FillRect(draw->hDC, &draw->rcItem, brush);
                DeleteObject(brush);

                wchar_t text[256]{};
                SendMessageW(draw->hwndItem, CB_GETLBTEXT, draw->itemID,
                             reinterpret_cast<LPARAM>(text));

                const bool isActive = state &&
                    draw->itemID < state->devices.size() &&
                    state->devices[draw->itemID].hasApo;

                SetBkMode(draw->hDC, TRANSPARENT);
                SetTextColor(draw->hDC, isActive ? kOnline : kText);
                HFONT font = CreateFontW(-12, 0, 0, 0, isActive ? FW_SEMIBOLD : FW_NORMAL,
                                         FALSE, FALSE, FALSE,
                                         DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                         CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
                HFONT previous = static_cast<HFONT>(SelectObject(draw->hDC, font));
                RECT textRect = draw->rcItem;
                textRect.left += 10;
                DrawTextW(draw->hDC, text, -1, &textRect,
                          DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
                SelectObject(draw->hDC, previous);
                DeleteObject(font);
                return TRUE;
            }
            if (draw->CtlID == kApoControl) {
                // Verde cuando el APO esta puesto (es el estado deseable),
                // contorno neutro cuando falta por instalar.
                const bool installed = state && state->apo.attached;
                HBRUSH brush = CreateSolidBrush(installed ? RGB(30, 62, 48) : kPanel);
                FillRect(draw->hDC, &draw->rcItem, brush);
                DeleteObject(brush);
                HBRUSH border = CreateSolidBrush(installed ? kOnline : RGB(58, 68, 84));
                FrameRect(draw->hDC, &draw->rcItem, border);
                DeleteObject(border);
                SetBkMode(draw->hDC, TRANSPARENT);
                SetTextColor(draw->hDC, installed ? kOnline : kText);
                HFONT font = CreateFontW(-13, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                                         DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                         CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
                HFONT previous = static_cast<HFONT>(SelectObject(draw->hDC, font));
                wchar_t buttonText[96]{};
                GetWindowTextW(draw->hwndItem, buttonText, static_cast<int>(std::size(buttonText)));
                DrawTextW(draw->hDC, buttonText, -1, &draw->rcItem,
                          DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                SelectObject(draw->hDC, previous);
                DeleteObject(font);
                return TRUE;
            }
            if (draw->CtlID != kToggleControl) break;
            const bool apoActive = state && isApoMode(*state);
            const bool active = state && state->active;
            const COLORREF fillColor = apoActive ? RGB(38, 46, 58)
                                                 : (active ? RGB(190, 75, 82) : kAccent);
            HBRUSH brush = CreateSolidBrush(fillColor);
            FillRect(draw->hDC, &draw->rcItem, brush);
            DeleteObject(brush);
            SetBkMode(draw->hDC, TRANSPARENT);
            SetTextColor(draw->hDC, apoActive ? kMuted : RGB(255, 255, 255));
            HFONT font = CreateFontW(-15, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                                     DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                     CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
            HFONT previous = static_cast<HFONT>(SelectObject(draw->hDC, font));
            wchar_t toggleText[64]{};
            GetWindowTextW(draw->hwndItem, toggleText, static_cast<int>(std::size(toggleText)));
            DrawTextW(draw->hDC, toggleText, -1, &draw->rcItem,
                      DT_CENTER | DT_VCENTER | DT_SINGLELINE);
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
            if (state) {
                saveSettings(*state);
                if (state->engine) state->engine->stop();
            }
            PostQuitMessage(0);
            return 0;
    }

    return DefWindowProcW(window, message, wParam, lParam);
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int showCommand) {
    using namespace audiopt;

    // COM lo inicializa la propia app: antes lo hacia el constructor del
    // motor de audio, pero en modo APO ese motor no se crea y la enumeracion
    // de dispositivos (que usa WASAPI) se quedaria sin COM.
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    AppState state;

    // El estado del APO se consulta ANTES de tocar el motor local: si el APO
    // esta puesto, la app es solo su panel de control y no necesita ni
    // VB-CABLE ni un motor WASAPI propio. Fallar aqui por un cable virtual
    // que ya no hace falta seria absurdo.
    state.apo = queryApoStatus();

    std::wstring error;
    bool engineReady = false;

    if (!state.apo.attached) {
        // Solo se monta el motor local cuando hace falta. Con el APO puesto,
        // inicializarlo abriria dispositivos de audio para nada -- y si
        // VB-CABLE ya no estuviera, fallaria sin motivo.
        state.engine = std::make_unique<WasapiLowLatencyEngine>();

        EngineConfig config;
        config.bufferFrames = 64;
        config.sampleRate = 44100;
        config.numChannels = 2;
        config.useLoopbackCapture = true;
        config.captureDeviceName = L"CABLE Input";
        config.renderDeviceName = L"Altavoces (Realtek(R) Audio)";

        engineReady = state.engine->initialize(config, error);
        if (!engineReady) {
            showInitializationError(nullptr, error);
            return 1;
        }
        state.engine->dspChain().setBypass(false);
    }

    // Restaura los ajustes guardados (o los valores por defecto si es la
    // primera vez) y empújalos a la cadena para que UI y DSP digan
    // exactamente lo mismo desde el primer frame.
    loadSettings(state);
    for (int i = 0; i < kSliderCount; ++i) {
        pushSliderToDsp(state, i);
    }

    // Reescribe los ajustes en la ubicacion que lee el APO. Sin esto, si los
    // ajustes venian del archivo antiguo, el APO seguiria con los valores por
    // defecto hasta que el usuario tocara un slider -- y la impresion seria
    // que la app "no hace nada".
    saveSettings(state);

    // Log de diagnóstico. Se escribe SIEMPRE, incluso en modo APO donde no
    // hay motor local: saber en que modo arranco la app es justo lo primero
    // que hace falta cuando algo no suena como se espera.
    {
        wchar_t tempPath[MAX_PATH]{};
        GetTempPathW(static_cast<DWORD>(std::size(tempPath)), tempPath);
        const std::wstring logPath = std::wstring(tempPath) + L"WarzoneAudioOptimizer_diag.log";
        HANDLE logFile = CreateFileW(logPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                                      CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (logFile != INVALID_HANDLE_VALUE) {
            std::wstring line = std::wstring(L"version=") + kAppVersion +
                L" modo=" + (state.apo.attached ? L"APO" : L"CABLE-VIRTUAL") +
                L" apoDllRegistrada=" + (state.apo.dllRegistered ? L"si" : L"no") + L"\r\n";
            if (state.apo.attached) {
                line += L"apo: dispositivo=\"" + state.apo.deviceName +
                        L"\" slot=" + state.apo.slotName + L"\r\n";
            }
            if (state.engine) {
                line += std::wstring(L"engine: render=") +
                    (state.engine->renderModeExclusive() ? L"EXCLUSIVE" : L"SHARED") +
                    L" format=" + (state.engine->renderFormatIsFloat() ? L"float32" :
                        (L"pcm" + std::to_wstring(state.engine->renderContainerBits()) + L"bit")) +
                    L" renderBuf=" + std::to_wstring(state.engine->renderBufferFrames()) +
                    L" captureBuf=" + std::to_wstring(state.engine->captureBufferFrames()) +
                    L" sampleRate=" + std::to_wstring(state.engine->sampleRate()) +
                    L" channels=" + std::to_wstring(state.engine->numChannels()) + L"\r\n" +
                    L"shared: " + state.engine->sharedModeDiagnostics() + L"\r\n";
            }
            std::string narrow;
            narrow.reserve(line.size());
            for (wchar_t ch : line) narrow.push_back(static_cast<char>(ch));
            DWORD written = 0;
            WriteFile(logFile, narrow.c_str(), static_cast<DWORD>(narrow.size()), &written, nullptr);
            CloseHandle(logFile);
        }
    }

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
