// =============================================================================
// ApoStatus.h
// -----------------------------------------------------------------------------
// Detecta si el APO (la version que procesa dentro del motor de audio de
// Windows) esta instalado, y en que dispositivo.
//
// POR QUE IMPORTA EN LA APP:
//   Las dos versiones hacen el MISMO procesamiento. Si ambas estuvieran
//   activas a la vez, el audio pasaria dos veces por la cadena -- sonaria
//   sobreprocesado y volveria el delay que el APO existe para eliminar.
//
//   Por eso, cuando el APO esta instalado, la app deja de ser un procesador y
//   pasa a ser su panel de control: los sliders siguen mandando (el APO relee
//   el archivo de ajustes en caliente), pero el motor WASAPI local no arranca.
// =============================================================================
#pragma once

#include <windows.h>
#include <string>
#include <vector>

namespace audiopt {

// CLSID del APO. Debe coincidir con apo/include/ApoGuids.h -- si cambia alli,
// cambia aqui.
constexpr wchar_t kApoClsidString[] = L"{E76EE61C-E30D-4C52-994D-F54422E9A2C9}";

struct ApoStatus {
    bool dllRegistered = false;   // La DLL existe y esta registrada como COM
    bool attached = false;        // Ademas, esta asociada a algun dispositivo
    std::wstring deviceName;      // Dispositivo donde esta asociada
    std::wstring slotName;        // SFX / MFX / EFX
};

namespace apodetail {

inline std::wstring readRegString(HKEY root, const std::wstring& subKey,
                                  const wchar_t* valueName) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(root, subKey.c_str(), 0, KEY_READ, &key) != ERROR_SUCCESS) {
        return L"";
    }
    wchar_t buffer[512]{};
    DWORD size = sizeof(buffer);
    DWORD type = 0;
    const LONG result = RegQueryValueExW(key, valueName, nullptr, &type,
                                          reinterpret_cast<BYTE*>(buffer), &size);
    RegCloseKey(key);
    if (result != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ)) {
        return L"";
    }
    return buffer;
}

} // namespace apodetail

// Consulta el estado del APO leyendo el registro. Solo lectura: no modifica
// nada, asi que es seguro llamarla desde la UI cuando se quiera.
inline ApoStatus queryApoStatus() {
    ApoStatus status;

    // 1) Esta registrada la DLL como objeto COM, y sigue existiendo?
    const std::wstring inprocKey =
        std::wstring(L"CLSID\\") + kApoClsidString + L"\\InprocServer32";
    const std::wstring dllPath =
        apodetail::readRegString(HKEY_CLASSES_ROOT, inprocKey, nullptr);
    if (!dllPath.empty() && GetFileAttributesW(dllPath.c_str()) != INVALID_FILE_ATTRIBUTES) {
        status.dllRegistered = true;
    }

    // 2) Esta asociada a algun dispositivo de salida? Se revisan los tres
    //    slots porque el instalador elige el que estuviera libre.
    struct Slot { const wchar_t* key; const wchar_t* name; };
    const Slot slots[] = {
        { L"{D04E05A6-594B-4FB6-A80D-01AF5EED7D1D},5", L"SFX" },
        { L"{D04E05A6-594B-4FB6-A80D-01AF5EED7D1D},6", L"MFX" },
        { L"{D04E05A6-594B-4FB6-A80D-01AF5EED7D1D},7", L"EFX" },
    };

    const std::wstring renderRoot =
        L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\MMDevices\\Audio\\Render";

    HKEY renderKey = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, renderRoot.c_str(), 0, KEY_READ, &renderKey)
            != ERROR_SUCCESS) {
        return status;
    }

    wchar_t endpointName[256]{};
    DWORD index = 0;
    DWORD nameSize = static_cast<DWORD>(std::size(endpointName));
    while (RegEnumKeyExW(renderKey, index, endpointName, &nameSize,
                          nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS) {
        ++index;
        nameSize = static_cast<DWORD>(std::size(endpointName));

        const std::wstring endpointBase = renderRoot + L"\\" + endpointName;
        const std::wstring fxKey = endpointBase + L"\\FxProperties";

        for (const auto& slot : slots) {
            const std::wstring value =
                apodetail::readRegString(HKEY_LOCAL_MACHINE, fxKey, slot.key);
            if (_wcsicmp(value.c_str(), kApoClsidString) != 0) continue;

            status.attached = true;
            status.slotName = slot.name;

            // Nombre legible del dispositivo, para poder mostrarlo.
            const std::wstring propsKey = endpointBase + L"\\Properties";
            std::wstring friendly = apodetail::readRegString(
                HKEY_LOCAL_MACHINE, propsKey,
                L"{a45c254e-df1c-4efd-8020-67d146a850e0},14");
            if (friendly.empty()) {
                friendly = apodetail::readRegString(
                    HKEY_LOCAL_MACHINE, propsKey,
                    L"{a45c254e-df1c-4efd-8020-67d146a850e0},2");
            }
            status.deviceName = friendly.empty() ? L"(dispositivo)" : friendly;
            break;
        }
        if (status.attached) break;
    }

    RegCloseKey(renderKey);
    return status;
}

} // namespace audiopt
