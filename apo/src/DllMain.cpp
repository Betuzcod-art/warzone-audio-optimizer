// =============================================================================
// DllMain.cpp
// -----------------------------------------------------------------------------
// Plomería COM de la DLL: class factory, conteo de referencias del módulo y
// los cuatro exports que Windows necesita para instanciar el APO.
//
// El registro en sí (DllRegisterServer) solo escribe la entrada CLSID que dice
// "esta DLL implementa este objeto". Asociar el APO a un dispositivo de audio
// concreto es un paso aparte que hace el instalador -- ver install/.
// =============================================================================
#include <windows.h>
#include <string>
#include <new>

#include "WarzoneApo.h"
#include "ApoGuids.h"

namespace {

HMODULE g_module = nullptr;
std::atomic<LONG> g_objectCount{0};

void addObject() { g_objectCount.fetch_add(1, std::memory_order_relaxed); }
void releaseObject() { g_objectCount.fetch_sub(1, std::memory_order_relaxed); }

class ApoClassFactory : public IClassFactory {
public:
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (IsEqualIID(riid, __uuidof(IUnknown)) ||
            IsEqualIID(riid, __uuidof(IClassFactory))) {
            *ppv = static_cast<IClassFactory*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }

    STDMETHODIMP_(ULONG) AddRef() override {
        return refCount_.fetch_add(1, std::memory_order_relaxed) + 1;
    }

    STDMETHODIMP_(ULONG) Release() override {
        const ULONG remaining = refCount_.fetch_sub(1, std::memory_order_acq_rel) - 1;
        if (remaining == 0) delete this;
        return remaining;
    }

    STDMETHODIMP CreateInstance(IUnknown* outer, REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        *ppv = nullptr;
        if (outer) return CLASS_E_NOAGGREGATION;

        auto* apo = new (std::nothrow) warzoneapo::WarzoneApoMfx();
        if (!apo) return E_OUTOFMEMORY;

        addObject();
        const HRESULT hr = apo->QueryInterface(riid, ppv);
        // El APO nace con refcount 1; QueryInterface lo subió a 2. Soltamos
        // la nuestra para que el llamante sea el único dueño.
        apo->Release();
        if (FAILED(hr)) releaseObject();
        return hr;
    }

    STDMETHODIMP LockServer(BOOL lock) override {
        if (lock) {
            addObject();
        } else {
            releaseObject();
        }
        return S_OK;
    }

private:
    std::atomic<ULONG> refCount_{1};
};

// Escribe una cadena en el registro; devuelve false al primer fallo para que
// DllRegisterServer pueda reportar el error en vez de dejar registro a medias.
bool writeRegistryString(HKEY root, const std::wstring& subKey,
                          const wchar_t* valueName, const std::wstring& value) {
    HKEY key = nullptr;
    if (RegCreateKeyExW(root, subKey.c_str(), 0, nullptr, REG_OPTION_NON_VOLATILE,
                         KEY_WRITE, nullptr, &key, nullptr) != ERROR_SUCCESS) {
        return false;
    }
    const DWORD bytes = static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t));
    const LONG result = RegSetValueExW(key, valueName, 0, REG_SZ,
                                        reinterpret_cast<const BYTE*>(value.c_str()), bytes);
    RegCloseKey(key);
    return result == ERROR_SUCCESS;
}

} // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID /*reserved*/) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_module = module;
        DisableThreadLibraryCalls(module);
    }
    return TRUE;
}

STDAPI DllGetClassObject(REFCLSID rclsid, REFIID riid, void** ppv) {
    if (!ppv) return E_POINTER;
    *ppv = nullptr;

    if (!IsEqualCLSID(rclsid, CLSID_WarzoneApoMfx)) {
        return CLASS_E_CLASSNOTAVAILABLE;
    }

    auto* factory = new (std::nothrow) ApoClassFactory();
    if (!factory) return E_OUTOFMEMORY;

    const HRESULT hr = factory->QueryInterface(riid, ppv);
    factory->Release();
    return hr;
}

STDAPI DllCanUnloadNow() {
    return g_objectCount.load(std::memory_order_relaxed) == 0 ? S_OK : S_FALSE;
}

STDAPI DllRegisterServer() {
    wchar_t modulePath[MAX_PATH]{};
    if (GetModuleFileNameW(g_module, modulePath,
                            static_cast<DWORD>(std::size(modulePath))) == 0) {
        return HRESULT_FROM_WIN32(GetLastError());
    }

    const std::wstring clsidKey =
        std::wstring(L"CLSID\\") + WARZONE_APO_MFX_CLSID_STRING;

    if (!writeRegistryString(HKEY_CLASSES_ROOT, clsidKey, nullptr,
                              L"Warzone Audio Optimizer APO")) {
        return E_ACCESSDENIED;
    }
    if (!writeRegistryString(HKEY_CLASSES_ROOT, clsidKey + L"\\InprocServer32",
                              nullptr, modulePath)) {
        return E_ACCESSDENIED;
    }
    // "Both" permite que el motor de audio cree el objeto en el apartamento
    // que le convenga, sin marshalling.
    if (!writeRegistryString(HKEY_CLASSES_ROOT, clsidKey + L"\\InprocServer32",
                              L"ThreadingModel", L"Both")) {
        return E_ACCESSDENIED;
    }

    // Registro ante el motor de audio, para que reconozca el CLSID como un APO
    // instalable y lo liste.
    const std::wstring engineKey =
        std::wstring(L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\MMDevices\\"
                     L"Audio\\AudioEngine\\AudioProcessingObjects\\") +
        WARZONE_APO_MFX_CLSID_STRING;
    writeRegistryString(HKEY_LOCAL_MACHINE, engineKey, L"FriendlyName",
                         L"Warzone Audio Optimizer");

    return S_OK;
}

STDAPI DllUnregisterServer() {
    const std::wstring clsidKey =
        std::wstring(L"CLSID\\") + WARZONE_APO_MFX_CLSID_STRING;
    RegDeleteKeyW(HKEY_CLASSES_ROOT, (clsidKey + L"\\InprocServer32").c_str());
    RegDeleteKeyW(HKEY_CLASSES_ROOT, clsidKey.c_str());

    const std::wstring engineKey =
        std::wstring(L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\MMDevices\\"
                     L"Audio\\AudioEngine\\AudioProcessingObjects\\") +
        WARZONE_APO_MFX_CLSID_STRING;
    RegDeleteKeyW(HKEY_LOCAL_MACHINE, engineKey.c_str());

    return S_OK;
}
