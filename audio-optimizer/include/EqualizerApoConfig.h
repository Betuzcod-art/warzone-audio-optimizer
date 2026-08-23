// =============================================================================
// EqualizerApoConfig.h
// -----------------------------------------------------------------------------
// Traduce los sliders de la app a un archivo de configuracion de Equalizer APO.
//
// POR QUE:
//   Equalizer APO ya resuelve la parte dificil -- meterse en el motor de audio
//   de Windows sin delay y sin romperlo -- y ahi es donde nuestro propio APO
//   fracaso. Pero su interfaz no es la nuestra. Escribiendo su archivo de
//   configuracion, la app sigue siendo el mando y Equalizer APO pone el motor.
//
// DONDE ESCRIBE:
//   C:\ProgramData\WarzoneAudioOptimizer\warzone-eq.txt
//   El config.txt de Equalizer APO (que vive en Program Files y necesitaria
//   permisos de administrador para cada cambio) solo lleva una linea:
//       Include: C:\ProgramData\WarzoneAudioOptimizer\warzone-eq.txt
//   Asi la app puede reescribir los ajustes sin elevarse, y Equalizer APO los
//   aplica al instante al detectar el cambio.
//
// LIMITE HONESTO:
//   Equalizer APO aplica filtros FIJOS. El realce dinamico de pasos (que sube
//   solo cuando la banda esta floja y se apaga ante un disparo) y la reduccion
//   dinamica de motores no tienen equivalente aqui: eso es compresion, no
//   ecualizacion. Lo que se traduce es la forma de la curva, no su reaccion.
// =============================================================================
#pragma once

#include <windows.h>
#include <string>
#include <cstdio>

namespace audiopt {

// La app escribe DIRECTAMENTE el config.txt de Equalizer APO.
//
// El primer intento fue dejar el config.txt con un "Include:" apuntando a un
// archivo nuestro en ProgramData, para no necesitar permisos. No funciono:
// Equalizer APO no aplicaba el archivo incluido. Escribir su config.txt
// directamente si funciona, y el instalador da permiso de escritura a esa
// carpeta una sola vez para que la app no tenga que elevarse en cada cambio.
inline std::wstring equalizerApoIncludePath() {
    return L"C:\\Program Files\\EqualizerAPO\\config\\config.txt";
}

// Ruta del config.txt de Equalizer APO, si esta instalado.
inline std::wstring equalizerApoConfigPath() {
    return L"C:\\Program Files\\EqualizerAPO\\config\\config.txt";
}

inline bool isEqualizerApoInstalled() {
    return GetFileAttributesW(equalizerApoConfigPath().c_str()) != INVALID_FILE_ATTRIBUTES;
}

// Genera el contenido del archivo a partir de los valores actuales.
//
// footstepBoostDb   -> realce de la banda de presencia
// vehicleThresholdDb-> cuanto se agacha la banda grave (mapeado a atenuacion)
// airGainDb         -> brillo/cajas
// lowCrossoverHz    -> centro de la atenuacion grave
// airCrossoverHz    -> donde empieza el recorte de agudos
// stereoWidth       -> ancho estereo via Mid/Side
// outputTrimDb      -> preamplificacion
inline std::string buildEqualizerApoConfig(
    float footstepBoostDb, float gunshotCutDb, float vehicleThresholdDb,
    float airGainDb, float lowCrossoverHz, float airCrossoverHz,
    float stereoWidth, float outputTrimDb) {

    // stereoWidth se recibe por compatibilidad con la interfaz, pero no se
    // usa: ver la nota sobre Copy mas abajo.
    (void)stereoWidth;

    // El umbral del compresor (-15..-45 dB) se traduce a cuanta atenuacion
    // fija aplicar: cuanto mas agresivo era el ajuste dinamico, mas se recorta
    // aqui. No es lo mismo, pero conserva la intencion del slider.
    //
    // El resultado tiene que ser NEGATIVO: se trata de agachar los motores.
    // Con el signo invertido, el slider de "reducir motores" los subia.
    const float vehicleCutDb = ((vehicleThresholdDb + 15.0f) / 30.0f) * 8.0f;

    // El realce dinamico llegaba a +14 dB porque solo actuaba sobre sonidos
    // flojos. Fijo, ese valor seria insoportable: se aplica algo mas de la
    // mitad, que es donde deja de sonar natural.
    const float presenceGain = footstepBoostDb * 0.55f;

    char buffer[4096];
    const int written = snprintf(buffer, sizeof(buffer),
        "# Generado por Warzone Audio Optimizer -- no editar a mano:\r\n"
        "# la app reescribe este archivo cada vez que mueves un slider.\r\n"
        "\r\n"
        "Preamp: %.1f dB\r\n"
        "\r\n"
        "# Retumbo fuera de la banda util\r\n"
        "Filter: ON HP Fc 80 Hz\r\n"
        "\r\n"
        "# Motores, helicopteros, cuerpo de explosiones\r\n"
        "Filter: ON PK Fc %.0f Hz Gain %.1f dB Q 0.8\r\n"
        "\r\n"
        "# Disparos: el 'punch' del arma vive sobre todo entre 700 y 1500 Hz,\r\n"
        "# una banda donde los pasos apenas tienen energia. Es lo unico que\r\n"
        "# permite bajarlos sin llevarse los pasos por delante, ya que en la\r\n"
        "# zona de 2-5 kHz ambos se solapan y un filtro fijo no los distingue.\r\n"
        "Filter: ON PK Fc 1100 Hz Gain %.1f dB Q 1.0\r\n"
        "\r\n"
        "# Pasos: cuerpo, presencia y detalle\r\n"
        "Filter: ON PK Fc 2200 Hz Gain %.1f dB Q 0.9\r\n"
        "Filter: ON PK Fc 3600 Hz Gain %.1f dB Q 1.1\r\n"
        "Filter: ON PK Fc 5000 Hz Gain %.1f dB Q 1.0\r\n"
        "\r\n"
        "# Brillo: cajas de botin, cristales, metal\r\n"
        "Filter: ON HS Fc %.0f Hz Gain %.1f dB Q 0.7\r\n"
        "\r\n"
        "# NOTA: el slider de amplitud estereo no se aplica aqui.\r\n"
        "#\r\n"
        "# El comando Copy de Equalizer APO evalua las asignaciones EN\r\n"
        "# SECUENCIA, no a la vez: al calcular R ya usa el L modificado en la\r\n"
        "# misma linea, lo que rompe el balance y deja el sonido cargado a un\r\n"
        "# canal. Se probo con canales virtuales M/S y con mezcla directa L/R,\r\n"
        "# y ambas formas desequilibraban el estereo.\r\n"
        "#\r\n"
        "# El ensanchado es un extra; los filtros son lo que aporta. Antes que\r\n"
        "# entregar un estereo roto, se deja fuera.\r\n",
        // Margen anti-saturacion proporcional al realce, no fijo: si se
        // restaran siempre 6 dB, un usuario que ya hubiera bajado el volumen
        // acabaria con el audio inaudible.
        outputTrimDb - (presenceGain * 0.5f),
        lowCrossoverHz * 0.31f,              // ~220 Hz cuando el corte esta en 700
        vehicleCutDb,
        gunshotCutDb,
        presenceGain * 0.6f,
        presenceGain,
        presenceGain * 0.4f,
        airCrossoverHz * 1.6f,               // el shelf empieza sobre el corte
        airGainDb);

    return written > 0 ? std::string(buffer) : std::string();
}

// Escribe el archivo. Devuelve false si no se pudo.
inline bool writeEqualizerApoConfig(const std::string& content) {
    HANDLE file = CreateFileW(equalizerApoIncludePath().c_str(), GENERIC_WRITE,
                              FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;

    DWORD written = 0;
    const BOOL ok = WriteFile(file, content.c_str(),
                              static_cast<DWORD>(content.size()), &written, nullptr);
    CloseHandle(file);
    return ok && written == content.size();
}

} // namespace audiopt
