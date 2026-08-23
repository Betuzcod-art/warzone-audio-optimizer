#include "WarzoneApo.h"

#include <mmreg.h>
#include <ks.h>
#include <ksmedia.h>
#include <shlobj.h>
#include <string>
#include <new>
#include <stdexcept>
#include <cstdarg>
#include <cstdio>

namespace warzoneapo {

namespace {

// -----------------------------------------------------------------------------
// Log de diagnostico del APO.
//
// Un APO que no carga es invisible: Windows no avisa, no hay error, y desde
// fuera es indistinguible de "el audio no pasa por aqui". Este log es la unica
// forma de saber si el motor llego a instanciarnos y con que formato.
//
// Se escribe en ProgramData porque audiodg.exe corre como SYSTEM: su %TEMP%
// no es el del usuario, y hace falta un sitio que ambos puedan leer.
//
// NUNCA se llama desde APOProcess: abrir un archivo en el camino de tiempo
// real es justo lo que no se puede hacer.
// -----------------------------------------------------------------------------
void apoLog(const wchar_t* format, ...) {
    wchar_t message[512]{};
    va_list args;
    va_start(args, format);
    _vsnwprintf_s(message, std::size(message), _TRUNCATE, format, args);
    va_end(args);

    const wchar_t* dir = L"C:\\ProgramData\\WarzoneAudioOptimizer";
    CreateDirectoryW(dir, nullptr);
    const std::wstring path = std::wstring(dir) + L"\\apo.log";

    HANDLE file = CreateFileW(path.c_str(), FILE_APPEND_DATA,
                              FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                              OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;

    SYSTEMTIME now{};
    GetLocalTime(&now);
    wchar_t line[640]{};
    _snwprintf_s(line, std::size(line), _TRUNCATE, L"[%02d:%02d:%02d] %s\r\n",
                 now.wHour, now.wMinute, now.wSecond, message);

    std::string narrow;
    for (const wchar_t* p = line; *p; ++p) narrow.push_back(static_cast<char>(*p));

    DWORD written = 0;
    SetFilePointer(file, 0, nullptr, FILE_END);
    WriteFile(file, narrow.c_str(), static_cast<DWORD>(narrow.size()), &written, nullptr);
    CloseHandle(file);
}

// Propiedades que Windows consulta para decidir cómo encajar el APO en el
// grafo. Una entrada/una salida, procesamiento in-place, y los tres "must
// match" por defecto (mismo sample rate, canales y bits en entrada y salida:
// no convertimos formato, solo procesamos).
const APO_REG_PROPERTIES kRegProperties = {
    CLSID_WarzoneApoMfx,
    // Sin SAMPLESPERFRAME_MUST_MATCH: ver la nota en install-apo.ps1. Debe
    // coincidir con los Flags que el instalador escribe en el registro.
    static_cast<APO_FLAG>(APO_FLAG_INPLACE |
                          APO_FLAG_FRAMESPERSECOND_MUST_MATCH |
                          APO_FLAG_BITSPERSAMPLE_MUST_MATCH),
    L"Warzone Audio Optimizer",
    L"Warzone Audio Optimizer",
    1, 0,          // versión mayor / menor
    1, 1,          // conexiones de entrada mín / máx
    1, 1,          // conexiones de salida mín / máx
    0xFFFFFFFF,    // instancias ilimitadas
    1,             // número de interfaces APO listadas abajo
    { __uuidof(IAudioProcessingObject) },
};

// Ruta del archivo de ajustes que escribe la app de escritorio. Compartir el
// mismo archivo permite que la UI existente siga siendo el panel de control
// del APO, sin duplicar interfaz.
//
// TIENE QUE SER ProgramData, no AppData: este codigo se ejecuta dentro de
// audiodg.exe, que corre como SYSTEM. Pedir "la carpeta del usuario" aqui
// devuelve la del perfil de SYSTEM
// (C:\Windows\System32\config\systemprofile\...), donde el usuario no escribe
// nunca -- el APO no veria jamas los ajustes y los sliders no harian nada.
std::wstring settingsPath() {
    return L"C:\\ProgramData\\WarzoneAudioOptimizer\\settings.ini";
}

float readSetting(const std::wstring& path, const wchar_t* key, float fallback) {
    if (path.empty()) return fallback;
    wchar_t buffer[64]{};
    GetPrivateProfileStringW(L"Ajustes", key, L"", buffer,
                              static_cast<DWORD>(std::size(buffer)), path.c_str());
    if (buffer[0] == L'\0') return fallback;
    wchar_t* end = nullptr;
    const double parsed = wcstod(buffer, &end);
    if (!end || *end != L'\0') return fallback;
    return static_cast<float>(parsed);
}

// Extrae el WAVEFORMATEX de un IAudioMediaType, o nullptr si no se puede.
const WAVEFORMATEX* waveFormatOf(IAudioMediaType* mediaType) {
    if (!mediaType) return nullptr;
    return mediaType->GetAudioFormat();
}

bool isFloat32(const WAVEFORMATEX* format) {
    if (!format) return false;
    if (format->wBitsPerSample != 32) return false;
    if (format->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) return true;
    if (format->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
        format->cbSize >= sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)) {
        const auto* ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(format);
        return IsEqualGUID(ext->SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT) != 0;
    }
    return false;
}

} // namespace

WarzoneApoMfx::WarzoneApoMfx() {
    apoLog(L"APO instanciado por el motor de audio");
}

WarzoneApoMfx::~WarzoneApoMfx() {
    // Si el motor destruye el APO sin llamar a UnlockForProcess, el hilo
    // vigilante seguiria vivo apuntando a un objeto liberado.
    stopSettingsWatcher();
}

// -----------------------------------------------------------------------------
// Vigilancia del archivo de ajustes
// -----------------------------------------------------------------------------
void WarzoneApoMfx::startSettingsWatcher() {
    if (watcherRunning_.load(std::memory_order_relaxed)) return;

    // Evento manual para poder cortar la espera al instante en vez de tener
    // que agotar el intervalo de sondeo al cerrar.
    watcherWakeup_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    watcherRunning_.store(true, std::memory_order_relaxed);

    settingsWatcher_ = std::thread([this]() {
        const std::wstring path = settingsPath();
        FILETIME lastWrite{};

        auto readTimestamp = [&](FILETIME& out) -> bool {
            WIN32_FILE_ATTRIBUTE_DATA data{};
            if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data)) return false;
            out = data.ftLastWriteTime;
            return true;
        };

        readTimestamp(lastWrite);

        while (watcherRunning_.load(std::memory_order_relaxed)) {
            // 400ms: lo bastante rápido para que mover un slider se sienta
            // inmediato, y lo bastante lento para que mirar un archivo no
            // suponga carga apreciable.
            if (WaitForSingleObject(watcherWakeup_, 400) == WAIT_OBJECT_0) break;

            FILETIME current{};
            if (!readTimestamp(current)) continue;
            if (CompareFileTime(&current, &lastWrite) == 0) continue;

            lastWrite = current;
            loadUserSettings();
            apoLog(L"Ajustes recargados: pasos=%.1fdB motores=%.1fdB brillo=%.1fdB",
                   chain_.footstepBoostDb(), chain_.vehicleThresholdDb(),
                   chain_.airGainDb());
        }
    });
}

void WarzoneApoMfx::stopSettingsWatcher() {
    if (!watcherRunning_.exchange(false)) return;
    if (watcherWakeup_) SetEvent(watcherWakeup_);
    if (settingsWatcher_.joinable()) settingsWatcher_.join();
    if (watcherWakeup_) {
        CloseHandle(watcherWakeup_);
        watcherWakeup_ = nullptr;
    }
}

// -----------------------------------------------------------------------------
// IUnknown
// -----------------------------------------------------------------------------
STDMETHODIMP WarzoneApoMfx::QueryInterface(REFIID riid, void** ppv) {
    if (!ppv) return E_POINTER;
    *ppv = nullptr;

    if (IsEqualIID(riid, __uuidof(IUnknown))) {
        *ppv = static_cast<IAudioProcessingObject*>(this);
    } else if (IsEqualIID(riid, __uuidof(IAudioProcessingObject))) {
        *ppv = static_cast<IAudioProcessingObject*>(this);
    } else if (IsEqualIID(riid, __uuidof(IAudioProcessingObjectConfiguration))) {
        *ppv = static_cast<IAudioProcessingObjectConfiguration*>(this);
    } else if (IsEqualIID(riid, __uuidof(IAudioProcessingObjectRT))) {
        *ppv = static_cast<IAudioProcessingObjectRT*>(this);
    } else if (IsEqualIID(riid, __uuidof(IAudioSystemEffects))) {
        *ppv = static_cast<IAudioSystemEffects*>(this);
    } else if (IsEqualIID(riid, __uuidof(IAudioSystemEffects2))) {
        *ppv = static_cast<IAudioSystemEffects2*>(this);
    } else {
        return E_NOINTERFACE;
    }

    AddRef();
    return S_OK;
}

STDMETHODIMP_(ULONG) WarzoneApoMfx::AddRef() {
    return refCount_.fetch_add(1, std::memory_order_relaxed) + 1;
}

STDMETHODIMP_(ULONG) WarzoneApoMfx::Release() {
    const ULONG remaining = refCount_.fetch_sub(1, std::memory_order_acq_rel) - 1;
    if (remaining == 0) delete this;
    return remaining;
}

// -----------------------------------------------------------------------------
// IAudioProcessingObject
// -----------------------------------------------------------------------------
STDMETHODIMP WarzoneApoMfx::Reset() {
    // "Reset" en audio significa olvidar la historia de la señal tras una
    // discontinuidad, NO reinicializarse. Aquí es solo limpiar filtros y
    // detectores: el motor puede llamar a este método con el stream activo,
    // así que no se reserva memoria ni se lee del disco (eso vive en
    // LockForProcess, que sí es el punto de configuración).
    chain_.resetState();
    return S_OK;
}

STDMETHODIMP WarzoneApoMfx::GetLatency(HNSTIME* pTime) {
    if (!pTime) return E_POINTER;
    // Cero latencia añadida: la cadena es feed-forward (biquads, detectores
    // de envolvente y ganancias). No hay look-ahead ni buffering interno.
    *pTime = 0;
    return S_OK;
}

STDMETHODIMP WarzoneApoMfx::GetRegistrationProperties(APO_REG_PROPERTIES** ppRegProps) {
    if (!ppRegProps) return E_POINTER;

    auto* props = static_cast<APO_REG_PROPERTIES*>(
        CoTaskMemAlloc(sizeof(APO_REG_PROPERTIES)));
    if (!props) return E_OUTOFMEMORY;

    *props = kRegProperties;
    *ppRegProps = props;
    return S_OK;
}

STDMETHODIMP WarzoneApoMfx::Initialize(UINT32 /*cbDataSize*/, BYTE* /*pbyData*/) {
    // No necesitamos datos de inicialización del driver: los ajustes vienen
    // del archivo que escribe la app de escritorio.
    return S_OK;
}

HRESULT WarzoneApoMfx::validateFormat(IAudioMediaType* format) const {
    const WAVEFORMATEX* wave = waveFormatOf(format);
    if (!wave) return E_INVALIDARG;
    if (wave->nChannels < 1) return APOERR_FORMAT_NOT_SUPPORTED;

    // Se acepta float32 y PCM de 16/24/32 bits. Antes solo se aceptaba
    // float32, y rechazar el formato hacia que Windows tumbase la cadena de
    // audio del dispositivo entero: el equipo se quedaba sin sonido. Un
    // efecto de modo debe adaptarse al formato que le den, no imponerlo.
    if (isFloat32(wave)) return S_OK;
    if (wave->wBitsPerSample == 16 || wave->wBitsPerSample == 24 ||
        wave->wBitsPerSample == 32) {
        return S_OK;
    }
    return APOERR_FORMAT_NOT_SUPPORTED;
}

// Conversiones entre el formato de la conexion y el float que usa la cadena.
// Sin reservas ni llamadas bloqueantes: se ejecutan en el hilo de audio.
void WarzoneApoMfx::convertToFloat(const BYTE* source, float* dest, UINT32 frames) const {
    const size_t samples = static_cast<size_t>(frames) * channelCount_;
    if (bitsPerSample_ == 16) {
        const auto* src = reinterpret_cast<const int16_t*>(source);
        for (size_t i = 0; i < samples; ++i) dest[i] = src[i] / 32768.0f;
    } else if (bitsPerSample_ == 24) {
        for (size_t i = 0; i < samples; ++i) {
            const BYTE* s = source + i * 3;
            // Sign-extend de 24 a 32 bits mirando el bit alto.
            const int32_t v = s[0] | (s[1] << 8) | (s[2] << 16) |
                              ((s[2] & 0x80) ? 0xFF000000 : 0);
            dest[i] = v / 8388608.0f;
        }
    } else {
        const auto* src = reinterpret_cast<const int32_t*>(source);
        for (size_t i = 0; i < samples; ++i) dest[i] = src[i] / 2147483648.0f;
    }
}

void WarzoneApoMfx::convertFromFloat(const float* source, BYTE* dest, UINT32 frames) const {
    const size_t samples = static_cast<size_t>(frames) * channelCount_;
    if (bitsPerSample_ == 16) {
        auto* out = reinterpret_cast<int16_t*>(dest);
        for (size_t i = 0; i < samples; ++i) {
            out[i] = static_cast<int16_t>(std::clamp(source[i], -1.0f, 1.0f) * 32767.0f);
        }
    } else if (bitsPerSample_ == 24) {
        for (size_t i = 0; i < samples; ++i) {
            const int32_t v = static_cast<int32_t>(
                std::clamp(source[i], -1.0f, 1.0f) * 8388607.0f);
            BYTE* d = dest + i * 3;
            d[0] = static_cast<BYTE>(v & 0xFF);
            d[1] = static_cast<BYTE>((v >> 8) & 0xFF);
            d[2] = static_cast<BYTE>((v >> 16) & 0xFF);
        }
    } else {
        auto* out = reinterpret_cast<int32_t*>(dest);
        for (size_t i = 0; i < samples; ++i) {
            out[i] = static_cast<int32_t>(
                std::clamp(source[i], -1.0f, 1.0f) * 2147483647.0f);
        }
    }
}

STDMETHODIMP WarzoneApoMfx::IsInputFormatSupported(
    IAudioMediaType* /*pOppositeFormat*/,
    IAudioMediaType* pRequestedInputFormat,
    IAudioMediaType** ppSupportedInputFormat) {
    if (ppSupportedInputFormat) *ppSupportedInputFormat = nullptr;
    if (!pRequestedInputFormat) return E_POINTER;

    const HRESULT hr = validateFormat(pRequestedInputFormat);
    if (FAILED(hr)) {
        const WAVEFORMATEX* w = waveFormatOf(pRequestedInputFormat);
        if (w) {
            apoLog(L"Formato de entrada RECHAZADO: tag=%u bits=%u canales=%u %uHz",
                   w->wFormatTag, w->wBitsPerSample, w->nChannels, w->nSamplesPerSec);
        } else {
            apoLog(L"Formato de entrada RECHAZADO: no se pudo leer");
        }
        return hr;
    }

    // El formato pedido sirve tal cual. Devolverlo es opcional cuando el
    // resultado es S_OK, pero el motor puede pedirlo: le pasamos el mismo.
    {
        const WAVEFORMATEX* w = waveFormatOf(pRequestedInputFormat);
        if (w) {
            apoLog(L"Formato de entrada ACEPTADO: bits=%u canales=%u %uHz",
                   w->wBitsPerSample, w->nChannels, w->nSamplesPerSec);
        }
    }
    if (ppSupportedInputFormat) {
        pRequestedInputFormat->AddRef();
        *ppSupportedInputFormat = pRequestedInputFormat;
    }
    return S_OK;
}

STDMETHODIMP WarzoneApoMfx::IsOutputFormatSupported(
    IAudioMediaType* /*pOppositeFormat*/,
    IAudioMediaType* pRequestedOutputFormat,
    IAudioMediaType** ppSupportedOutputFormat) {
    if (ppSupportedOutputFormat) *ppSupportedOutputFormat = nullptr;
    if (!pRequestedOutputFormat) return E_POINTER;

    const HRESULT hr = validateFormat(pRequestedOutputFormat);
    if (FAILED(hr)) return hr;

    if (ppSupportedOutputFormat) {
        pRequestedOutputFormat->AddRef();
        *ppSupportedOutputFormat = pRequestedOutputFormat;
    }
    return S_OK;
}

STDMETHODIMP WarzoneApoMfx::GetInputChannelCount(UINT32* pu32ChannelCount) {
    if (!pu32ChannelCount) return E_POINTER;
    *pu32ChannelCount = channelCount_ > 0 ? channelCount_ : 2;
    return S_OK;
}

// -----------------------------------------------------------------------------
// IAudioProcessingObjectConfiguration
//
// Aquí es donde se hace TODO el trabajo pesado: validar formatos, reservar
// buffers y leer los ajustes del usuario. Después de esto, APOProcess() no
// puede hacer nada de eso.
// -----------------------------------------------------------------------------
STDMETHODIMP WarzoneApoMfx::LockForProcess(
    UINT32 u32NumInputConnections,
    APO_CONNECTION_DESCRIPTOR** ppInputConnections,
    UINT32 u32NumOutputConnections,
    APO_CONNECTION_DESCRIPTOR** ppOutputConnections) {

    apoLog(L"LockForProcess: %u entradas, %u salidas",
           u32NumInputConnections, u32NumOutputConnections);

    if (locked_) {
        apoLog(L"  rechazado: ya estaba bloqueado");
        return APOERR_ALREADY_INITIALIZED;
    }
    if (u32NumInputConnections != 1 || u32NumOutputConnections != 1) {
        apoLog(L"  rechazado: numero de conexiones no soportado");
        return APOERR_NUM_CONNECTIONS_INVALID;
    }
    if (!ppInputConnections || !ppOutputConnections ||
        !ppInputConnections[0] || !ppOutputConnections[0]) {
        apoLog(L"  rechazado: descriptores nulos");
        return E_POINTER;
    }

    const APO_CONNECTION_DESCRIPTOR* input = ppInputConnections[0];
    const APO_CONNECTION_DESCRIPTOR* output = ppOutputConnections[0];

    HRESULT hr = validateFormat(input->pFormat);
    if (FAILED(hr)) {
        const WAVEFORMATEX* w = waveFormatOf(input->pFormat);
        if (w) {
            apoLog(L"  rechazado: formato de ENTRADA bits=%u canales=%u %uHz",
                   w->wBitsPerSample, w->nChannels, w->nSamplesPerSec);
        }
        return hr;
    }
    hr = validateFormat(output->pFormat);
    if (FAILED(hr)) {
        const WAVEFORMATEX* w = waveFormatOf(output->pFormat);
        if (w) {
            apoLog(L"  rechazado: formato de SALIDA bits=%u canales=%u %uHz",
                   w->wBitsPerSample, w->nChannels, w->nSamplesPerSec);
        }
        return hr;
    }

    const WAVEFORMATEX* inputWave = waveFormatOf(input->pFormat);
    const WAVEFORMATEX* outputWave = waveFormatOf(output->pFormat);
    if (!inputWave || !outputWave) return E_INVALIDARG;

    // Procesamos in-place: entrada y salida deben coincidir en todo.
    if (inputWave->nChannels != outputWave->nChannels ||
        inputWave->nSamplesPerSec != outputWave->nSamplesPerSec) {
        return APOERR_INVALID_CONNECTION_FORMAT;
    }

    channelCount_ = inputWave->nChannels;
    sampleRate_ = inputWave->nSamplesPerSec;
    maxFrameCount_ = input->u32MaxFrameCount;
    if (output->u32MaxFrameCount < maxFrameCount_) {
        maxFrameCount_ = output->u32MaxFrameCount;
    }

    if (maxFrameCount_ == 0) return APOERR_INVALID_OUTPUT_MAXFRAMECOUNT;

    // Toda la reserva de memoria ocurre aquí, nunca en APOProcess.
    //
    // El try/catch no es decorativo: prepare() y reserve() reservan, y una
    // excepción de C++ escapando por una frontera COM es comportamiento
    // indefinido -- dentro de audiodg.exe puede tumbar el audio de todo el
    // sistema. Un fallo de reserva debe salir como HRESULT, no como throw.
    formatIsFloat_ = isFloat32(inputWave);
    bitsPerSample_ = inputWave->wBitsPerSample;

    try {
        chain_.prepare(static_cast<double>(sampleRate_), channelCount_);
        chain_.reserve(maxFrameCount_);
        // Buffer de conversion: solo hace falta si el formato no es float,
        // pero se reserva siempre para que APOProcess nunca tenga que decidir
        // si reservar (reservar ahi esta prohibido).
        conversionBuffer_.assign(
            static_cast<size_t>(maxFrameCount_) * channelCount_, 0.0f);
    } catch (const std::bad_alloc&) {
        return E_OUTOFMEMORY;
    } catch (...) {
        return E_FAIL;
    }

    loadUserSettings();
    startSettingsWatcher();

    apoLog(L"PROCESANDO: %u canales @ %uHz, bloques de hasta %u frames",
           channelCount_, sampleRate_, maxFrameCount_);

    locked_ = true;
    return S_OK;
}

STDMETHODIMP WarzoneApoMfx::UnlockForProcess() {
    if (!locked_) return APOERR_ALREADY_UNLOCKED;
    stopSettingsWatcher();
    locked_ = false;
    return S_OK;
}

void WarzoneApoMfx::loadUserSettings() {
    // Leer del disco y construir strings puede lanzar; igual que arriba, nada
    // de eso debe escapar hacia el motor de audio. Si falla, los ajustes se
    // quedan en sus valores por defecto, que es un estado perfectamente
    // usable -- mucho mejor que propagar la excepción.
    try {
        loadUserSettingsImpl();
    } catch (...) {
        // Silencio deliberado: sin ajustes de usuario el APO sigue siendo
        // funcional con los valores por defecto de la cadena.
    }
}

void WarzoneApoMfx::loadUserSettingsImpl() {
    const std::wstring path = settingsPath();

    // Que el APO vea o no el archivo del usuario es la diferencia entre
    // "los sliders no hacen nada" y "funcionan": conviene dejarlo por escrito.
    static bool reported = false;
    if (!reported) {
        reported = true;
        const bool exists = !path.empty() &&
            GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
        apoLog(L"Ajustes en \"%s\" -> %s", path.c_str(),
               exists ? L"encontrados" : L"NO EXISTE (se usan valores por defecto)");
    }

    // Si el archivo no existe todavía (el usuario nunca abrió la app), cada
    // ajuste cae a su valor por defecto -- los mismos que usa la app.
    chain_.setFootstepBoostDb(readSetting(path, L"footstepBoostDb", 7.0f));
    chain_.setVehicleThresholdDb(readSetting(path, L"vehicleThresholdDb", -40.0f));
    chain_.setAirGainDb(readSetting(path, L"airGainDb", -4.0f));
    chain_.setLowCrossoverHz(readSetting(path, L"lowCrossoverHz", 700.0f));
    chain_.setAirCrossoverHz(readSetting(path, L"airCrossoverHz", 5000.0f));
    chain_.setStereoWidth(readSetting(path, L"stereoWidth", 1.3f));
    chain_.setOutputTrimDb(readSetting(path, L"outputTrimDb", 0.0f));
}

// -----------------------------------------------------------------------------
// IAudioProcessingObjectRT -- CAMINO DE TIEMPO REAL
//
// Prohibido aquí: reservar/liberar memoria, locks, I/O, excepciones, memoria
// paginable. Todo lo que hace esta función es leer el buffer de entrada,
// pasarlo por la cadena DSP (que tampoco reserva) y marcar la salida.
// -----------------------------------------------------------------------------
STDMETHODIMP_(void) WarzoneApoMfx::APOProcess(
    UINT32 u32NumInputConnections,
    APO_CONNECTION_PROPERTY** ppInputConnections,
    UINT32 u32NumOutputConnections,
    APO_CONNECTION_PROPERTY** ppOutputConnections) {

    if (u32NumInputConnections == 0 || u32NumOutputConnections == 0) return;
    if (!ppInputConnections || !ppOutputConnections) return;

    APO_CONNECTION_PROPERTY* input = ppInputConnections[0];
    APO_CONNECTION_PROPERTY* output = ppOutputConnections[0];
    if (!input || !output) return;

    switch (input->u32BufferFlags) {
        case BUFFER_VALID: {
            auto* inBytes = reinterpret_cast<BYTE*>(input->pBuffer);
            auto* outBytes = reinterpret_cast<BYTE*>(output->pBuffer);
            if (!inBytes || !outBytes) return;

            UINT32 frames = input->u32ValidFrameCount;
            // Nunca procesar mas de lo reservado: el buffer de conversion se
            // dimensiono en LockForProcess y aqui no se puede agrandar.
            const UINT32 capacity = channelCount_ > 0
                ? static_cast<UINT32>(conversionBuffer_.size() / channelCount_) : 0;
            if (frames > capacity) frames = capacity;
            if (frames == 0) return;

            if (formatIsFloat_) {
                auto* samples = reinterpret_cast<float*>(inBytes);
                auto* outSamples = reinterpret_cast<float*>(outBytes);

                // Declaramos APO_FLAG_INPLACE, así que normalmente entrada y
                // salida son el mismo buffer. Si el motor decide darnos
                // buffers distintos, copiamos primero para no perder la señal.
                if (samples != outSamples) {
                    memcpy(outSamples, samples,
                           static_cast<size_t>(frames) * channelCount_ * sizeof(float));
                }
                chain_.process(outSamples, frames);
            } else {
                // PCM: convertir a float, procesar, y devolver al formato
                // original. Aceptar PCM en vez de rechazarlo es lo que evita
                // que Windows tumbe la cadena de audio del dispositivo.
                convertToFloat(inBytes, conversionBuffer_.data(), frames);
                chain_.process(conversionBuffer_.data(), frames);
                convertFromFloat(conversionBuffer_.data(), outBytes, frames);
            }

            output->u32ValidFrameCount = frames;
            output->u32BufferFlags = BUFFER_VALID;
            break;
        }

        case BUFFER_SILENT:
            // Silencio: no hay nada que procesar. Propagamos la marca para
            // que las etapas siguientes puedan saltarse trabajo igual que
            // nosotros -- pero seguimos declarando los frames como válidos.
            output->u32ValidFrameCount = input->u32ValidFrameCount;
            output->u32BufferFlags = BUFFER_SILENT;
            break;

        default:
            output->u32ValidFrameCount = 0;
            output->u32BufferFlags = input->u32BufferFlags;
            break;
    }
}

STDMETHODIMP_(UINT32) WarzoneApoMfx::CalcInputFrames(UINT32 u32OutputFrameCount) {
    // Relación 1:1 -- no remuestreamos ni acumulamos.
    return u32OutputFrameCount;
}

STDMETHODIMP_(UINT32) WarzoneApoMfx::CalcOutputFrames(UINT32 u32InputFrameCount) {
    return u32InputFrameCount;
}

// -----------------------------------------------------------------------------
// IAudioSystemEffects2
// -----------------------------------------------------------------------------
STDMETHODIMP WarzoneApoMfx::GetEffectsList(LPGUID* ppEffectsIds, UINT* pcEffects,
                                             HANDLE /*Event*/) {
    if (!ppEffectsIds || !pcEffects) return E_POINTER;

    // Windows usa esta lista para mostrar los efectos activos en la interfaz
    // de sonido. Declaramos uno: la cadena completa se presenta como un solo
    // efecto, porque para el usuario es una sola cosa que esta o no activa.
    auto* ids = static_cast<LPGUID>(CoTaskMemAlloc(sizeof(GUID)));
    if (!ids) {
        *ppEffectsIds = nullptr;
        *pcEffects = 0;
        return E_OUTOFMEMORY;
    }

    ids[0] = CLSID_WarzoneApoMfx;
    *ppEffectsIds = ids;
    *pcEffects = 1;
    return S_OK;
}

} // namespace warzoneapo
