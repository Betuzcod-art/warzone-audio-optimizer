// =============================================================================
// WarzoneAudioChain.h
// -----------------------------------------------------------------------------
// Cadena DSP multibanda aplicada por bloque:
//
//   x -> HPF 80Hz (retumbo/DC)
//     -> doble crossover complementario, TRES bandas:
//          - BAJA   (<700Hz)      motores, helicópteros, cuerpo de explosiones
//          - MEDIA  (700Hz-5kHz)  pasos, recargas, voz, ropa/equipo -- casi
//                                 toda la información direccional útil
//          - AGUDA  (>5kHz)       brillo metálico: cajas de botín, cristales
//     -> banda BAJA:  compresor dedicado "vehicleTamer_" (ataque lento,
//                     release largo) que SOLO agacha esa banda cuando hay
//                     energía sostenida/fuerte (motor, explosión), dejando
//                     graves normales de música/ambiente intactos.
//     -> banda MEDIA: realce peaking en frecuencias de pasos, más
//                     "footstepBooster_" (realce DINAMICO: sube el volumen
//                     solo cuando la banda está floja -- típico de un paso
//                     lejano -- y se apaga cuando está fuerte -- típico de
//                     un disparo, que comparte la misma banda de frecuencia
//                     pero con mucha más energía absoluta), más un
//                     ensanchado Mid/Side (acentúa L-R) para que la
//                     dirección del paso se perciba más nítida.
//     -> banda AGUDA: solo una ganancia (normalmente atenuación). Esta banda
//                     se separó precisamente para que el realce dinámico NO
//                     la toque: el tintineo de una caja de botín es, para el
//                     detector, indistinguible de un paso (flojo +
//                     transitorio repetido), así que antes se realzaba igual
//                     y acababa tapando lo que queríamos oír.
//     -> suma de las tres bandas
//     -> compresor general "compressor_" (controla picos de disparos)
//     -> trim de salida + clamp anti-clipping
//
// Los crossovers son "por resta": media = (x - baja) filtrada, aguda = resto.
// Eso garantiza que las tres bandas sumen exactamente x si ninguna se toca,
// sin importar la pendiente del filtro -- así no hay huecos ni duplicaciones
// de energía en
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
// a nivel de sistema (loopback), igual que un EQ de hardware externo. Los
// biquads y los compresores aplican la MISMA ganancia a ambos canales en
// cada muestra (el compresor detecta sobre el máximo entre canales), así que
// no distorsionan la dirección original del juego. La ÚNICA excepción
// deliberada es el ensanchado Mid/Side de la banda de pasos (ver más abajo):
// ese sí trata L y R distinto a propósito, para ACENTUAR la diferencia
// inter-canal que ya trae el juego, no para inventar una nueva.
// =============================================================================
#pragma once

#include "BiquadFilter.h"
#include "DynamicsCompressor.h"
#include "UpwardBooster.h"
#include <atomic>
#include <algorithm>
#include <vector>
#include <cstdint>

#if defined(__AVX2__)
#include <immintrin.h>
#endif

namespace audiopt {

struct ChannelFilters {
    BiquadFilter highPass;          // 80Hz, retumbo/DC fuera de todas las bandas
    BiquadFilter lowCrossover;      // ~700Hz LowPass: extrae la banda de vehículos/explosiones
    BiquadFilter airCrossover;      // ~5kHz LowPass: separa pasos (abajo) de brillo/cajas (arriba)
    BiquadFilter footstepDefinition;// Peaking ~2.2kHz: cuerpo/definición del paso
    BiquadFilter footstepPresence;  // Peaking ~3.6kHz: presencia de pasos/recargas
};

class WarzoneAudioChain {
public:
    // Reserva por defecto: cubre de sobra los bloques típicos (64-1056
    // frames). El APO la ajusta al máximo real vía reserve().
    static constexpr size_t kDefaultScratchFrames = 4096;

    void prepare(double sampleRate, uint32_t numChannels) {
        sampleRate_ = sampleRate;
        numChannels_ = numChannels;
        channelFilters_.assign(numChannels, ChannelFilters{});

        // Los cortes se leen de los valores vigentes (no hardcodeados) para
        // que un prepare() posterior -- p.ej. tras cambiar de dispositivo --
        // no pise los ajustes que el usuario haya hecho.
        appliedLowCrossoverHz_ = pendingLowCrossoverHz_.load(std::memory_order_relaxed);
        appliedAirCrossoverHz_ = pendingAirCrossoverHz_.load(std::memory_order_relaxed);

        for (auto& cf : channelFilters_) {
            cf.highPass.setSampleRate(sampleRate);
            cf.highPass.configure(BiquadType::HighPass, /*freq*/ 80.0, /*Q*/ 0.707);

            // Corte BAJO (700Hz por defecto): el "cuerpo" de motores,
            // hélices y el retumbo de granadas vive por debajo de aquí.
            cf.lowCrossover.setSampleRate(sampleRate);
            cf.lowCrossover.configure(BiquadType::LowPass, appliedLowCrossoverHz_, /*Q*/ 0.707);

            // Corte ALTO (5kHz por defecto): separa la banda de PASOS del
            // BRILLO. El tintineo de las cajas de botín vive arriba de este
            // corte, y acústicamente es casi idéntico a un paso para el
            // realce dinámico (flojo + transitorio repetido), así que el
            // booster lo subía igual. Separando la banda, el realce ya no
            // puede tocarlo -- y de paso se puede atenuar aparte.
            cf.airCrossover.setSampleRate(sampleRate);
            cf.airCrossover.configure(BiquadType::LowPass, appliedAirCrossoverHz_, /*Q*/ 0.707);

            cf.footstepDefinition.setSampleRate(sampleRate);
            cf.footstepDefinition.configure(BiquadType::Peaking, /*freq*/ 2200.0,
                                             /*Q*/ 0.9, /*gainDb*/ 1.5);

            cf.footstepPresence.setSampleRate(sampleRate);
            // Realce de presencia: rango donde viven pasos, recargas de arma
            // y movimiento de tela/equipo de otros jugadores. Ganancia
            // moderada a propósito: esta banda tambien la ocupan los
            // disparos (con mas energia absoluta que un paso), asi que un
            // realce ESTATICO grande los sube a ellos tambien -- lo probamos
            // y empeoro. El realce fuerte de verdad ahora lo hace
            // footstepBooster_ (dinamico, ver mas abajo), que solo actua
            // cuando esta banda esta baja de nivel (tipico de un paso), no
            // cuando ya esta fuerte (tipico de un disparo).
            cf.footstepPresence.configure(BiquadType::Peaking, /*freq*/ 3600.0,
                                           /*Q*/ 1.1, /*gainDb*/ 4.0);

            // NOTA: aquí había un tercer peaking de "aire" a 6.5kHz. Se
            // quitó: esa banda es justo donde brilla el metal de las cajas
            // de botín, y realzarla ensuciaba la mezcla sin aportar a los
            // pasos. Ahora esa zona va por su propia banda (airBand_) con
            // ganancia controlable por el usuario, normalmente atenuada.
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

        // Realce dinámico de pasos: sube hasta 7dB la banda alta SOLO cuando
        // pasan DOS cosas a la vez: (1) está floja -- no es un disparo/grito
        // -- y (2) acaba de aparecer -- no es ambiente sostenido (viento,
        // room tone) que ya llevaba rato sonando igual de bajo. La condición
        // (2) es la que evita que subir los pasos también suba "el sonido
        // del mundo": un paso es un evento nuevo, el ambiente no.
        UpwardBoosterParams boosterParams;
        boosterParams.quietThresholdDb = -36.0f;
        boosterParams.loudThresholdDb = -18.0f;
        boosterParams.maxBoostDb = 7.0f;
        // El fondo debe ser MUCHO más lento que la duración de un paso
        // (~100-200ms) para no "alcanzarlo" a mitad de camino y cortar el
        // boost antes de tiempo -- eso es lo que sonaba sucio/cortado.
        boosterParams.backgroundMs = 450.0f;
        boosterParams.onsetThresholdDb = 3.0f;
        boosterParams.attackMs = 3.0f;
        boosterParams.releaseMs = 200.0f;
        footstepBooster_.prepare(sampleRate, boosterParams);

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

        reserve(kDefaultScratchFrames);

        // Sincroniza los "pendientes" con lo que acabamos de configurar,
        // para que un prepare() posterior no revierta ajustes del usuario
        // ni al revés.
        pendingFootstepBoostDb_.store(boosterParams.maxBoostDb, std::memory_order_relaxed);
        pendingVehicleThresholdDb_.store(vehicleParams.thresholdDb, std::memory_order_relaxed);
    }

    // Limpia el estado interno (filtros, detectores) SIN reconfigurar nada
    // ni reservar memoria. Es lo que debe hacer un "reset" de audio: olvidar
    // la historia de la señal tras una discontinuidad, no reinicializarse.
    // Seguro de llamar desde el hilo de audio.
    void resetState() {
        for (auto& cf : channelFilters_) {
            cf.highPass.reset();
            cf.lowCrossover.reset();
            cf.airCrossover.reset();
            cf.footstepDefinition.reset();
            cf.footstepPresence.reset();
        }
        vehicleTamer_.resetState();
        footstepBooster_.resetState();
        compressor_.resetState();
        peakLimiter_.resetState();
    }

    // Reserva los buffers internos para bloques de hasta maxFrames. DEBE
    // llamarse desde fuera del hilo de audio (prepare/LockForProcess): es
    // el único punto de la clase que reserva memoria.
    void reserve(size_t maxFrames) {
        const size_t samples = maxFrames * numChannels_;
        lowBand_.assign(samples, 0.0f);
        highBand_.assign(samples, 0.0f);
        airBand_.assign(samples, 0.0f);
    }

    // Procesa un bloque interleaved in-place. Pensado para bloques de
    // 64-128 frames (1.3-2.6ms a 48kHz) para cumplir el objetivo <5ms.
    //
    // NUNCA reserva memoria: se ejecuta en el hilo de audio y, dentro de un
    // APO, corre en audiodg.exe bajo tiempo real estricto, donde reservar
    // (o cualquier llamada bloqueante) puede provocar cortes. Si llega un
    // bloque mayor que lo reservado, se procesa lo que cabe en vez de
    // crecer el buffer; llama a reserve() con el tamaño máximo real antes
    // de empezar a procesar.
    void process(float* interleaved, size_t numFrames) {
        if (bypass_) return;

        applyPendingParams();

        const size_t capacityFrames = numChannels_ > 0 ? lowBand_.size() / numChannels_ : 0;
        if (numFrames > capacityFrames) numFrames = capacityFrames;
        if (numFrames == 0) return;

        const size_t total = numFrames * numChannels_;

        // 1) HPF + doble crossover complementario, por canal, muestra a
        //    muestra. Tres bandas: graves (vehículos), medios (pasos) y
        //    agudos (brillo/cajas). Las restas garantizan que las tres
        //    sumen exactamente la señal original si no se toca ninguna.
        for (size_t i = 0; i < numFrames; ++i) {
            const float* frame = &interleaved[i * numChannels_];
            float* lowFrame = &lowBand_[i * numChannels_];
            float* highFrame = &highBand_[i * numChannels_];
            float* airFrame = &airBand_[i * numChannels_];
            for (uint32_t ch = 0; ch < numChannels_; ++ch) {
                auto& cf = channelFilters_[ch];
                const float x = cf.highPass.processSample(frame[ch]);
                const float low = cf.lowCrossover.processSample(x);
                const float rest = x - low;              // todo lo que está sobre 700Hz
                const float mid = cf.airCrossover.processSample(rest);  // 700Hz-5kHz: pasos
                lowFrame[ch] = low;
                airFrame[ch] = rest - mid;               // >5kHz: brillo, cajas, metal
                highFrame[ch] = cf.footstepPresence.processSample(
                    cf.footstepDefinition.processSample(mid));
            }
        }

        // 2) Agacha la banda baja SOLO cuando hay energía sostenida fuerte
        //    (motor/helicóptero/explosión), sin tocar la banda de pasos.
        vehicleTamer_.processBlock(lowBand_.data(), numFrames, numChannels_);

        // 2b) Sube la banda de PASOS solo cuando está floja (paso) y la deja
        //     intacta cuando ya está fuerte (disparo). Al operar sobre la
        //     banda media -- no sobre todo lo agudo -- el tintineo de las
        //     cajas ya no entra aquí: vive en airBand_, fuera de su alcance.
        footstepBooster_.processBlock(highBand_.data(), numFrames, numChannels_);

        // 2c) Ensancha el estéreo SOLO en la banda de pasos. La dirección de
        //     un sonido se percibe sobre todo por la diferencia de nivel
        //     entre L y R (ILD), y esa diferencia vive justo en esta banda.
        //     Acentuarla aquí hace la dirección de un paso más nítida sin
        //     tocar la banda baja, donde ensanchar rompe la compatibilidad
        //     mono/fase (por eso NO se aplica a lowBand_ ni a la mezcla).
        applyMidSideWidth(highBand_.data(), numFrames, numChannels_, stereoWidth_);

        // 3) Recompone: graves atenuados + pasos realzados + agudos con la
        //    ganancia que el usuario haya elegido (normalmente atenuados,
        //    para que las cajas y el metal no tapen los pasos).
        for (size_t i = 0; i < total; ++i) {
            interleaved[i] = lowBand_[i] + highBand_[i] + airBand_[i] * airGain_;
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

    // -------------------------------------------------------------------
    // Controles en vivo (llamados desde el hilo de UI mientras el hilo de
    // audio procesa). No tocan los objetos DSP directamente: guardan el
    // valor en un atómico y el hilo de audio lo aplica al inicio del
    // siguiente bloque (applyPendingParams). Así el hilo de render nunca
    // lee un parámetro a medio escribir.
    // -------------------------------------------------------------------

    // Cuánto se realzan los pasos, en dB (0 = sin realce).
    void setFootstepBoostDb(float db) {
        pendingFootstepBoostDb_.store(db, std::memory_order_relaxed);
    }
    float footstepBoostDb() const {
        return pendingFootstepBoostDb_.load(std::memory_order_relaxed);
    }

    // Umbral del compresor de la banda baja: más negativo = agacha más
    // motores/helicópteros/explosiones.
    void setVehicleThresholdDb(float db) {
        pendingVehicleThresholdDb_.store(db, std::memory_order_relaxed);
    }
    float vehicleThresholdDb() const {
        return pendingVehicleThresholdDb_.load(std::memory_order_relaxed);
    }

    // Ancho estéreo de la banda de pasos: 1.0 = sin cambio, >1 ensancha.
    void setStereoWidth(float width) {
        pendingStereoWidth_.store(width, std::memory_order_relaxed);
    }
    float stereoWidth() const {
        return pendingStereoWidth_.load(std::memory_order_relaxed);
    }

    // Ganancia de la banda de agudos (>5kHz) en dB: donde vive el tintineo
    // de las cajas de botín y el brillo metálico. Negativo = más apagado.
    void setAirGainDb(float db) {
        pendingAirGainDb_.store(db, std::memory_order_relaxed);
    }
    float airGainDb() const {
        return pendingAirGainDb_.load(std::memory_order_relaxed);
    }

    // Dónde EMPIEZA la banda de pasos: por debajo de esta frecuencia manda
    // el compresor de vehículos/explosiones.
    void setLowCrossoverHz(float hz) {
        pendingLowCrossoverHz_.store(hz, std::memory_order_relaxed);
    }
    float lowCrossoverHz() const {
        return pendingLowCrossoverHz_.load(std::memory_order_relaxed);
    }

    // Dónde TERMINA la banda de pasos: por encima de esta frecuencia manda
    // el control de brillo/cajas y el realce dinámico ya no llega.
    void setAirCrossoverHz(float hz) {
        pendingAirCrossoverHz_.store(hz, std::memory_order_relaxed);
    }
    float airCrossoverHz() const {
        return pendingAirCrossoverHz_.load(std::memory_order_relaxed);
    }

    void setOutputTrimDb(float db) {
        pendingOutputTrimDb_.store(db, std::memory_order_relaxed);
    }
    float outputTrimDb() const {
        return pendingOutputTrimDb_.load(std::memory_order_relaxed);
    }

    void setBypass(bool bypass) { bypass_ = bypass; }

private:
    // Lee los parámetros que la UI haya dejado pendientes y los aplica a
    // los objetos DSP. Se llama desde el hilo de audio, al inicio de cada
    // bloque -- nunca desde la UI.
    void applyPendingParams() {
        const float boostDb = pendingFootstepBoostDb_.load(std::memory_order_relaxed);
        if (boostDb != footstepBooster_.maxBoostDb()) {
            footstepBooster_.setMaxBoostDb(boostDb);
        }

        const float vehicleDb = pendingVehicleThresholdDb_.load(std::memory_order_relaxed);
        if (vehicleDb != vehicleTamer_.thresholdDb()) {
            vehicleTamer_.setThresholdDb(vehicleDb);
        }

        stereoWidth_ = pendingStereoWidth_.load(std::memory_order_relaxed);

        const float airDb = pendingAirGainDb_.load(std::memory_order_relaxed);
        if (airDb != appliedAirGainDb_) {
            appliedAirGainDb_ = airDb;
            airGain_ = std::pow(10.0f, airDb / 20.0f);
        }

        const float trimDb = pendingOutputTrimDb_.load(std::memory_order_relaxed);
        if (trimDb != appliedOutputTrimDb_) {
            appliedOutputTrimDb_ = trimDb;
            outputTrimLinear_ = std::pow(10.0f, trimDb / 20.0f);
        }

        // Frecuencias de corte: reconfigurar un biquad es más caro que
        // mover una ganancia (recalcula coeficientes con transcendentales),
        // así que solo se hace cuando el valor realmente cambió. Los saltos
        // grandes de golpe pueden dar un click leve porque el estado interno
        // del filtro queda momentáneamente desfasado de los coeficientes
        // nuevos; arrastrando el slider los deltas son mínimos y no se nota.
        const float lowHz = pendingLowCrossoverHz_.load(std::memory_order_relaxed);
        const float airHz = pendingAirCrossoverHz_.load(std::memory_order_relaxed);
        if (lowHz != appliedLowCrossoverHz_ || airHz != appliedAirCrossoverHz_) {
            appliedLowCrossoverHz_ = lowHz;
            appliedAirCrossoverHz_ = airHz;
            for (auto& cf : channelFilters_) {
                cf.lowCrossover.configure(BiquadType::LowPass, lowHz, /*Q*/ 0.707);
                cf.airCrossover.configure(BiquadType::LowPass, airHz, /*Q*/ 0.707);
            }
        }
    }

    // Codificación Mid/Side: mid=(L+R)/2 no se toca, side=(L-R)/2 se
    // amplifica. Solo tiene sentido en estéreo -- en cualquier otro conteo
    // de canales no hace nada (no hay "L-R" que ensanchar).
    static void applyMidSideWidth(float* data, size_t numFrames, uint32_t numChannels,
                                   float widthGain) {
        if (numChannels != 2) return;
        for (size_t i = 0; i < numFrames; ++i) {
            float* frame = &data[i * 2];
            const float mid = 0.5f * (frame[0] + frame[1]);
            const float side = 0.5f * (frame[0] - frame[1]) * widthGain;
            frame[0] = mid + side;
            frame[1] = mid - side;
        }
    }

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
    UpwardBooster footstepBooster_;    // Sube la banda alta SOLO cuando está floja (pasos)
    DynamicsCompressor compressor_;    // Control general de picos (disparos)
    DynamicsCompressor peakLimiter_;   // Red de seguridad final anti-sobrepico
    std::vector<float> lowBand_;
    std::vector<float> highBand_;
    std::vector<float> airBand_;
    float outputTrimLinear_ = 1.0f;
    float appliedOutputTrimDb_ = 0.0f;
    float airGain_ = 1.0f;
    float appliedAirGainDb_ = 0.0f;
    float appliedLowCrossoverHz_ = 700.0f;
    float appliedAirCrossoverHz_ = 5000.0f;
    // Ganancia del canal "side" (L-R) de la banda de pasos. 1.3 por defecto:
    // ensanchar de más suena artificial y desestabiliza la imagen en vez de
    // aclararla.
    float stereoWidth_ = 1.3f;
    bool bypass_ = false;

    // Valores que la UI deja pendientes; los aplica applyPendingParams()
    // desde el hilo de audio.
    std::atomic<float> pendingFootstepBoostDb_{7.0f};
    std::atomic<float> pendingVehicleThresholdDb_{-40.0f};
    std::atomic<float> pendingStereoWidth_{1.3f};
    std::atomic<float> pendingAirGainDb_{0.0f};
    std::atomic<float> pendingOutputTrimDb_{0.0f};
    std::atomic<float> pendingLowCrossoverHz_{700.0f};
    std::atomic<float> pendingAirCrossoverHz_{5000.0f};
};

} // namespace audiopt
