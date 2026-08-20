// =============================================================================
// WarzoneAudioChain.h
// -----------------------------------------------------------------------------
// Cadena DSP multibanda aplicada por bloque:
//
//   x -> HPF 80Hz (retumbo/DC)
//     -> crossover complementario a ~400Hz:
//          - banda BAJA  (motores, helicópteros, cuerpo de explosiones)
//          - banda ALTA  (pasos, recargas, voz, ropa/equipo -- casi toda
//                         la información direccional que importa en combate)
//     -> banda BAJA:  compresor dedicado "vehicleTamer_" (ataque lento,
//                     release largo) que SOLO agacha esa banda cuando hay
//                     energía sostenida/fuerte (motor, explosión), dejando
//                     graves normales de música/ambiente intactos.
//     -> banda ALTA:  realce peaking en las frecuencias de pasos/pisadas
//                     y una banda extra de "aire" para recargas/ropa.
//     -> suma banda BAJA (ya atenuada) + banda ALTA (ya realzada)
//     -> compresor general "compressor_" (controla picos de disparos)
//     -> trim de salida + clamp anti-clipping
//
// El crossover es "por resta": bandaAlta = x - bandaBaja. Eso garantiza que
// bandaBaja + bandaAlta == x exactamente si ninguna se toca, sin importar la
// pendiente del filtro -- así no hay huecos ni duplicaciones de energía en
// la frecuencia de corte al recomponer la señal.
//
// Los biquads se ejecutan muestra a muestra (son recursivos, no vectorizables
// directamente sin romper el orden). El paso final de aplicar ganancia/makeup
// a todo el bloque (p.ej. un "output trim" del usuario) SÍ se vectoriza con
// AVX2 porque es una multiplicación elemento a elemento independiente por
// muestra.
//
// NOTA IMPORTANTE (fidelidad posicional):
// Esta cadena se aplica DESPUÉS de que el juego ya mezcló su audio 3D/HRTF a
// estéreo (o al feed de auriculares). No se toca el buffer de paneo interno
// del motor del juego -- este proceso opera como un "insert" de post-render
// a nivel de sistema (loopback), igual que un EQ de hardware externo. Tanto
// los biquads como los compresores aplican la MISMA ganancia a ambos canales
// en cada muestra (el compresor detecta sobre el máximo entre canales), así
// que las diferencias inter-canal (ITD/ILD) que dan la dirección del sonido
// nunca se tocan de forma distinta entre L y R.
// =============================================================================
#pragma once

#include "BiquadFilter.h"
#include "DynamicsCompressor.h"
#include <algorithm>
#include <vector>
#include <cstdint>

#if defined(__AVX2__)
#include <immintrin.h>
#endif

namespace audiopt {

struct ChannelFilters {
    BiquadFilter highPass;          // 80Hz, retumbo/DC fuera de ambas bandas
    BiquadFilter lowCrossover;      // ~400Hz LowPass: extrae la banda de vehículos/explosiones
    BiquadFilter footstepDefinition;// Peaking ~2.2kHz: cuerpo/definición del paso
    BiquadFilter footstepPresence;  // Peaking ~3.6kHz: presencia de pasos/recargas
    BiquadFilter footstepAir;       // Peaking ~6.5kHz: aire/detalle (ropa, respiración, recarga)
};

class WarzoneAudioChain {
public:
    void prepare(double sampleRate, uint32_t numChannels) {
        sampleRate_ = sampleRate;
        numChannels_ = numChannels;
        channelFilters_.assign(numChannels, ChannelFilters{});

        for (auto& cf : channelFilters_) {
            cf.highPass.setSampleRate(sampleRate);
            cf.highPass.configure(BiquadType::HighPass, /*freq*/ 80.0, /*Q*/ 0.707);

            cf.lowCrossover.setSampleRate(sampleRate);
            // Subido de 400 a 700Hz: el "cuerpo" de motores/hélices/turbinas
            // y el retumbo de granadas/explosiones se extiende bastante más
            // arriba de 400Hz -- con el corte bajo se nos escapaba la mayor
            // parte de esa energía sin tocar.
            cf.lowCrossover.configure(BiquadType::LowPass, /*freq*/ 700.0, /*Q*/ 0.707);

            cf.footstepDefinition.setSampleRate(sampleRate);
            cf.footstepDefinition.configure(BiquadType::Peaking, /*freq*/ 2200.0,
                                             /*Q*/ 0.9, /*gainDb*/ 1.5);

            cf.footstepPresence.setSampleRate(sampleRate);
            // Realce de presencia: rango donde viven pasos, recargas de arma
            // y movimiento de tela/equipo de otros jugadores. Ganancia
            // moderada a propósito: subir mucho esta banda también sube el
            // ruido de fondo de vehículos/disparos que cae en el mismo
            // rango -- el trabajo de "bajar el fondo" lo hacen los
            // compresores, no este realce.
            cf.footstepPresence.configure(BiquadType::Peaking, /*freq*/ 3600.0,
                                           /*Q*/ 1.1, /*gainDb*/ 4.0);

            cf.footstepAir.setSampleRate(sampleRate);
            // Aire/detalle: matices de recarga, ropa y respiración que
            // ayudan a distinguir pasos reales de ruido de fondo.
            cf.footstepAir.configure(BiquadType::Peaking, /*freq*/ 6500.0,
                                      /*Q*/ 1.0, /*gainDb*/ 2.0);
        }

        // Compresor dedicado a la banda BAJA (motores/hélices/cuerpo de
        // explosiones/granadas): umbral bajo y ratio alto a propósito, casi
        // limitando -- el objetivo es que CUALQUIER energía sostenida en esa
        // banda quede notablemente más baja, no solo los picos extremos.
        // Ataque algo lento para no reaccionar a un thump grave puntual de
        // un paso pesado; release largo para no "bombear" con el zumbido
        // sostenido de un vehículo/helicóptero. makeupGainDb en 0: el
        // objetivo es que suene más bajo, no devolverle el volumen.
        CompressorParams vehicleParams;
        vehicleParams.thresholdDb = -40.0f;
        vehicleParams.ratio = 9.0f;
        vehicleParams.attackMs = 6.0f;
        vehicleParams.releaseMs = 220.0f;
        vehicleParams.kneeDb = 6.0f;
        vehicleParams.makeupGainDb = 0.0f;
        vehicleTamer_.prepare(sampleRate, vehicleParams);

        // Compresor general: controla picos de explosiones/disparos sobre la
        // mezcla ya recompuesta (llegan también con energía en la banda
        // alta, donde el crossover no los agarra). Umbral bajo + ratio alto
        // para aplanar de verdad los eventos fuertes, no solo suavizarlos.
        CompressorParams cp;
        cp.thresholdDb = -20.0f;
        cp.ratio = 6.0f;
        cp.attackMs = 1.5f;
        cp.releaseMs = 130.0f;
        cp.kneeDb = 4.0f;
        cp.makeupGainDb = 0.0f;
        compressor_.prepare(sampleRate, cp);

        // Limitador final de seguridad: atrapa cualquier pico residual antes
        // del clamp anti-clipping, para que un sobrepico se "aplane" en vez
        // de recortarse en seco (que suena a distorsión).
        CompressorParams lp;
        lp.thresholdDb = -6.0f;
        lp.ratio = 20.0f;
        lp.attackMs = 0.5f;
        lp.releaseMs = 60.0f;
        lp.kneeDb = 1.0f;
        lp.makeupGainDb = 0.0f;
        peakLimiter_.prepare(sampleRate, lp);

        const size_t scratchFrames = 4096;
        lowBand_.assign(scratchFrames * numChannels_, 0.0f);
        highBand_.assign(scratchFrames * numChannels_, 0.0f);
    }

    // Procesa un bloque interleaved in-place. Pensado para bloques de
    // 64-128 frames (1.3-2.6ms a 48kHz) para cumplir el objetivo <5ms.
    void process(float* interleaved, size_t numFrames) {
        if (bypass_) return;

        const size_t total = numFrames * numChannels_;
        if (lowBand_.size() < total) {
            lowBand_.resize(total, 0.0f);
            highBand_.resize(total, 0.0f);
        }

        // 1) HPF + crossover complementario, por canal, muestra a muestra.
        for (size_t i = 0; i < numFrames; ++i) {
            const float* frame = &interleaved[i * numChannels_];
            float* lowFrame = &lowBand_[i * numChannels_];
            float* highFrame = &highBand_[i * numChannels_];
            for (uint32_t ch = 0; ch < numChannels_; ++ch) {
                auto& cf = channelFilters_[ch];
                const float x = cf.highPass.processSample(frame[ch]);
                const float low = cf.lowCrossover.processSample(x);
                const float high = x - low;
                lowFrame[ch] = low;
                highFrame[ch] = cf.footstepAir.processSample(
                    cf.footstepPresence.processSample(
                        cf.footstepDefinition.processSample(high)));
            }
        }

        // 2) Agacha la banda baja SOLO cuando hay energía sostenida fuerte
        //    (motor/helicóptero/explosión), sin tocar la banda de pasos.
        vehicleTamer_.processBlock(lowBand_.data(), numFrames, numChannels_);

        // 3) Recompone la señal: banda baja atenuada + banda alta realzada.
        for (size_t i = 0; i < total; ++i) {
            interleaved[i] = lowBand_[i] + highBand_[i];
        }

        // 4) Compresor general: controla picos de explosiones/disparos,
        //    misma ganancia aplicada a todos los canales por muestra
        //    (preserva diferencias inter-canal del posicionamiento 3D nativo).
        compressor_.processBlock(interleaved, numFrames, numChannels_);

        // 5) Trim final vectorizado (ejemplo de aplicación SIMD explícita;
        //    útil si se añade un control de ganancia de salida por usuario).
        applyOutputTrimSimd(interleaved, total, outputTrimLinear_);

        // 6) Limitador de picos: última red de seguridad antes del clamp.
        peakLimiter_.processBlock(interleaved, numFrames, numChannels_);

        // Evita que la suma de EQ y makeup gain produzca clipping digital.
        for (size_t i = 0; i < total; ++i) {
            interleaved[i] = std::clamp(interleaved[i], -0.98f, 0.98f);
        }
    }

    void setOutputTrimDb(float db) {
        outputTrimLinear_ = std::pow(10.0f, db / 20.0f);
    }

    void setBypass(bool bypass) { bypass_ = bypass; }

private:
    // Multiplicación de bloque vectorizada AVX2 (8 floats/iteración), con
    // fallback escalar automático si el hardware no soporta AVX2 o el
    // tamaño no es múltiplo de 8.
    static void applyOutputTrimSimd(float* data, size_t count, float gain) {
#if defined(__AVX2__)
        const __m256 gainVec = _mm256_set1_ps(gain);
        size_t i = 0;
        const size_t simdEnd = count - (count % 8);
        for (; i < simdEnd; i += 8) {
            __m256 v = _mm256_loadu_ps(&data[i]);
            v = _mm256_mul_ps(v, gainVec);
            _mm256_storeu_ps(&data[i], v);
        }
        for (; i < count; ++i) data[i] *= gain;
#else
        for (size_t i = 0; i < count; ++i) data[i] *= gain;
#endif
    }

    double sampleRate_ = 48000.0;
    uint32_t numChannels_ = 2;
    std::vector<ChannelFilters> channelFilters_;
    DynamicsCompressor vehicleTamer_;  // Agacha la banda baja (motores/explosiones)
    DynamicsCompressor compressor_;    // Control general de picos (disparos)
    DynamicsCompressor peakLimiter_;   // Red de seguridad final anti-sobrepico
    std::vector<float> lowBand_;
    std::vector<float> highBand_;
    float outputTrimLinear_ = 1.0f;
    bool bypass_ = false;
};

} // namespace audiopt
