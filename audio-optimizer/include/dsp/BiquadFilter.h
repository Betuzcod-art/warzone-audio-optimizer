// =============================================================================
// BiquadFilter.h
// -----------------------------------------------------------------------------
// Filtro Biquad de forma directa II transpuesta (RBJ Cookbook), de fase mínima
// y orden bajo (12 dB/oct). Se usa para:
//   - High-Pass: elimina retumbo/DC por debajo de la banda útil de pasos.
//   - Peaking (campana): realce/atenuación de una banda concreta.
//
// Diseño: se aplica SOLO a la señal ya mezclada/renderizada (post motor 3D del
// juego), nunca al buffer de paneo posicional. Un biquad de orden 2 introduce
// desfase suave y continuo (no lineal a saltos), que a las frecuencias e
// intensidades usadas aquí no es perceptible como desplazamiento de imagen
// estéreo/posicional -- a diferencia de un filtro FIR de fase lineal, que
// añadiría latencia de varios ms (inaceptable para el objetivo <5ms).
// =============================================================================
#pragma once

#include <cmath>
#include <numbers>

namespace audiopt {

enum class BiquadType { HighPass, LowPass, Peaking, LowShelf, HighShelf };

class BiquadFilter {
public:
    void setSampleRate(double sr) { sampleRate_ = sr; }

    // frequencyHz: frecuencia de corte/centro
    // q: factor de calidad (ancho de banda). Mayor Q = banda más estrecha.
    // gainDb: solo relevante para Peaking/Shelf.
    void configure(BiquadType type, double frequencyHz, double q, double gainDb = 0.0) {
        const double A = std::pow(10.0, gainDb / 40.0);
        const double w0 = 2.0 * std::numbers::pi * frequencyHz / sampleRate_;
        const double cosW0 = std::cos(w0);
        const double sinW0 = std::sin(w0);
        const double alpha = sinW0 / (2.0 * q);

        double b0 = 1.0;
        double b1 = 0.0;
        double b2 = 0.0;
        double a0 = 1.0;
        double a1 = 0.0;
        double a2 = 0.0;

        switch (type) {
            case BiquadType::HighPass:
                b0 = (1.0 + cosW0) / 2.0;
                b1 = -(1.0 + cosW0);
                b2 = (1.0 + cosW0) / 2.0;
                a0 = 1.0 + alpha;
                a1 = -2.0 * cosW0;
                a2 = 1.0 - alpha;
                break;
            case BiquadType::LowPass:
                b0 = (1.0 - cosW0) / 2.0;
                b1 = 1.0 - cosW0;
                b2 = (1.0 - cosW0) / 2.0;
                a0 = 1.0 + alpha;
                a1 = -2.0 * cosW0;
                a2 = 1.0 - alpha;
                break;
            case BiquadType::Peaking:
                b0 = 1.0 + alpha * A;
                b1 = -2.0 * cosW0;
                b2 = 1.0 - alpha * A;
                a0 = 1.0 + alpha / A;
                a1 = -2.0 * cosW0;
                a2 = 1.0 - alpha / A;
                break;
            case BiquadType::LowShelf: {
                const double sq = std::sqrt(A) * (1.0 / q);
                b0 = A * ((A + 1) - (A - 1) * cosW0 + sq * sinW0 * 2.0 * std::sqrt(A) / 2.0);
                b1 = 2.0 * A * ((A - 1) - (A + 1) * cosW0);
                b2 = A * ((A + 1) - (A - 1) * cosW0 - sq * sinW0 * 2.0 * std::sqrt(A) / 2.0);
                a0 = (A + 1) + (A - 1) * cosW0 + sq * sinW0 * 2.0 * std::sqrt(A) / 2.0;
                a1 = -2.0 * ((A - 1) + (A + 1) * cosW0);
                a2 = (A + 1) + (A - 1) * cosW0 - sq * sinW0 * 2.0 * std::sqrt(A) / 2.0;
                break;
            }
            case BiquadType::HighShelf: {
                const double sq = std::sqrt(A) * (1.0 / q);
                b0 = A * ((A + 1) + (A - 1) * cosW0 + sq * sinW0 * 2.0 * std::sqrt(A) / 2.0);
                b1 = -2.0 * A * ((A - 1) + (A + 1) * cosW0);
                b2 = A * ((A + 1) + (A - 1) * cosW0 - sq * sinW0 * 2.0 * std::sqrt(A) / 2.0);
                a0 = (A + 1) - (A - 1) * cosW0 + sq * sinW0 * 2.0 * std::sqrt(A) / 2.0;
                a1 = 2.0 * ((A - 1) - (A + 1) * cosW0);
                a2 = (A + 1) - (A - 1) * cosW0 - sq * sinW0 * 2.0 * std::sqrt(A) / 2.0;
                break;
            }
        }

        b0_ = static_cast<float>(b0 / a0);
        b1_ = static_cast<float>(b1 / a0);
        b2_ = static_cast<float>(b2 / a0);
        a1_ = static_cast<float>(a1 / a0);
        a2_ = static_cast<float>(a2 / a0);
    }

    // Procesamiento muestra a muestra, Direct Form II Transpuesta.
    // Un único estado (z1_, z2_) por canal -> el llamador debe mantener una
    // instancia de BiquadFilter POR CANAL para audio estéreo/multicanal.
    inline float processSample(float x) {
        const float y = b0_ * x + z1_;
        z1_ = b1_ * x - a1_ * y + z2_;
        z2_ = b2_ * x - a2_ * y;
        return y;
    }

    void reset() { z1_ = z2_ = 0.0f; }

private:
    double sampleRate_ = 48000.0;
    float b0_ = 1.0f, b1_ = 0.0f, b2_ = 0.0f, a1_ = 0.0f, a2_ = 0.0f;
    float z1_ = 0.0f, z2_ = 0.0f;
};

} // namespace audiopt
