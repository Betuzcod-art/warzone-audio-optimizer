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
#include <mmdeviceapi.h>
#include <wrl/client.h>
#include <string>
#include <vector>
#include <algorithm>

namespace audiopt {

// CLSID del APO. Debe coincidir con apo/include/ApoGuids.h -- si cambia alli,
// cambia aqui.
constexpr wchar_t kApoClsidString[] = L"{E76EE61C-E30D-4C52-994D-F54422E9A2C9}";

struct ApoStatus {
    bool dllRegistered = false;   // La DLL existe y esta registrada como COM
    bool attached = false;        // Ademas, esta asociada a algun dispositivo
    std::wstring deviceName;      // Dispositivo donde esta asociada
    std::wstring slotName;        // SFX / MFX / EFX
    std::wstring deviceGuid;      // Endpoint donde esta asociada
};

// Un dispositivo de salida activo, tal como se lo ofrecemos al usuario.
struct RenderDevice {
    std::wstring guid;        // Clave del endpoint, {....}
    std::wstring name;        // Nombre visible
    std::wstring adapter;     // Tarjeta/driver: lo que distingue tres "Altavoces"
    bool hasApo = false;      // El APO esta puesto aqui
    bool isDefault = false;   // Es la salida predeterminada de Windows
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
            status.deviceGuid = endpointName;
            break;
        }
        if (status.attached) break;
    }

    RegCloseKey(renderKey);
    return status;
}

namespace apodetail {

// GUID del endpoint predeterminado de Windows, o "" si no se puede saber.
// El id que devuelve WASAPI es del tipo "{0.0.0.00000000}.{guid}", asi que
// nos quedamos con la ultima llave, que es la clave del registro.
inline std::wstring defaultRenderEndpointGuid() {
    Microsoft::WRL::ComPtr<IMMDeviceEnumerator> enumerator;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                 __uuidof(IMMDeviceEnumerator),
                                 reinterpret_cast<void**>(enumerator.GetAddressOf())))) {
        return L"";
    }
    Microsoft::WRL::ComPtr<IMMDevice> device;
    if (FAILED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, device.GetAddressOf()))) {
        return L"";
    }
    LPWSTR rawId = nullptr;
    if (FAILED(device->GetId(&rawId)) || !rawId) return L"";

    std::wstring id(rawId);
    CoTaskMemFree(rawId);

    const size_t open = id.find_last_of(L'{');
    if (open == std::wstring::npos) return L"";
    return id.substr(open);
}

} // namespace apodetail

// Lista los dispositivos de salida activos, marcando cual tiene el APO y
// cual es el predeterminado de Windows. Solo lectura.
inline std::vector<RenderDevice> enumerateRenderDevices() {
    std::vector<RenderDevice> devices;

    const std::wstring renderRoot =
        L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\MMDevices\\Audio\\Render";
    HKEY renderKey = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, renderRoot.c_str(), 0, KEY_READ, &renderKey)
            != ERROR_SUCCESS) {
        return devices;
    }

    const ApoStatus status = queryApoStatus();
    const std::wstring defaultGuid = apodetail::defaultRenderEndpointGuid();

    wchar_t endpointName[256]{};
    DWORD index = 0;
    DWORD nameSize = static_cast<DWORD>(std::size(endpointName));
    while (RegEnumKeyExW(renderKey, index, endpointName, &nameSize,
                          nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS) {
        ++index;
        nameSize = static_cast<DWORD>(std::size(endpointName));

        const std::wstring endpointBase = renderRoot + L"\\" + endpointName;

        // Solo dispositivos activos: los desconectados o deshabilitados solo
        // servirian para elegir mal.
        DWORD state = 0;
        DWORD stateSize = sizeof(state);
        HKEY endpointKey = nullptr;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, endpointBase.c_str(), 0, KEY_READ, &endpointKey)
                == ERROR_SUCCESS) {
            DWORD type = 0;
            RegQueryValueExW(endpointKey, L"DeviceState", nullptr, &type,
                             reinterpret_cast<BYTE*>(&state), &stateSize);
            RegCloseKey(endpointKey);
        }
        if (state != 1) continue;

        const std::wstring propsKey = endpointBase + L"\\Properties";

        RenderDevice device;
        device.guid = endpointName;
        device.name = apodetail::readRegString(HKEY_LOCAL_MACHINE, propsKey,
            L"{a45c254e-df1c-4efd-8020-67d146a850e0},14");
        if (device.name.empty()) {
            device.name = apodetail::readRegString(HKEY_LOCAL_MACHINE, propsKey,
                L"{a45c254e-df1c-4efd-8020-67d146a850e0},2");
        }
        if (device.name.empty()) device.name = L"(sin nombre)";

        // El adaptador es lo que permite distinguir varios dispositivos que
        // se llaman igual ("Altavoces" de la placa, del 7.1, del mando...).
        device.adapter = apodetail::readRegString(HKEY_LOCAL_MACHINE, propsKey,
            L"{b3f8fa53-0004-438e-9003-51a46e139bfc},6");

        device.hasApo = (_wcsicmp(device.guid.c_str(), status.deviceGuid.c_str()) == 0) &&
                        status.attached;
        device.isDefault = !defaultGuid.empty() &&
                           _wcsicmp(device.guid.c_str(), defaultGuid.c_str()) == 0;

        devices.push_back(std::move(device));
    }

    RegCloseKey(renderKey);

    // Orden util: primero donde esta el APO, luego el predeterminado, y el
    // resto alfabetico -- asi lo relevante queda arriba.
    std::sort(devices.begin(), devices.end(),
              [](const RenderDevice& a, const RenderDevice& b) {
                  if (a.hasApo != b.hasApo) return a.hasApo;
                  if (a.isDefault != b.isDefault) return a.isDefault;
                  return a.name < b.name;
              });
    return devices;
}

} // namespace audiopt
