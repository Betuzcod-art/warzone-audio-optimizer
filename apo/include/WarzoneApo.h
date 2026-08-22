// =============================================================================
// WarzoneApo.h
// -----------------------------------------------------------------------------
// APO (Audio Processing Object) que inserta la cadena DSP de Warzone Audio
// Optimizer DENTRO del motor de audio de Windows.
//
// POR QUÉ UN APO (vs. la app con VB-CABLE):
//   La app original captura el audio ya renderizado por loopback desde un cable
//   virtual, lo procesa y lo vuelve a reproducir. Eso implica: un dispositivo
//   virtual extra, dos relojes de hardware distintos (con su deriva), un ring
//   buffer entre ambos, y el período del motor compartido (10ms en el equipo de
//   prueba) contado DOS veces. De ahí el delay que no se podía afinar.
//
//   Un APO no añade ninguna de esas etapas: Windows llama a APOProcess() con el
//   bloque de audio que ya iba a reproducir, lo modificamos en el sitio y sigue
//   su camino. La latencia añadida es esencialmente cero (declaramos 0 en
//   GetLatency porque la cadena es puramente feed-forward: biquads, compresores
//   y ganancias, sin look-ahead ni FFT).
//
// RESTRICCIONES DE TIEMPO REAL (importantes):
//   APOProcess() se ejecuta dentro de audiodg.exe en un hilo de prioridad
//   crítica. Ahí NO se puede: reservar/liberar memoria, tomar locks, tocar
//   memoria paginable, llamar a APIs bloqueantes, ni lanzar excepciones. Si se
//   incumple, el audio del sistema entero da cortes. Por eso toda la reserva
//   ocurre en LockForProcess() y la cadena DSP se configuró para no reservar
//   nunca dentro de process() (ver WarzoneAudioChain::reserve).
//
// FORMATO:
//   El motor de audio entrega float32 interleaved, que es justo lo que la
//   cadena espera. Rechazamos cualquier otro formato en IsInputFormatSupported
//   en vez de intentar convertir.
// =============================================================================
#pragma once

#include <windows.h>
#include <audioenginebaseapo.h>
#include <audioengineextensionapo.h>
#include <atomic>

#include "ApoGuids.h"
#include "dsp/WarzoneAudioChain.h"

namespace warzoneapo {

class WarzoneApoMfx
    : public IAudioProcessingObject,
      public IAudioProcessingObjectConfiguration,
      public IAudioProcessingObjectRT,
      public IAudioSystemEffects {
public:
    WarzoneApoMfx();
    virtual ~WarzoneApoMfx();

    // --- IUnknown ---
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override;
    STDMETHODIMP_(ULONG) AddRef() override;
    STDMETHODIMP_(ULONG) Release() override;

    // --- IAudioProcessingObject ---
    STDMETHODIMP Reset() override;
    STDMETHODIMP GetLatency(HNSTIME* pTime) override;
    STDMETHODIMP GetRegistrationProperties(APO_REG_PROPERTIES** ppRegProps) override;
    STDMETHODIMP Initialize(UINT32 cbDataSize, BYTE* pbyData) override;
    STDMETHODIMP IsInputFormatSupported(IAudioMediaType* pOppositeFormat,
                                         IAudioMediaType* pRequestedInputFormat,
                                         IAudioMediaType** ppSupportedInputFormat) override;
    STDMETHODIMP IsOutputFormatSupported(IAudioMediaType* pOppositeFormat,
                                          IAudioMediaType* pRequestedOutputFormat,
                                          IAudioMediaType** ppSupportedOutputFormat) override;
    STDMETHODIMP GetInputChannelCount(UINT32* pu32ChannelCount) override;

    // --- IAudioProcessingObjectConfiguration ---
    STDMETHODIMP LockForProcess(UINT32 u32NumInputConnections,
                                 APO_CONNECTION_DESCRIPTOR** ppInputConnections,
                                 UINT32 u32NumOutputConnections,
                                 APO_CONNECTION_DESCRIPTOR** ppOutputConnections) override;
    STDMETHODIMP UnlockForProcess() override;

    // --- IAudioProcessingObjectRT (camino de tiempo real) ---
    STDMETHODIMP_(void) APOProcess(UINT32 u32NumInputConnections,
                                    APO_CONNECTION_PROPERTY** ppInputConnections,
                                    UINT32 u32NumOutputConnections,
                                    APO_CONNECTION_PROPERTY** ppOutputConnections) override;
    STDMETHODIMP_(UINT32) CalcInputFrames(UINT32 u32OutputFrameCount) override;
    STDMETHODIMP_(UINT32) CalcOutputFrames(UINT32 u32InputFrameCount) override;

private:
    // Comprueba que el formato sea float32 interleaved, que es lo único que
    // la cadena procesa. Devuelve S_OK si sirve tal cual.
    HRESULT validateFormat(IAudioMediaType* format) const;

    // Relee los ajustes del usuario desde disco. Se llama fuera del camino
    // de tiempo real (LockForProcess): leer un archivo dentro de APOProcess
    // sería exactamente el tipo de llamada bloqueante que está prohibida.
    void loadUserSettings();

    std::atomic<ULONG> refCount_{1};
    audiopt::WarzoneAudioChain chain_;

    bool locked_ = false;
    UINT32 channelCount_ = 0;
    UINT32 sampleRate_ = 0;
    UINT32 maxFrameCount_ = 0;
};

} // namespace warzoneapo
