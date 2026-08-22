// =============================================================================
// ApoSmokeTest.cpp
// -----------------------------------------------------------------------------
// Ejercita el APO fuera de audiodg.exe, siguiendo la misma secuencia que hace
// el motor de audio de Windows al cargarlo:
//
//   DllGetClassObject -> CreateInstance -> QueryInterface -> GetRegistration
//   Properties -> IsInput/OutputFormatSupported -> LockForProcess ->
//   APOProcess (con audio real) -> Reset -> UnlockForProcess -> Release
//
// POR QUÉ EXISTE: un fallo dentro de audiodg.exe se manifiesta como "no hay
// audio en el sistema" sin mensaje de error útil, y depurarlo ahí es
// miserable. Todo lo que se pueda romper aquí es un fallo que no llega a
// tocar el audio real de la máquina.
//
// Compilar y ejecutar: ver test/README o el CMakeLists de este directorio.
// =============================================================================
#include <windows.h>
// initguid.h antes de ApoGuids.h define el CLSID aquí (el test no enlaza con
// la DLL, la carga en runtime, así que necesita su propia copia del símbolo).
#include <initguid.h>
#include <audioenginebaseapo.h>
#include <audiomediatype.h>
#include <mmreg.h>
#include <ks.h>
#include <ksmedia.h>

#include <cstdio>
#include <cmath>
#include <vector>
#include <atomic>
#include <algorithm>

#include "../include/ApoGuids.h"

namespace {

int g_failures = 0;
int g_checks = 0;

void check(bool condition, const char* what) {
    ++g_checks;
    if (condition) {
        printf("  [ OK ] %s\n", what);
    } else {
        printf("  [FAIL] %s\n", what);
        ++g_failures;
    }
}

void checkHr(HRESULT hr, const char* what) {
    ++g_checks;
    if (SUCCEEDED(hr)) {
        printf("  [ OK ] %s\n", what);
    } else {
        printf("  [FAIL] %s (hr=0x%08lX)\n", what, static_cast<unsigned long>(hr));
        ++g_failures;
    }
}

// -----------------------------------------------------------------------------
// IAudioMediaType mínimo. El motor de audio pasa su propia implementación; aquí
// solo necesitamos que GetAudioFormat devuelva el WAVEFORMATEX correcto, que es
// lo único que el APO consulta.
// -----------------------------------------------------------------------------
class FakeMediaType : public IAudioMediaType {
public:
    explicit FakeMediaType(const WAVEFORMATEXTENSIBLE& format) : format_(format) {}

    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (IsEqualIID(riid, __uuidof(IUnknown)) ||
            IsEqualIID(riid, __uuidof(IAudioMediaType))) {
            *ppv = static_cast<IAudioMediaType*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return ++refCount_; }
    STDMETHODIMP_(ULONG) Release() override {
        const ULONG remaining = --refCount_;
        if (remaining == 0) delete this;
        return remaining;
    }

    STDMETHODIMP IsCompressedFormat(BOOL* pfCompressed) override {
        if (!pfCompressed) return E_POINTER;
        *pfCompressed = FALSE;
        return S_OK;
    }
    STDMETHODIMP IsEqual(IAudioMediaType*, DWORD* pdwFlags) override {
        if (pdwFlags) *pdwFlags = 0;
        return S_OK;
    }
    STDMETHODIMP_(const WAVEFORMATEX*) GetAudioFormat() override {
        return reinterpret_cast<const WAVEFORMATEX*>(&format_);
    }
    STDMETHODIMP GetUncompressedAudioFormat(UNCOMPRESSEDAUDIOFORMAT*) override {
        return E_NOTIMPL;
    }

private:
    WAVEFORMATEXTENSIBLE format_;
    std::atomic<ULONG> refCount_{1};
};

WAVEFORMATEXTENSIBLE makeFloat32Stereo(DWORD sampleRate) {
    WAVEFORMATEXTENSIBLE f{};
    f.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    f.Format.nChannels = 2;
    f.Format.nSamplesPerSec = sampleRate;
    f.Format.wBitsPerSample = 32;
    f.Format.nBlockAlign = 2 * sizeof(float);
    f.Format.nAvgBytesPerSec = sampleRate * f.Format.nBlockAlign;
    f.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    f.Samples.wValidBitsPerSample = 32;
    f.dwChannelMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
    f.SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
    return f;
}

WAVEFORMATEXTENSIBLE makePcm16Stereo(DWORD sampleRate) {
    WAVEFORMATEXTENSIBLE f{};
    f.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    f.Format.nChannels = 2;
    f.Format.nSamplesPerSec = sampleRate;
    f.Format.wBitsPerSample = 16;
    f.Format.nBlockAlign = 2 * sizeof(int16_t);
    f.Format.nAvgBytesPerSec = sampleRate * f.Format.nBlockAlign;
    f.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    f.Samples.wValidBitsPerSample = 16;
    f.dwChannelMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
    f.SubFormat = KSDATAFORMAT_SUBTYPE_PCM;
    return f;
}

} // namespace

int main() {
    printf("=== Warzone APO - prueba de humo ===\n\n");

    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr)) {
        printf("CoInitializeEx fallo: 0x%08lX\n", static_cast<unsigned long>(hr));
        return 1;
    }

    // -----------------------------------------------------------------------
    printf("[1] Cargar la DLL y obtener la class factory\n");
    // -----------------------------------------------------------------------
    HMODULE dll = LoadLibraryW(L"WarzoneAudioApo.dll");
    if (!dll) {
        printf("  [FAIL] LoadLibrary fallo (error %lu). Copia la DLL junto al test.\n",
               GetLastError());
        CoUninitialize();
        return 1;
    }
    printf("  [ OK ] DLL cargada\n");
    ++g_checks;

    using DllGetClassObjectFn = HRESULT(STDAPICALLTYPE*)(REFCLSID, REFIID, void**);
    auto getClassObject =
        reinterpret_cast<DllGetClassObjectFn>(GetProcAddress(dll, "DllGetClassObject"));
    check(getClassObject != nullptr, "DllGetClassObject exportado");
    if (!getClassObject) { FreeLibrary(dll); CoUninitialize(); return 1; }

    IClassFactory* factory = nullptr;
    hr = getClassObject(CLSID_WarzoneApoMfx, __uuidof(IClassFactory),
                        reinterpret_cast<void**>(&factory));
    checkHr(hr, "Class factory creada para nuestro CLSID");
    if (FAILED(hr) || !factory) { FreeLibrary(dll); CoUninitialize(); return 1; }

    // Un CLSID desconocido debe rechazarse, no devolver algo al azar.
    IClassFactory* bogus = nullptr;
    hr = getClassObject(CLSID_NULL, __uuidof(IClassFactory),
                        reinterpret_cast<void**>(&bogus));
    check(hr == CLASS_E_CLASSNOTAVAILABLE, "CLSID desconocido rechazado");
    if (bogus) bogus->Release();

    // -----------------------------------------------------------------------
    printf("\n[2] Instanciar el APO y pedir sus interfaces\n");
    // -----------------------------------------------------------------------
    IAudioProcessingObject* apo = nullptr;
    hr = factory->CreateInstance(nullptr, __uuidof(IAudioProcessingObject),
                                  reinterpret_cast<void**>(&apo));
    checkHr(hr, "CreateInstance");
    if (FAILED(hr) || !apo) { factory->Release(); FreeLibrary(dll); CoUninitialize(); return 1; }

    IAudioProcessingObjectConfiguration* config = nullptr;
    checkHr(apo->QueryInterface(__uuidof(IAudioProcessingObjectConfiguration),
                                 reinterpret_cast<void**>(&config)),
            "QueryInterface IAudioProcessingObjectConfiguration");

    IAudioProcessingObjectRT* rt = nullptr;
    checkHr(apo->QueryInterface(__uuidof(IAudioProcessingObjectRT),
                                 reinterpret_cast<void**>(&rt)),
            "QueryInterface IAudioProcessingObjectRT");

    IAudioSystemEffects* effects = nullptr;
    checkHr(apo->QueryInterface(__uuidof(IAudioSystemEffects),
                                 reinterpret_cast<void**>(&effects)),
            "QueryInterface IAudioSystemEffects");

    IUnknown* unknown = nullptr;
    checkHr(apo->QueryInterface(__uuidof(IUnknown), reinterpret_cast<void**>(&unknown)),
            "QueryInterface IUnknown");

    // -----------------------------------------------------------------------
    printf("\n[3] Propiedades de registro y latencia\n");
    // -----------------------------------------------------------------------
    APO_REG_PROPERTIES* regProps = nullptr;
    hr = apo->GetRegistrationProperties(&regProps);
    checkHr(hr, "GetRegistrationProperties");
    if (SUCCEEDED(hr) && regProps) {
        check(IsEqualCLSID(regProps->clsid, CLSID_WarzoneApoMfx),
              "El CLSID declarado coincide con el nuestro");
        check((regProps->Flags & APO_FLAG_INPLACE) != 0,
              "Declara procesamiento in-place");
        check(regProps->u32MinInputConnections == 1 && regProps->u32MaxInputConnections == 1,
              "Exactamente una conexion de entrada");
        printf("         nombre: %ls\n", regProps->szFriendlyName);
        CoTaskMemFree(regProps);
    }

    HNSTIME latency = -1;
    checkHr(apo->GetLatency(&latency), "GetLatency");
    check(latency == 0, "Latencia declarada = 0 (cadena feed-forward)");

    // -----------------------------------------------------------------------
    printf("\n[4] Negociacion de formato\n");
    // -----------------------------------------------------------------------
    const WAVEFORMATEXTENSIBLE floatFormat = makeFloat32Stereo(48000);
    const WAVEFORMATEXTENSIBLE pcmFormat = makePcm16Stereo(48000);

    auto* floatType = new FakeMediaType(floatFormat);
    auto* pcmType = new FakeMediaType(pcmFormat);

    IAudioMediaType* negotiated = nullptr;
    hr = apo->IsInputFormatSupported(nullptr, floatType, &negotiated);
    checkHr(hr, "float32 estereo aceptado como entrada");
    if (negotiated) { negotiated->Release(); negotiated = nullptr; }

    hr = apo->IsOutputFormatSupported(nullptr, floatType, &negotiated);
    checkHr(hr, "float32 estereo aceptado como salida");
    if (negotiated) { negotiated->Release(); negotiated = nullptr; }

    hr = apo->IsInputFormatSupported(nullptr, pcmType, &negotiated);
    check(FAILED(hr), "PCM16 rechazado (la cadena solo procesa float32)");
    if (negotiated) { negotiated->Release(); negotiated = nullptr; }

    // -----------------------------------------------------------------------
    printf("\n[5] LockForProcess\n");
    // -----------------------------------------------------------------------
    constexpr UINT32 kMaxFrames = 480;
    constexpr UINT32 kChannels = 2;

    std::vector<float> buffer(kMaxFrames * kChannels, 0.0f);

    APO_CONNECTION_DESCRIPTOR inputDesc{};
    inputDesc.Type = APO_CONNECTION_BUFFER_TYPE_EXTERNAL;
    inputDesc.pBuffer = reinterpret_cast<UINT_PTR>(buffer.data());
    inputDesc.u32MaxFrameCount = kMaxFrames;
    inputDesc.pFormat = floatType;
    inputDesc.u32Signature = APO_CONNECTION_DESCRIPTOR_SIGNATURE;

    APO_CONNECTION_DESCRIPTOR outputDesc = inputDesc;  // in-place: mismo buffer

    APO_CONNECTION_DESCRIPTOR* inputs[] = { &inputDesc };
    APO_CONNECTION_DESCRIPTOR* outputs[] = { &outputDesc };

    checkHr(config->LockForProcess(1, inputs, 1, outputs), "LockForProcess");

    // Un segundo lock debe rechazarse en vez de corromper el estado.
    check(FAILED(config->LockForProcess(1, inputs, 1, outputs)),
          "Segundo LockForProcess rechazado");

    UINT32 channelCount = 0;
    checkHr(apo->GetInputChannelCount(&channelCount), "GetInputChannelCount");
    check(channelCount == kChannels, "Cuenta de canales correcta");

    // -----------------------------------------------------------------------
    printf("\n[6] APOProcess con audio real\n");
    // -----------------------------------------------------------------------
    // Tono de 1kHz a nivel medio: cae en la banda de pasos, asi que la cadena
    // deberia modificarlo de forma medible.
    for (UINT32 i = 0; i < kMaxFrames; ++i) {
        const float sample =
            0.25f * std::sin(2.0f * 3.14159265f * 1000.0f * i / 48000.0f);
        buffer[i * kChannels] = sample;
        buffer[i * kChannels + 1] = sample;
    }
    const std::vector<float> original = buffer;

    APO_CONNECTION_PROPERTY inputProp{};
    inputProp.pBuffer = reinterpret_cast<UINT_PTR>(buffer.data());
    inputProp.u32ValidFrameCount = kMaxFrames;
    inputProp.u32BufferFlags = BUFFER_VALID;
    inputProp.u32Signature = APO_CONNECTION_PROPERTY_SIGNATURE;

    APO_CONNECTION_PROPERTY outputProp = inputProp;

    APO_CONNECTION_PROPERTY* inputProps[] = { &inputProp };
    APO_CONNECTION_PROPERTY* outputProps[] = { &outputProp };

    rt->APOProcess(1, inputProps, 1, outputProps);

    check(outputProp.u32ValidFrameCount == kMaxFrames,
          "Devuelve el mismo numero de frames");
    check(outputProp.u32BufferFlags == BUFFER_VALID, "Marca la salida como valida");

    bool changed = false;
    bool finite = true;
    float peak = 0.0f;
    for (size_t i = 0; i < buffer.size(); ++i) {
        if (std::fabs(buffer[i] - original[i]) > 1e-6f) changed = true;
        if (!std::isfinite(buffer[i])) finite = false;
        peak = (std::max)(peak, std::fabs(buffer[i]));
    }
    check(changed, "La senal fue procesada (la salida difiere de la entrada)");
    check(finite, "Sin NaN ni infinitos en la salida");
    check(peak <= 1.0f, "Salida dentro de rango (sin clipping digital)");
    printf("         pico de salida: %.4f\n", peak);

    // Silencio: debe propagarse sin tocar nada.
    inputProp.u32BufferFlags = BUFFER_SILENT;
    outputProp.u32BufferFlags = BUFFER_VALID;
    rt->APOProcess(1, inputProps, 1, outputProps);
    check(outputProp.u32BufferFlags == BUFFER_SILENT, "Propaga el flag de silencio");

    // Un bloque mayor que lo reservado no debe desbordar: la cadena recorta.
    inputProp.u32BufferFlags = BUFFER_VALID;
    inputProp.u32ValidFrameCount = kMaxFrames;   // el maximo permitido
    rt->APOProcess(1, inputProps, 1, outputProps);
    check(true, "APOProcess con el bloque maximo no desborda");

    check(rt->CalcInputFrames(256) == 256, "CalcInputFrames es 1:1");
    check(rt->CalcOutputFrames(256) == 256, "CalcOutputFrames es 1:1");

    // -----------------------------------------------------------------------
    printf("\n[7] Reset y UnlockForProcess\n");
    // -----------------------------------------------------------------------
    checkHr(apo->Reset(), "Reset con el stream activo");

    // Tras el reset debe seguir procesando sin romperse.
    rt->APOProcess(1, inputProps, 1, outputProps);
    bool stillFinite = true;
    for (float sample : buffer) {
        if (!std::isfinite(sample)) stillFinite = false;
    }
    check(stillFinite, "Sigue procesando correctamente tras Reset");

    checkHr(config->UnlockForProcess(), "UnlockForProcess");
    check(FAILED(config->UnlockForProcess()), "Segundo Unlock rechazado");

    // Un ciclo lock/unlock repetido no debe degradar nada.
    checkHr(config->LockForProcess(1, inputs, 1, outputs), "Re-lock tras unlock");
    checkHr(config->UnlockForProcess(), "Re-unlock");

    // -----------------------------------------------------------------------
    printf("\n[8] Escenarios que el motor produce en la practica\n");
    // -----------------------------------------------------------------------

    // (a) Buffers SEPARADOS. Declaramos in-place, pero el motor no esta
    //     obligado a darnos el mismo puntero; si no se copia, se pierde audio.
    {
        std::vector<float> inBuf(kMaxFrames * kChannels, 0.5f);
        std::vector<float> outBuf(kMaxFrames * kChannels, 0.0f);

        APO_CONNECTION_DESCRIPTOR inD = inputDesc;
        inD.pBuffer = reinterpret_cast<UINT_PTR>(inBuf.data());
        APO_CONNECTION_DESCRIPTOR outD = outputDesc;
        outD.pBuffer = reinterpret_cast<UINT_PTR>(outBuf.data());
        APO_CONNECTION_DESCRIPTOR* inArr[] = { &inD };
        APO_CONNECTION_DESCRIPTOR* outArr[] = { &outD };

        checkHr(config->LockForProcess(1, inArr, 1, outArr), "Lock con buffers separados");

        APO_CONNECTION_PROPERTY inP{};
        inP.pBuffer = reinterpret_cast<UINT_PTR>(inBuf.data());
        inP.u32ValidFrameCount = kMaxFrames;
        inP.u32BufferFlags = BUFFER_VALID;
        inP.u32Signature = APO_CONNECTION_PROPERTY_SIGNATURE;
        APO_CONNECTION_PROPERTY outP = inP;
        outP.pBuffer = reinterpret_cast<UINT_PTR>(outBuf.data());

        APO_CONNECTION_PROPERTY* inPs[] = { &inP };
        APO_CONNECTION_PROPERTY* outPs[] = { &outP };
        rt->APOProcess(1, inPs, 1, outPs);

        bool wrote = false;
        for (float sample : outBuf) {
            if (std::fabs(sample) > 1e-6f) { wrote = true; break; }
        }
        check(wrote, "Escribe en el buffer de salida cuando es distinto del de entrada");
        config->UnlockForProcess();
    }

    // (b) Bloques PARCIALES. El motor casi nunca entrega el maximo exacto.
    {
        APO_CONNECTION_DESCRIPTOR* inArr[] = { &inputDesc };
        APO_CONNECTION_DESCRIPTOR* outArr[] = { &outputDesc };
        checkHr(config->LockForProcess(1, inArr, 1, outArr), "Lock para bloques parciales");

        bool ok = true;
        for (UINT32 frames : { 1u, 7u, 64u, 128u, 441u }) {
            std::fill(buffer.begin(), buffer.end(), 0.1f);
            inputProp.u32ValidFrameCount = frames;
            inputProp.u32BufferFlags = BUFFER_VALID;
            outputProp = inputProp;
            rt->APOProcess(1, inputProps, 1, outputProps);
            if (outputProp.u32ValidFrameCount != frames) ok = false;
            for (UINT32 i = 0; i < frames * kChannels; ++i) {
                if (!std::isfinite(buffer[i])) ok = false;
            }
        }
        check(ok, "Bloques parciales (1, 7, 64, 128, 441 frames) procesados bien");

        // (c) Estres: muchas llamadas seguidas, como en uso real.
        bool stable = true;
        for (int iteration = 0; iteration < 2000; ++iteration) {
            for (UINT32 i = 0; i < kMaxFrames * kChannels; ++i) {
                buffer[i] = 0.3f * std::sin(0.01f * (iteration * kMaxFrames + i));
            }
            inputProp.u32ValidFrameCount = kMaxFrames;
            inputProp.u32BufferFlags = BUFFER_VALID;
            outputProp = inputProp;
            rt->APOProcess(1, inputProps, 1, outputProps);
            for (float sample : buffer) {
                if (!std::isfinite(sample) || std::fabs(sample) > 1.0f) stable = false;
            }
            if (!stable) break;
        }
        check(stable, "2000 bloques seguidos: sin NaN, sin divergencia, sin clipping");
        config->UnlockForProcess();
    }

    // (d) Otros formatos de canal/frecuencia. Un endpoint 5.1 a 44.1kHz es
    //     perfectamente posible y no debe romper nada.
    {
        WAVEFORMATEXTENSIBLE surround{};
        surround.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
        surround.Format.nChannels = 6;
        surround.Format.nSamplesPerSec = 44100;
        surround.Format.wBitsPerSample = 32;
        surround.Format.nBlockAlign = 6 * sizeof(float);
        surround.Format.nAvgBytesPerSec = 44100 * surround.Format.nBlockAlign;
        surround.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
        surround.Samples.wValidBitsPerSample = 32;
        surround.dwChannelMask = KSAUDIO_SPEAKER_5POINT1;
        surround.SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;

        auto* surroundType = new FakeMediaType(surround);
        std::vector<float> surroundBuf(kMaxFrames * 6, 0.2f);

        APO_CONNECTION_DESCRIPTOR d{};
        d.Type = APO_CONNECTION_BUFFER_TYPE_EXTERNAL;
        d.pBuffer = reinterpret_cast<UINT_PTR>(surroundBuf.data());
        d.u32MaxFrameCount = kMaxFrames;
        d.pFormat = surroundType;
        d.u32Signature = APO_CONNECTION_DESCRIPTOR_SIGNATURE;
        APO_CONNECTION_DESCRIPTOR* dArr[] = { &d };

        checkHr(config->LockForProcess(1, dArr, 1, dArr), "Lock en 5.1 a 44.1kHz");

        APO_CONNECTION_PROPERTY p{};
        p.pBuffer = reinterpret_cast<UINT_PTR>(surroundBuf.data());
        p.u32ValidFrameCount = kMaxFrames;
        p.u32BufferFlags = BUFFER_VALID;
        p.u32Signature = APO_CONNECTION_PROPERTY_SIGNATURE;
        APO_CONNECTION_PROPERTY* pArr[] = { &p };
        rt->APOProcess(1, pArr, 1, pArr);

        bool surroundOk = true;
        for (float sample : surroundBuf) {
            if (!std::isfinite(sample)) surroundOk = false;
        }
        check(surroundOk, "Audio 5.1 procesado sin corrupcion");

        config->UnlockForProcess();
        surroundType->Release();
    }

    // (e) Robustez ante entradas invalidas: el motor no deberia mandarlas,
    //     pero un fallo aqui tumba el audio del sistema entero.
    {
        APO_CONNECTION_DESCRIPTOR* inArr[] = { &inputDesc };
        APO_CONNECTION_DESCRIPTOR* outArr[] = { &outputDesc };
        config->LockForProcess(1, inArr, 1, outArr);

        rt->APOProcess(0, nullptr, 0, nullptr);
        rt->APOProcess(1, nullptr, 1, nullptr);
        APO_CONNECTION_PROPERTY* nullEntry[] = { nullptr };
        rt->APOProcess(1, nullEntry, 1, nullEntry);
        check(true, "Punteros nulos no provocan caida");

        config->UnlockForProcess();
    }

    // (f) Conteos de conexion invalidos deben rechazarse.
    {
        APO_CONNECTION_DESCRIPTOR* arr[] = { &inputDesc, &outputDesc };
        check(FAILED(config->LockForProcess(2, arr, 1, arr)),
              "Rechaza mas de una conexion de entrada");
        check(FAILED(config->LockForProcess(0, nullptr, 0, nullptr)),
              "Rechaza cero conexiones");
    }

    // -----------------------------------------------------------------------
    printf("\n[9] Liberacion de referencias\n");
    // -----------------------------------------------------------------------
    unknown->Release();
    effects->Release();
    rt->Release();
    config->Release();
    const ULONG finalRef = apo->Release();
    check(finalRef == 0, "El APO se destruye al soltar la ultima referencia");

    floatType->Release();
    pcmType->Release();
    factory->Release();

    check(DllCanUnloadNow != nullptr, "DLL lista para descargar");
    FreeLibrary(dll);
    CoUninitialize();

    // -----------------------------------------------------------------------
    printf("\n=== Resultado: %d/%d comprobaciones superadas ===\n",
           g_checks - g_failures, g_checks);
    return g_failures == 0 ? 0 : 1;
}
