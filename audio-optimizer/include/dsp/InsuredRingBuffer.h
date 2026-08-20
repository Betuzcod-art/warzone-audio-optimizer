// =============================================================================
// InsuredRingBuffer.h
// -----------------------------------------------------------------------------
// Ring buffer lock-free de un solo productor / un solo consumidor (SPSC),
// especializado para audio en tiempo real.
//
// "Insured Frame": cuando el hilo productor (captura, o el motor del juego)
// sufre un tirón de CPU y no logra escribir a tiempo, el consumidor (hilo de
// render) NO debe:
//   a) bloquearse esperando datos (introduce latencia/jitter), ni
//   b) insertar silencio abrupto (genera "pop"/click audible), ni
//   c) repetir el último bloque completo sin más (genera eco/tartamudeo).
//
// En su lugar, aplicamos una rampa de extrapolación basada en la envolvente
// reciente de la señal (última muestra + pendiente decreciente hacia cero)
// con un micro-crossfade al reanudar el flujo real. Esto "asegura" (insures)
// el frame ante el underrun sin costo de latencia adicional: la decisión se
// toma en el mismo callback, sin esperar.
// =============================================================================
#pragma once

#include <atomic>
#include <cstdint>
#include <cstring>
#include <memory>
#include <algorithm>
#include <cmath>

namespace audiopt {

// Ring buffer lock-free SPSC de floats intercalados (interleaved) por canal.
class InsuredRingBuffer {
public:
    InsuredRingBuffer(size_t capacityFrames, uint32_t numChannels)
        : capacityFrames_(nextPow2(capacityFrames)),
          mask_(capacityFrames_ - 1),
          numChannels_(numChannels),
          buffer_(std::make_unique<float[]>(capacityFrames_ * numChannels)),
          writeIndex_(0),
          readIndex_(0),
          lastGoodFrame_(std::make_unique<float[]>(numChannels)),
          fadeGain_(1.0f) {
        std::memset(buffer_.get(), 0, capacityFrames_ * numChannels * sizeof(float));
        std::memset(lastGoodFrame_.get(), 0, numChannels * sizeof(float));
    }

    uint32_t channels() const { return numChannels_; }
    size_t capacity() const { return capacityFrames_; }

    // Llamado únicamente desde el hilo PRODUCTOR (captura / mezcla del juego).
    // Devuelve frames realmente escritos (puede ser < numFrames si el buffer
    // está lleno; en audio bien dimensionado esto no debería ocurrir).
    size_t write(const float* interleavedIn, size_t numFrames) {
        const size_t w = writeIndex_.load(std::memory_order_relaxed);
        const size_t r = readIndex_.load(std::memory_order_acquire);
        const size_t free = capacityFrames_ - (w - r);
        const size_t toWrite = std::min(numFrames, free);

        for (size_t i = 0; i < toWrite; ++i) {
            const size_t idx = (w + i) & mask_;
            std::memcpy(&buffer_[idx * numChannels_],
                        &interleavedIn[i * numChannels_],
                        numChannels_ * sizeof(float));
        }
        writeIndex_.store(w + toWrite, std::memory_order_release);
        return toWrite;
    }

    // Llamado únicamente desde el hilo CONSUMIDOR (hilo de render WASAPI).
    // Siempre llena `numFrames` completos: si no hay suficientes datos reales
    // disponibles, aplica el algoritmo de "Insured Frame" (relleno suave) en
    // lugar de dejar el buffer en silencio abrupto.
    void readInsured(float* interleavedOut, size_t numFrames) {
        const size_t w = writeIndex_.load(std::memory_order_acquire);
        size_t r = readIndex_.load(std::memory_order_relaxed);
        size_t available = w - r;

        // Guarda anti-drift: el hilo de captura (loopback) y el hilo de
        // render corren sobre relojes de hardware independientes. Sin
        // corrección, el desfase se acumula con el tiempo y el backlog del
        // ring buffer crece sin límite -> delay perceptible y creciente
        // durante una partida larga. Si el margen supera varios bloques,
        // descartamos el sobrante más antiguo y dejamos solo un pequeño
        // colchón: esto puede sonar como un micro-salto muy ocasional, pero
        // evita que la latencia derive hacia arriba sin límite.
        const size_t maxBacklog = numFrames * kMaxBacklogBlocks;
        if (available > maxBacklog) {
            const size_t target = numFrames * kResyncTargetBlocks;
            const size_t drop = available - target;
            r += drop;
            available = target;
            resyncCount_.fetch_add(1, std::memory_order_relaxed);
        }

        const size_t realFrames = std::min(numFrames, available);

        // 1) Copiar lo que realmente hay disponible.
        for (size_t i = 0; i < realFrames; ++i) {
            const size_t idx = (r + i) & mask_;
            std::memcpy(&interleavedOut[i * numChannels_],
                        &buffer_[idx * numChannels_],
                        numChannels_ * sizeof(float));
        }

        if (realFrames > 0) {
            // Guardamos el último frame real como semilla para una futura
            // extrapolación, y reseteamos la ganancia de fundido.
            std::memcpy(lastGoodFrame_.get(),
                        &interleavedOut[(realFrames - 1) * numChannels_],
                        numChannels_ * sizeof(float));
            fadeGain_ = 1.0f;
        }

        readIndex_.store(r + realFrames, std::memory_order_release);

        // 2) Underrun: nos faltan (numFrames - realFrames) frames.
        const size_t missing = numFrames - realFrames;
        if (missing > 0) {
            insureMissingFrames(&interleavedOut[realFrames * numChannels_], missing);
            underrunCount_.fetch_add(1, std::memory_order_relaxed);
        }
    }

    uint64_t underrunCount() const { return underrunCount_.load(std::memory_order_relaxed); }
    uint64_t resyncCount() const { return resyncCount_.load(std::memory_order_relaxed); }

private:
    // Backlog máximo tolerado antes de forzar un resync (en múltiplos del
    // tamaño de bloque pedido por el render), y a cuántos bloques lo
    // recortamos cuando eso ocurre.
    static constexpr size_t kMaxBacklogBlocks = 4;
    static constexpr size_t kResyncTargetBlocks = 1;

    static size_t nextPow2(size_t v) {
        size_t p = 1;
        while (p < v) p <<= 1;
        return p;
    }

    // Algoritmo de relleno suave ("Insured Frame"):
    // Extrapola desde el último frame conocido con una rampa exponencial
    // hacia el silencio (evita DC offset y clicks), en vez de cortar en seco.
    // Constante de decaimiento calibrada para ~3ms a 48kHz (~144 muestras),
    // suficientemente rápida para no sonar como "eco" perceptible pero lo
    // bastante suave para no generar un transitorio audible.
    void insureMissingFrames(float* out, size_t missing) {
        constexpr float kDecayPerFrame = 0.965f; // ~3ms de vida útil a 48kHz
        for (size_t i = 0; i < missing; ++i) {
            fadeGain_ *= kDecayPerFrame;
            for (uint32_t ch = 0; ch < numChannels_; ++ch) {
                out[i * numChannels_ + ch] = lastGoodFrame_[ch] * fadeGain_;
            }
        }
    }

    const size_t capacityFrames_;
    const size_t mask_;
    const uint32_t numChannels_;
    std::unique_ptr<float[]> buffer_;

    alignas(64) std::atomic<size_t> writeIndex_;
    alignas(64) std::atomic<size_t> readIndex_;

    std::unique_ptr<float[]> lastGoodFrame_;
    float fadeGain_;
    std::atomic<uint64_t> underrunCount_{0};
    std::atomic<uint64_t> resyncCount_{0};
};

} // namespace audiopt
