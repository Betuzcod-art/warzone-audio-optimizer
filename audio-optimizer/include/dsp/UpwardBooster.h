// =============================================================================
// UpwardBooster.h
// -----------------------------------------------------------------------------
// Lo opuesto a un compresor: en vez de bajar lo fuerte, sube lo flojo. Se usa
// sobre la banda de pasos/presencia para hacerlos más audibles SIN subir
// también los disparos que comparten esa misma banda de frecuencia (un
// disparo llega con mucha más energía absoluta que un paso lejano, así que
// un realce estático de EQ sube ambos por igual -- probado, suena peor).
//
// SEGUNDA COMPUERTA (transitorio/onset): subir "todo lo flojo" también sube
// el ambiente sostenido (viento, room tone, zumbido de fondo) que está en el
// mismo rango de volumen que un paso lejano -- probado, se nota. No podemos
// separar el paso del "sonido del mundo" como fuentes independientes (solo
// tenemos la mezcla final del loopback, no las pistas originales del juego),
// pero SÍ podemos distinguirlos por comportamiento: un paso es un evento que
// aparece de golpe; el ambiente sostenido lleva rato sonando igual. Por eso
// llevamos, además del nivel instantáneo, un seguidor de "fondo reciente"
// mucho más lento (backgroundMs). Cuando el nivel instantáneo se despega
// claramente de ese fondo (onset > onsetThresholdDb), es que apareció algo
// nuevo -> boost completo. Si el nivel instantáneo y el fondo van parejos
// (nada nuevo, es solo ambiente sostenido), el boost se apaga aunque esté
// flojo.
//
// Curva de nivel: por debajo de quietThresholdDb, ganancia máxima
// (+maxBoostDb). Por encima de loudThresholdDb, ganancia 0 (ahí caen los
// disparos). Esa ganancia se multiplica por la "compuerta" de onset descrita
// arriba, así que el boost real solo aparece cuando AMBAS condiciones se
// cumplen: está flojo Y es un evento nuevo.
//
// Ataque rápido = el boost se apaga casi de inmediato apenas algo se pone
// fuerte (antes de que el disparo alcance a sonar más alto). Release más
// lento = el boost vuelve suave tras el evento fuerte, sin pumping audible.
// =============================================================================
#pragma once

#include <cmath>
#include <algorithm>

namespace audiopt {

struct UpwardBoosterParams {
    float quietThresholdDb = -36.0f;  // por debajo de esto: boost máximo
    float loudThresholdDb = -18.0f;   // por encima de esto: boost cero
    float maxBoostDb = 6.0f;
    float attackMs = 3.0f;
    float releaseMs = 200.0f;

    // Compuerta de onset (distingue "evento nuevo" de "ambiente sostenido").
    float backgroundMs = 150.0f;      // qué tan lento es el seguidor de fondo
    float onsetThresholdDb = 5.0f;    // cuánto debe destacar sobre el fondo para contar como algo nuevo
};

class UpwardBooster {
public:
    void prepare(double sampleRate, const UpwardBoosterParams& params) {
        params_ = params;
        attackCoeff_ = computeCoeff(params.attackMs, sampleRate);
        releaseCoeff_ = computeCoeff(params.releaseMs, sampleRate);
        backgroundCoeff_ = computeCoeff(params.backgroundMs, sampleRate);
    }

    // Cambiar solo la ganancia máxima es barato y seguro en caliente: no
    // recalcula ningún coeficiente de suavizado, solo mueve la altura de la
    // curva de boost. Debe llamarse desde el hilo de audio (ver
    // WarzoneAudioChain::applyPendingParams).
    void setMaxBoostDb(float db) { params_.maxBoostDb = db; }
    float maxBoostDb() const { return params_.maxBoostDb; }

    // Limpia los seguidores de nivel sin tocar parámetros ni reservar.
    // Seguro de llamar desde el hilo de audio.
    void resetState() {
        envelopeDb_ = -100.0f;
        backgroundDb_ = -100.0f;
    }

    void processBlock(float* interleaved, size_t numFrames, uint32_t numChannels) {
        for (size_t i = 0; i < numFrames; ++i) {
            float* frame = &interleaved[i * numChannels];

            float peak = 0.0f;
            for (uint32_t ch = 0; ch < numChannels; ++ch) {
                peak = std::max(peak, std::fabs(frame[ch]));
            }

            const float peakDb = linearToDb(peak);
            // Ataque cuando el nivel SUBE (para apagar el boost rápido);
            // release cuando el nivel BAJA (para recuperar el boost suave).
            const float coeff = (peakDb > envelopeDb_) ? attackCoeff_ : releaseCoeff_;
            envelopeDb_ = coeff * envelopeDb_ + (1.0f - coeff) * peakDb;

            // Fondo/ambiente: persigue al envolvente rápido pero mucho más
            // despacio -- representa "lo que ha sonado normalmente hace
            // poco", no el instante actual. Mientras dura un paso (~100-
            // 200ms) este seguidor no alcanza a ponerse al día, así que el
            // onset se mantiene alto durante todo el evento sin lógica extra.
            backgroundDb_ = backgroundCoeff_ * backgroundDb_ + (1.0f - backgroundCoeff_) * envelopeDb_;

            const float onsetDb = envelopeDb_ - backgroundDb_;
            const float onsetGate = std::clamp(onsetDb / params_.onsetThresholdDb, 0.0f, 1.0f);

            const float levelBoostDb = computeLevelBoost(envelopeDb_);
            const float boostDb = levelBoostDb * onsetGate;
            const float gain = dbToLinear(boostDb);
            for (uint32_t ch = 0; ch < numChannels; ++ch) {
                frame[ch] *= gain;
            }
        }
    }

private:
    static float dbToLinear(float db) { return std::pow(10.0f, db / 20.0f); }
    static float linearToDb(float lin) {
        return 20.0f * std::log10(std::max(lin, 1e-9f));
    }

    float computeCoeff(float timeMs, double sampleRate) const {
        return std::exp(-1.0f / (0.001f * timeMs * static_cast<float>(sampleRate)));
    }

    float computeLevelBoost(float levelDb) const {
        if (levelDb <= params_.quietThresholdDb) return params_.maxBoostDb;
        if (levelDb >= params_.loudThresholdDb) return 0.0f;
        const float t = (levelDb - params_.quietThresholdDb) /
                         (params_.loudThresholdDb - params_.quietThresholdDb);
        return params_.maxBoostDb * (1.0f - t);
    }

    UpwardBoosterParams params_{};
    float attackCoeff_ = 0.0f;
    float releaseCoeff_ = 0.0f;
    float backgroundCoeff_ = 0.0f;
    float envelopeDb_ = -100.0f;
    float backgroundDb_ = -100.0f;
};

} // namespace audiopt
