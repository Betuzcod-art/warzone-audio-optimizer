// =============================================================================
// DynamicsCompressor.h
// -----------------------------------------------------------------------------
// Compresor dinámico feed-forward, detector de envolvente peak con ataque y
// release exponenciales independientes, y "knee" suave.
//
// Objetivo específico de Warzone:
//   - Threshold bajo + ratio alto para explosiones/disparos (transitorios de
//     alta energía) -> atenúa picos fuertes.
//   - Combinado con el EQ Peaking (realce 2-6kHz), el resultado perceptual es
//     que pasos/recargas quedan relativamente más audibles sin necesidad de
//     subir el volumen maestro (que sería lo que satura con las explosiones).
//
// Ataque en el orden de 1-3 ms para capturar disparos de arma automática sin
// añadir latencia perceptible (no es un limitador look-ahead; es feed-forward
// puro, cero latencia añadida más allá del propio tiempo de ataque del
// detector, que actúa dentro del mismo buffer).
// =============================================================================
#pragma once

#include <cmath>
#include <algorithm>

namespace audiopt {

struct CompressorParams {
    float thresholdDb = -24.0f;   // Nivel a partir del cual empieza a comprimir
    float ratio = 4.0f;           // 4:1 -> reduce fuerte los picos de disparos
    float attackMs = 2.0f;        // Rápido: captura transitorios de disparo
    float releaseMs = 120.0f;     // Suave: evita "bombeo" (pumping) audible
    float kneeDb = 6.0f;          // Knee suave para transición natural
    float makeupGainDb = 4.0f;    // Compensa la reducción de ganancia global
};

class DynamicsCompressor {
public:
    void prepare(double sampleRate, const CompressorParams& params) {
        sampleRate_ = sampleRate;
        setParams(params);
    }

    void setParams(const CompressorParams& p) {
        params_ = p;
        attackCoeff_ = computeCoeff(p.attackMs);
        releaseCoeff_ = computeCoeff(p.releaseMs);
        makeupLinear_ = dbToLinear(p.makeupGainDb);
    }

    // Cambiar solo el umbral es barato y seguro en caliente: la curva
    // estática lo lee directamente, sin recalcular coeficientes de
    // ataque/release. Debe llamarse desde el hilo de audio (ver
    // WarzoneAudioChain::applyPendingParams).
    void setThresholdDb(float db) { params_.thresholdDb = db; }
    float thresholdDb() const { return params_.thresholdDb; }

    // Limpia el detector de envolvente sin tocar parámetros ni reservar.
    // Seguro de llamar desde el hilo de audio.
    void resetState() { envelopeDb_ = -100.0f; }

    // Procesa un bloque interleaved in-place. Detección de envolvente basada
    // en el máximo absoluto entre canales (para no descompensar la imagen
    // estéreo/posicional: la ganancia aplicada es la MISMA para todos los
    // canales en cada muestra, preservando diferencias inter-canal ITD/ILD).
    void processBlock(float* interleaved, size_t numFrames, uint32_t numChannels) {
        for (size_t i = 0; i < numFrames; ++i) {
            float* frame = &interleaved[i * numChannels];

            float peak = 0.0f;
            for (uint32_t ch = 0; ch < numChannels; ++ch) {
                peak = std::max(peak, std::fabs(frame[ch]));
            }

            const float peakDb = linearToDb(peak);
            const float coeff = (peakDb > envelopeDb_) ? attackCoeff_ : releaseCoeff_;
            envelopeDb_ = coeff * envelopeDb_ + (1.0f - coeff) * peakDb;

            const float gainDb = computeGainReduction(envelopeDb_);
            const float gainLinear = dbToLinear(gainDb) * makeupLinear_;

            for (uint32_t ch = 0; ch < numChannels; ++ch) {
                frame[ch] *= gainLinear;
            }
        }
    }

private:
    static float dbToLinear(float db) { return std::pow(10.0f, db / 20.0f); }
    static float linearToDb(float lin) {
        return 20.0f * std::log10(std::max(lin, 1e-9f));
    }

    float computeCoeff(float timeMs) const {
        // Coeficiente de suavizado exponencial (one-pole) por muestra.
        return std::exp(-1.0f / (0.001f * timeMs * static_cast<float>(sampleRate_)));
    }

    // Curva estática de compresión con knee suave (cuadrática en la región
    // del knee), devuelve la reducción de ganancia en dB (<= 0).
    float computeGainReduction(float inputDb) const {
        const float thr = params_.thresholdDb;
        const float knee = params_.kneeDb;
        const float ratio = params_.ratio;

        float overDb;
        if (inputDb < thr - knee / 2.0f) {
            return 0.0f; // Por debajo del knee: sin compresión.
        } else if (inputDb > thr + knee / 2.0f) {
            overDb = inputDb - thr;
            return -(overDb - overDb / ratio);
        } else {
            // Región de knee suave: interpolación cuadrática.
            const float x = inputDb - thr + knee / 2.0f;
            overDb = (x * x) / (2.0f * knee);
            return -(overDb - overDb / ratio);
        }
    }

    double sampleRate_ = 48000.0;
    CompressorParams params_{};
    float attackCoeff_ = 0.0f;
    float releaseCoeff_ = 0.0f;
    float makeupLinear_ = 1.0f;
    float envelopeDb_ = -100.0f;
};

} // namespace audiopt
