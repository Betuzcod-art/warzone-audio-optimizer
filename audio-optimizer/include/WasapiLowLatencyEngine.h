// =============================================================================
// WasapiLowLatencyEngine.h
// -----------------------------------------------------------------------------
// Motor de audio en Windows usando WASAPI en modo EXCLUSIVE + EVENT-DRIVEN.
//
// Arquitectura de dos hilos, cada uno THREAD_PRIORITY_TIME_CRITICAL:
//
//   [Hilo de Captura]  Loopback del dispositivo de salida del juego (o un
//                       dispositivo de captura dedicado)
//                            |
//                            v  write()
//                   [InsuredRingBuffer]  <-- lock-free SPSC
//                            |
//                            v  readInsured()
//   [Hilo de Render]   WarzoneAudioChain::process()  ->  WASAPI Render Exclusive
//
// El uso de modo Exclusive evita el mezclador compartido de Windows (APO/
// audio engine mixer), que por sí solo puede añadir 10-20ms. Exclusive +
// Event-Driven con buffers de 64-128 samples permite acercarse al mínimo de
// hardware.
//
// NOTA: WASAPI es la API recomendada aquí en vez de ASIO porque:
//   - ASIO requiere drivers específicos del fabricante (no todos los
//     dispositivos los tienen) y una licencia/SDK de Steinberg.
//   - WASAPI Exclusive Event-Driven alcanza latencias comparables (~3-6ms)
//     en hardware moderno sin dependencias externas.
//   Si el hardware objetivo tiene driver ASIO dedicado, la misma arquitectura
//   de hilos/ring buffer/DSP es reutilizable cambiando solo la capa de E/S.
// =============================================================================
#pragma once

#include <Windows.h>
#include <mmdeviceapi.h>
#include <Audioclient.h>
#include <avrt.h>
#include <wrl/client.h>
#include <atomic>
#include <thread>
#include <memory>
#include <string>

#include "dsp/InsuredRingBuffer.h"
#include "dsp/WarzoneAudioChain.h"

#pragma comment(lib, "avrt.lib")

namespace audiopt {

using Microsoft::WRL::ComPtr;

struct EngineConfig {
    uint32_t bufferFrames = 96;     // Entre 64 y 128, punto medio equilibrado
    uint32_t sampleRate = 48000;
    uint32_t numChannels = 2;
    bool useLoopbackCapture = true; // true: captura el audio que el juego ya envía a salida
    std::wstring captureDeviceName; // Subcadena del endpoint de loopback, vacía = predeterminado
    std::wstring renderDeviceName;  // Subcadena del endpoint físico de salida

    // WASAPI Exclusive en el render evita el mezclador de Windows (menos
    // latencia), pero por diseño de la API BLOQUEA el dispositivo físico
    // para cualquier otra app mientras esté activo -- Discord, sonidos de
    // Windows, Spotify, todo se queda mudo. Para una app que corre de fondo
    // mientras se juega (con Discord abierto), ese trade-off no vale la
    // pena por defecto. Déjalo en true solo si sabes que quieres perder el
    // resto del audio del sistema a cambio de la latencia mínima.
    bool allowExclusiveRender = false;
};

class WasapiLowLatencyEngine {
public:
    WasapiLowLatencyEngine();
    ~WasapiLowLatencyEngine();

    // Inicializa dispositivos, formatos y buffers. Devuelve false + fills
    // outError en caso de fallo (HRESULT humanizado).
    bool initialize(const EngineConfig& config, std::wstring& outError);

    // Arranca los hilos de captura y render en THREAD_PRIORITY_TIME_CRITICAL.
    bool start();

    // Señala parada y hace join de ambos hilos de forma segura.
    void stop();

    // Estadísticas en vivo (para overlay/telemetría de la app).
    uint64_t insuredFrameEvents() const;
    uint64_t resyncEvents() const;
    double measuredRoundTripLatencyMs() const { return measuredLatencyMs_.load(); }
    uint32_t sampleRate() const { return config_.sampleRate; }
    uint32_t numChannels() const { return config_.numChannels; }
    bool renderModeExclusive() const { return renderExclusive_; }
    uint32_t renderContainerBits() const { return renderContainerBits_; }
    bool renderFormatIsFloat() const { return renderFormatIsFloat_; }
    uint32_t renderBufferFrames() const { return renderBufferFrameCount_; }
    uint32_t captureBufferFrames() const { return captureBufferFrameCount_; }
    // Traza de la negociación de período en modo Shared (IAudioClient3).
    const std::wstring& sharedModeDiagnostics() const { return sharedModeDiag_; }

    WarzoneAudioChain& dspChain() { return dspChain_; }

private:
    bool initCaptureClient(std::wstring& outError);
    bool initRenderClient(std::wstring& outError);

    void captureThreadProc();
    void renderThreadProc();
    void writeRenderSample(BYTE* dest, float value) const;

    static void pinThreadTimeCritical(const wchar_t* avTaskName, HANDLE* avrtHandleOut);
    static void unpinThread(HANDLE avrtHandle);

    EngineConfig config_{};

    ComPtr<IMMDeviceEnumerator> deviceEnumerator_;
    ComPtr<IMMDevice> renderDevice_;
    ComPtr<IMMDevice> captureDevice_;
    ComPtr<IAudioClient> renderClient_;
    ComPtr<IAudioClient> captureClient_;
    ComPtr<IAudioRenderClient> renderService_;
    ComPtr<IAudioCaptureClient> captureService_;

    HANDLE renderEvent_ = nullptr;
    HANDLE captureEvent_ = nullptr;
    UINT32 renderBufferFrameCount_ = 0;
    UINT32 captureBufferFrameCount_ = 0;
    UINT32 renderChannels_ = 2;
    UINT32 renderLeftChannel_ = 0;
    UINT32 renderRightChannel_ = 1;
    UINT32 captureChannels_ = 2;
    UINT32 captureBitsPerSample_ = 32;
    UINT32 captureContainerBits_ = 32;
    UINT32 captureBlockAlign_ = 8;
    bool captureFormatIsFloat_ = true;
    bool renderExclusive_ = false;
    UINT32 renderContainerBits_ = 32;
    bool renderFormatIsFloat_ = true;
    std::wstring sharedModeDiag_;

    std::unique_ptr<InsuredRingBuffer> ringBuffer_;
    WarzoneAudioChain dspChain_;

    std::thread captureThread_;
    std::thread renderThread_;
    std::atomic<bool> running_{false};

    std::atomic<double> measuredLatencyMs_{0.0};
};

} // namespace audiopt
