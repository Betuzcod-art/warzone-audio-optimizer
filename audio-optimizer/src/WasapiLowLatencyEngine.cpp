// =============================================================================
// WasapiLowLatencyEngine.cpp
// -----------------------------------------------------------------------------
// Implementación paso a paso:
//   1) Enumeración de dispositivos y activación de IAudioClient.
//   2) Negociación de formato EXCLUSIVE con el period más pequeño soportado
//      (mapeado a nuestro bufferFrames objetivo de 64-128 samples).
//   3) Configuración Event-Driven (SetEventHandle) para que el hilo se
//      despierte exactamente cuando el buffer necesita servicio, en vez de
//      hacer polling (que desperdicia tiempo de CPU y añade jitter).
//   4) Hilos dedicados con MMCSS ("Pro Audio") + THREAD_PRIORITY_TIME_CRITICAL.
// =============================================================================
#include "WasapiLowLatencyEngine.h"
#include <functiondiscoverykeys_devpkey.h>
#include <ksmedia.h>
#include <cmath>
#include <sstream>
#include <algorithm>

namespace audiopt {

namespace {
    std::wstring hresultToString(HRESULT hr, const wchar_t* context) {
        std::wstringstream ss;
        ss << context << L" (HRESULT=0x" << std::hex << hr << L")";
        return ss.str();
    }

    bool selectRenderEndpoint(IMMDeviceEnumerator* enumerator,
                              const std::wstring& name,
                              IMMDevice** device) {
        if (name.empty()) {
            return SUCCEEDED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, device));
        }

        Microsoft::WRL::ComPtr<IMMDeviceCollection> devices;
        if (FAILED(enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE,
                                                   devices.GetAddressOf()))) {
            return false;
        }

        UINT count = 0;
        devices->GetCount(&count);
        for (UINT index = 0; index < count; ++index) {
            Microsoft::WRL::ComPtr<IMMDevice> candidate;
            if (FAILED(devices->Item(index, candidate.GetAddressOf()))) continue;

            Microsoft::WRL::ComPtr<IPropertyStore> properties;
            if (FAILED(candidate->OpenPropertyStore(STGM_READ, properties.GetAddressOf()))) continue;

            PROPVARIANT value;
            PropVariantInit(&value);
            const HRESULT propertyResult = properties->GetValue(
                PKEY_Device_FriendlyName, &value);
            const bool matches = SUCCEEDED(propertyResult) &&
                value.vt == VT_LPWSTR && value.pwszVal &&
                std::wstring(value.pwszVal).find(name) != std::wstring::npos;
            PropVariantClear(&value);
            if (matches) {
                *device = candidate.Detach();
                return true;
            }
        }
        return false;
    }
}

WasapiLowLatencyEngine::WasapiLowLatencyEngine() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
}

WasapiLowLatencyEngine::~WasapiLowLatencyEngine() {
    stop();
    if (renderEvent_) CloseHandle(renderEvent_);
    if (captureEvent_) CloseHandle(captureEvent_);
    CoUninitialize();
}

// -----------------------------------------------------------------------------
// PASO 1: Inicialización general
// -----------------------------------------------------------------------------
bool WasapiLowLatencyEngine::initialize(const EngineConfig& config, std::wstring& outError) {
    config_ = config;

    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                   __uuidof(IMMDeviceEnumerator),
                                   reinterpret_cast<void**>(deviceEnumerator_.GetAddressOf()));
    if (FAILED(hr)) {
        outError = hresultToString(hr, L"No se pudo crear IMMDeviceEnumerator");
        return false;
    }

    if (!initRenderClient(outError)) return false;
    if (!initCaptureClient(outError)) return false;

    // Dimensionamos el ring buffer con margen (varios periodos de
    // dispositivo) para absorber jitter entre los dos hilos -- captura
    // (loopback) y render corren sobre relojes de hardware independientes y
    // pueden desfasarse. El margen extra NO se traduce en más latencia en
    // régimen estable porque readInsured() vigila el backlog y lo recorta
    // (resync) en cuanto crece más de la cuenta; ver InsuredRingBuffer.
    const UINT32 deviceBufferFrames = std::max(
        renderBufferFrameCount_, captureBufferFrameCount_);
    constexpr UINT32 kRingBufferHeadroomBlocks = 6;
    ringBuffer_ = std::make_unique<InsuredRingBuffer>(
        static_cast<size_t>(deviceBufferFrames) * kRingBufferHeadroomBlocks,
        config_.numChannels);

    dspChain_.prepare(config_.sampleRate, config_.numChannels);

    return true;
}

// -----------------------------------------------------------------------------
// PASO 2: Cliente de RENDER en modo Exclusive + Event-Driven
// -----------------------------------------------------------------------------
bool WasapiLowLatencyEngine::initRenderClient(std::wstring& outError) {
    HRESULT hr = AUDCLNT_E_DEVICE_INVALIDATED;
    if (!selectRenderEndpoint(deviceEnumerator_.Get(), config_.renderDeviceName,
                              renderDevice_.GetAddressOf())) {
        // El nombre del dispositivo físico cambia entre equipos; usa el
        // endpoint predeterminado como fallback portable.
        hr = deviceEnumerator_->GetDefaultAudioEndpoint(
            eRender, eConsole, renderDevice_.GetAddressOf());
        if (FAILED(hr)) {
            outError = hresultToString(hr, L"No se encontró dispositivo de render");
            return false;
        }
    }

    hr = renderDevice_->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                  reinterpret_cast<void**>(renderClient_.GetAddressOf()));
    if (FAILED(hr)) {
        outError = hresultToString(hr, L"No se pudo activar IAudioClient de render");
        return false;
    }

        WAVEFORMATEX* mixFormat = nullptr;
        hr = renderClient_->GetMixFormat(&mixFormat);
        if (FAILED(hr) || !mixFormat) {
            outError = hresultToString(hr, L"No se pudo obtener el formato nativo de render");
            return false;
        }
        config_.sampleRate = mixFormat->nSamplesPerSec;
        renderChannels_ = mixFormat->nChannels;
        renderLeftChannel_ = 0;
        renderRightChannel_ = renderChannels_ > 1 ? 1 : 0;
        if (mixFormat->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
            const auto* extensible = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(mixFormat);
            UINT32 channelIndex = 0;
            for (DWORD bit = 1; bit != 0 && channelIndex < renderChannels_; bit <<= 1) {
                if ((extensible->dwChannelMask & bit) != 0) {
                    if (bit == SPEAKER_FRONT_LEFT) renderLeftChannel_ = channelIndex;
                    if (bit == SPEAKER_FRONT_RIGHT) renderRightChannel_ = channelIndex;
                    ++channelIndex;
                }
            }
        }
        // VB-CABLE entrega una mezcla estéreo; procesamos en estéreo y dejamos
        // que WASAPI adapte la salida al número de canales físico del equipo.
        config_.numChannels = 2;

    // Intentamos primero modo EXCLUSIVE: evita por completo el mezclador de
    // audio compartido de Windows (audio engine / APOs de sistema), que por
    // sí solo añade varios ms de más -- es la mayor fuente de latencia que
    // SÍ podemos recortar desde aquí sin depender de sustituir VB-CABLE. Es
    // sensible al driver/formato: si el dispositivo físico no acepta el mix
    // format en Exclusive, caemos de forma transparente al modo Shared de
    // siempre (con período mínimo vía IAudioClient3 si el driver lo soporta).
    renderExclusive_ = false;
    hr = renderClient_->IsFormatSupported(AUDCLNT_SHAREMODE_EXCLUSIVE, mixFormat, nullptr);
    if (SUCCEEDED(hr)) {
        REFERENCE_TIME defaultPeriod = 0;
        REFERENCE_TIME minimumPeriod = 0;
        if (SUCCEEDED(renderClient_->GetDevicePeriod(&defaultPeriod, &minimumPeriod))) {
            const DWORD exclusiveFlags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK;
            hr = renderClient_->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE, exclusiveFlags,
                                            minimumPeriod, minimumPeriod, mixFormat, nullptr);
            if (hr == AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED) {
                // WASAPI exige recrear el IAudioClient tras este error
                // específico antes de reintentar con el tamaño alineado.
                UINT32 alignedFrames = 0;
                renderClient_->GetBufferSize(&alignedFrames);
                renderClient_.Reset();
                hr = renderDevice_->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                              reinterpret_cast<void**>(renderClient_.GetAddressOf()));
                if (SUCCEEDED(hr)) {
                    const REFERENCE_TIME alignedPeriod = static_cast<REFERENCE_TIME>(
                        (10000000.0 * alignedFrames) / mixFormat->nSamplesPerSec + 0.5);
                    hr = renderClient_->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE, exclusiveFlags,
                                                    alignedPeriod, alignedPeriod, mixFormat, nullptr);
                }
            }
            renderExclusive_ = SUCCEEDED(hr);
        } else {
            hr = E_FAIL;
        }
    }

    if (!renderExclusive_ && !renderClient_) {
        // El reintento de alineación pudo dejar renderClient_ vacío si la
        // reactivación falló; lo recreamos para poder caer a modo Shared.
        hr = renderDevice_->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                      reinterpret_cast<void**>(renderClient_.GetAddressOf()));
        if (FAILED(hr)) {
            CoTaskMemFree(mixFormat);
            outError = hresultToString(hr, L"No se pudo reactivar IAudioClient de render");
            return false;
        }
    }

    if (!renderExclusive_) {
        // Compartido estéreo: WASAPI realiza una única conversión al formato
        // físico de los auriculares y evita escribir manualmente canales
        // 5.1/7.1.
        const DWORD sharedFlags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK |
                                  AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM;
        Microsoft::WRL::ComPtr<IAudioClient3> renderClient3;
        UINT32 defaultPeriod = 0;
        UINT32 fundamentalPeriod = 0;
        UINT32 minPeriod = 0;
        UINT32 maxPeriod = 0;
        const bool supportsSmallPeriod =
            SUCCEEDED(renderClient_.As(&renderClient3)) &&
            SUCCEEDED(renderClient3->GetSharedModeEnginePeriod(
                mixFormat, &defaultPeriod, &fundamentalPeriod,
                &minPeriod, &maxPeriod));

        if (supportsSmallPeriod) {
            UINT32 period = minPeriod;
            period = ((period + fundamentalPeriod - 1) / fundamentalPeriod) * fundamentalPeriod;
            period = std::min(period, maxPeriod);
            hr = renderClient3->InitializeSharedAudioStream(
                sharedFlags, period, mixFormat, nullptr);
        }
        if (!supportsSmallPeriod || FAILED(hr)) {
            hr = renderClient_->Initialize(
                AUDCLNT_SHAREMODE_SHARED, sharedFlags, 0, 0,
                mixFormat, nullptr);
        }
    }

    CoTaskMemFree(mixFormat);

    if (FAILED(hr)) {
        outError = hresultToString(hr, renderExclusive_
            ? L"Fallo al inicializar IAudioClient de render en modo Exclusive"
            : L"Fallo al inicializar IAudioClient de render en modo Shared");
        return false;
    }

    renderEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    renderClient_->SetEventHandle(renderEvent_);
    renderClient_->GetBufferSize(&renderBufferFrameCount_);

    hr = renderClient_->GetService(__uuidof(IAudioRenderClient),
                                    reinterpret_cast<void**>(renderService_.GetAddressOf()));
    if (FAILED(hr)) {
        outError = hresultToString(hr, L"No se pudo obtener IAudioRenderClient");
        return false;
    }

    // Pre-relleno de silencio ANTES de arrancar el reloj del dispositivo
    // (eso lo hace start(), más tarde). Si se hiciera después -- como estaba
    // antes, dentro del propio hilo de render -- el dispositivo ya estaría
    // pidiendo datos desde IAudioClient::Start() mientras el hilo todavía ni
    // se ha programado, generando un underrun/resync espurio garantizado en
    // cada arranque.
    BYTE* prefillData = nullptr;
    if (SUCCEEDED(renderService_->GetBuffer(renderBufferFrameCount_, &prefillData))) {
        std::memset(prefillData, 0, static_cast<size_t>(renderBufferFrameCount_) *
                                         renderChannels_ * sizeof(float));
        renderService_->ReleaseBuffer(renderBufferFrameCount_, 0);
    }

    return true;
}

// -----------------------------------------------------------------------------
// PASO 3: Cliente de CAPTURA (loopback del propio dispositivo de salida, para
// interceptar el audio que el juego ya envió, o un input dedicado)
// -----------------------------------------------------------------------------
bool WasapiLowLatencyEngine::initCaptureClient(std::wstring& outError) {
    if (config_.useLoopbackCapture) {
        // Loopback: reutilizamos el dispositivo de RENDER por defecto como
        // fuente de captura -- así interceptamos la mezcla final del juego
        // sin necesidad de que el juego sepa nada de nuestra app.
        if (!selectRenderEndpoint(deviceEnumerator_.Get(), config_.captureDeviceName,
                                  captureDevice_.GetAddressOf())) {
            const HRESULT hr = AUDCLNT_E_DEVICE_INVALIDATED;
            outError = hresultToString(hr,
                L"No se encontró el dispositivo de loopback solicitado. Instale VB-CABLE y reinicie Windows; se esperaba CABLE Input");
            return false;
        }
    } else {
        HRESULT hr = deviceEnumerator_->GetDefaultAudioEndpoint(eCapture, eConsole,
                                                                 captureDevice_.GetAddressOf());
        if (FAILED(hr)) {
            outError = hresultToString(hr, L"No se encontró dispositivo de captura");
            return false;
        }
    }

    HRESULT hr = captureDevice_->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                           reinterpret_cast<void**>(captureClient_.GetAddressOf()));
    if (FAILED(hr)) {
        outError = hresultToString(hr, L"No se pudo activar IAudioClient de captura");
        return false;
    }

        WAVEFORMATEX* mixFormat = nullptr;
        hr = captureClient_->GetMixFormat(&mixFormat);
        if (FAILED(hr) || !mixFormat) {
            outError = hresultToString(hr, L"No se pudo obtener el formato nativo de captura");
            return false;
        }

        captureChannels_ = mixFormat->nChannels;
        captureBitsPerSample_ = mixFormat->wBitsPerSample;
        captureContainerBits_ = mixFormat->wBitsPerSample;
        captureBlockAlign_ = mixFormat->nBlockAlign;
        captureFormatIsFloat_ = mixFormat->wFormatTag == WAVE_FORMAT_IEEE_FLOAT;
        if (mixFormat->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
            const auto* extensible = reinterpret_cast<WAVEFORMATEXTENSIBLE*>(mixFormat);
            captureFormatIsFloat_ = extensible->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
            captureBitsPerSample_ = extensible->Samples.wValidBitsPerSample;
        }

    REFERENCE_TIME hnsBufferDuration =
        static_cast<REFERENCE_TIME>((10000000.0 * config_.bufferFrames) / config_.sampleRate);

    // NOTA: el loopback true de WASAPI (AUDCLNT_STREAMFLAGS_LOOPBACK) solo
    // opera en modo SHARED, no Exclusive (limitación de la API de Windows).
    // Por eso, si useLoopbackCapture=true, inicializamos en Shared con el
    // flag de loopback; si es captura dedicada (micrófono virtual/cable de
    // audio), sí podemos ir Exclusive+EventDriven igual que el render.
    DWORD streamFlags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK;
    AUDCLNT_SHAREMODE shareMode = AUDCLNT_SHAREMODE_EXCLUSIVE;

    if (config_.useLoopbackCapture) {
        shareMode = AUDCLNT_SHAREMODE_SHARED;
        streamFlags |= AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM;
        Microsoft::WRL::ComPtr<IAudioClient3> captureClient3;
        UINT32 defaultPeriod = 0;
        UINT32 fundamentalPeriod = 0;
        UINT32 minPeriod = 0;
        UINT32 maxPeriod = 0;
        const bool supportsSmallPeriod =
            SUCCEEDED(captureClient_.As(&captureClient3)) &&
            SUCCEEDED(captureClient3->GetSharedModeEnginePeriod(
                mixFormat, &defaultPeriod, &fundamentalPeriod,
                &minPeriod, &maxPeriod));
        if (supportsSmallPeriod) {
            UINT32 period = minPeriod;
            period = ((period + fundamentalPeriod - 1) / fundamentalPeriod) * fundamentalPeriod;
            period = std::min(period, maxPeriod);
            hr = captureClient3->InitializeSharedAudioStream(
                streamFlags, period, mixFormat, nullptr);
        }
        if (!supportsSmallPeriod || FAILED(hr)) {
            hr = captureClient_->Initialize(shareMode, streamFlags, 0, 0,
                                             mixFormat, nullptr);
        }
    } else {
        hr = captureClient_->Initialize(shareMode, streamFlags, hnsBufferDuration,
                         hnsBufferDuration, mixFormat, nullptr);
    }

    CoTaskMemFree(mixFormat);

    if (FAILED(hr)) {
        outError = hresultToString(hr, L"Fallo al inicializar IAudioClient de captura");
        return false;
    }

    captureEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    captureClient_->SetEventHandle(captureEvent_);
    captureClient_->GetBufferSize(&captureBufferFrameCount_);

    hr = captureClient_->GetService(__uuidof(IAudioCaptureClient),
                                     reinterpret_cast<void**>(captureService_.GetAddressOf()));
    if (FAILED(hr)) {
        outError = hresultToString(hr, L"No se pudo obtener IAudioCaptureClient");
        return false;
    }

    return true;
}

// -----------------------------------------------------------------------------
// PASO 4: Arranque de hilos con prioridad crítica + MMCSS "Pro Audio"
// -----------------------------------------------------------------------------
void WasapiLowLatencyEngine::pinThreadTimeCritical(const wchar_t* avTaskName, HANDLE* avrtHandleOut) {
    // MMCSS ("Pro Audio") indica al scheduler de Windows que este hilo
    // requiere latencia mínima y baja tasa de context-switch -- es lo mismo
    // que usan las DAWs profesionales (Pro Tools, Cubase, etc.) para
    // garantizar que el hilo de audio no sea desalojado por hilos de menor
    // criticidad del sistema.
    DWORD taskIndex = 0;
    HANDLE avrt = AvSetMmThreadCharacteristicsW(avTaskName, &taskIndex);
    if (avrt) {
        AvSetMmThreadPriority(avrt, AVRT_PRIORITY_CRITICAL);
    }
    // Prioridad de hilo Win32 estándar como capa adicional de garantía.
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    *avrtHandleOut = avrt;
}

void WasapiLowLatencyEngine::unpinThread(HANDLE avrtHandle) {
    if (avrtHandle) AvRevertMmThreadCharacteristics(avrtHandle);
}

bool WasapiLowLatencyEngine::start() {
    if (running_.exchange(true)) return true; // ya corriendo

    HRESULT hr = captureClient_->Start();
    if (FAILED(hr)) { running_ = false; return false; }

    hr = renderClient_->Start();
    if (FAILED(hr)) { running_ = false; captureClient_->Stop(); return false; }

    captureThread_ = std::thread(&WasapiLowLatencyEngine::captureThreadProc, this);
    renderThread_ = std::thread(&WasapiLowLatencyEngine::renderThreadProc, this);

    return true;
}

void WasapiLowLatencyEngine::stop() {
    if (!running_.exchange(false)) return;

    // Despertamos los hilos por si están bloqueados en WaitForSingleObject.
    if (captureEvent_) SetEvent(captureEvent_);
    if (renderEvent_) SetEvent(renderEvent_);

    if (captureThread_.joinable()) captureThread_.join();
    if (renderThread_.joinable()) renderThread_.join();

    if (captureClient_) captureClient_->Stop();
    if (renderClient_) renderClient_->Stop();
}

// -----------------------------------------------------------------------------
// Bucle de CAPTURA: lee del dispositivo (loopback del juego) y empuja al
// ring buffer. No aplica DSP aquí -- el DSP vive en el hilo de render para
// mantener el pipeline de captura lo más corto posible.
// -----------------------------------------------------------------------------
void WasapiLowLatencyEngine::captureThreadProc() {
    HANDLE avrtHandle = nullptr;
    pinThreadTimeCritical(L"Pro Audio", &avrtHandle);

    while (running_.load(std::memory_order_relaxed)) {
        DWORD waitResult = WaitForSingleObject(captureEvent_, 2000);
        if (waitResult != WAIT_OBJECT_0) continue;
        if (!running_.load(std::memory_order_relaxed)) break;

        UINT32 packetLength = 0;
        captureService_->GetNextPacketSize(&packetLength);

        while (packetLength > 0) {
            BYTE* data = nullptr;
            UINT32 numFramesAvailable = 0;
            DWORD flags = 0;

            HRESULT hr = captureService_->GetBuffer(&data, &numFramesAvailable, &flags,
                                                      nullptr, nullptr);
            if (FAILED(hr)) break;

            if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
                // Dispositivo en silencio: escribimos ceros explícitos para
                // no confundir al ring buffer con "no hay datos".
                static thread_local std::vector<float> silence;
                silence.assign(static_cast<size_t>(numFramesAvailable) * config_.numChannels, 0.0f);
                ringBuffer_->write(silence.data(), numFramesAvailable);
            } else if (captureFormatIsFloat_ && captureContainerBits_ == 32 &&
                       captureChannels_ == 2 && captureBlockAlign_ == 2 * sizeof(float)) {
                ringBuffer_->write(reinterpret_cast<const float*>(data), numFramesAvailable);
            } else {
                static thread_local std::vector<float> converted;
                converted.resize(static_cast<size_t>(numFramesAvailable) * config_.numChannels);
                const auto* bytes = reinterpret_cast<const BYTE*>(data);
                const uint32_t containerBytes = captureContainerBits_ / 8;
                for (UINT32 frame = 0; frame < numFramesAvailable; ++frame) {
                    for (uint32_t channel = 0; channel < config_.numChannels; ++channel) {
                        const uint32_t sourceChannel = std::min(channel, captureChannels_ - 1);
                        const BYTE* sample = bytes + frame * captureBlockAlign_ +
                                             sourceChannel * containerBytes;
                        float value = 0.0f;
                        if (captureFormatIsFloat_ && captureContainerBits_ == 32) {
                            value = *reinterpret_cast<const float*>(sample);
                        } else if (captureBitsPerSample_ == 16) {
                            value = *reinterpret_cast<const int16_t*>(sample) / 32768.0f;
                        } else if (captureBitsPerSample_ == 24) {
                            const int32_t raw = sample[0] | (sample[1] << 8) |
                                (sample[2] << 16) | ((sample[2] & 0x80) ? 0xFF000000 : 0);
                            value = raw / 8388608.0f;
                        } else if (captureBitsPerSample_ == 32) {
                            value = *reinterpret_cast<const int32_t*>(sample) / 2147483648.0f;
                        }
                        converted[frame * config_.numChannels + channel] = value;
                    }
                }
                ringBuffer_->write(converted.data(), numFramesAvailable);
            }

            captureService_->ReleaseBuffer(numFramesAvailable);
            captureService_->GetNextPacketSize(&packetLength);
        }
    }

    unpinThread(avrtHandle);
}

// -----------------------------------------------------------------------------
// Bucle de RENDER: se despierta por evento cuando WASAPI necesita el
// siguiente bloque, lee del ring buffer (con protección Insured Frame ante
// underrun), aplica la cadena DSP, y entrega el bloque al dispositivo.
// -----------------------------------------------------------------------------
void WasapiLowLatencyEngine::renderThreadProc() {
    HANDLE avrtHandle = nullptr;
    pinThreadTimeCritical(L"Pro Audio", &avrtHandle);

    std::vector<float> scratch(static_cast<size_t>(renderBufferFrameCount_) * config_.numChannels);

    // El pre-relleno de silencio ya se hizo en initRenderClient(), antes de
    // que start() arrancara el reloj del dispositivo -- ver ese archivo.

    while (running_.load(std::memory_order_relaxed)) {
        DWORD waitResult = WaitForSingleObject(renderEvent_, 2000);
        if (waitResult != WAIT_OBJECT_0) continue;
        if (!running_.load(std::memory_order_relaxed)) break;

        UINT32 currentPadding = 0;
        if (FAILED(renderClient_->GetCurrentPadding(&currentPadding))) continue;
        const UINT32 framesToWrite = renderBufferFrameCount_ > currentPadding
            ? renderBufferFrameCount_ - currentPadding : 0;
        if (framesToWrite == 0) continue;

        BYTE* data = nullptr;
        HRESULT hr = renderService_->GetBuffer(framesToWrite, &data);
        if (FAILED(hr)) continue;

        // 1) Lectura protegida (Insured Frame) desde el ring buffer.
        ringBuffer_->readInsured(scratch.data(), framesToWrite);

        // 2) Cadena DSP en el propio buffer de salida (in-place, cero copias
        //    adicionales -> minimiza latencia y presión de caché).
        dspChain_.process(scratch.data(), framesToWrite);

        // El DSP trabaja en estéreo. Colocamos L/R según la máscara real del
        // endpoint, evitando perder la imagen direccional en 5.1/7.1.
        float* outFloat = reinterpret_cast<float*>(data);
        for (UINT32 frame = 0; frame < framesToWrite; ++frame) {
            for (UINT32 channel = 0; channel < renderChannels_; ++channel) {
                outFloat[frame * renderChannels_ + channel] = 0.0f;
            }
            outFloat[frame * renderChannels_ + renderLeftChannel_] =
                scratch[frame * config_.numChannels];
            if (config_.numChannels > 1) {
                outFloat[frame * renderChannels_ + renderRightChannel_] =
                    scratch[frame * config_.numChannels + 1];
            }
        }

        renderService_->ReleaseBuffer(framesToWrite, 0);
    }

    unpinThread(avrtHandle);
}

uint64_t WasapiLowLatencyEngine::insuredFrameEvents() const {
    return ringBuffer_ ? ringBuffer_->underrunCount() : 0;
}

uint64_t WasapiLowLatencyEngine::resyncEvents() const {
    return ringBuffer_ ? ringBuffer_->resyncCount() : 0;
}

} // namespace audiopt
