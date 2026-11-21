#include "bridge_driver.h"
#include "driver_guid.h"

#include <atomic>
#include <new>
#include <objbase.h>
#include <string>
#include <windows.h>

namespace {

HMODULE g_module = nullptr;
std::atomic<long> g_dllRefCount{0};

class BridgeClassFactory final : public IClassFactory {
public:
    BridgeClassFactory() { g_dllRefCount.fetch_add(1, std::memory_order_relaxed); }
    ~BridgeClassFactory() { g_dllRefCount.fetch_sub(1, std::memory_order_relaxed); }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override {
        if (!ppvObject) return E_POINTER;
        if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_IClassFactory)) {
            *ppvObject = static_cast<IClassFactory*>(this);
            AddRef();
            return S_OK;
        }
        *ppvObject = nullptr;
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override {
        return refCount_.fetch_add(1, std::memory_order_relaxed) + 1;
    }

    ULONG STDMETHODCALLTYPE Release() override {
        ULONG c = refCount_.fetch_sub(1, std::memory_order_acq_rel) - 1;
        if (c == 0) delete this;
        return c;
    }

    HRESULT STDMETHODCALLTYPE CreateInstance(IUnknown* pUnkOuter, REFIID riid, void** ppvObject) override {
        if (!ppvObject) return E_POINTER;
        *ppvObject = nullptr;
        if (pUnkOuter) return CLASS_E_NOAGGREGATION;

        auto* drv = new (std::nothrow) BridgeDriver();
        if (!drv) return E_OUTOFMEMORY;

        HRESULT hr = drv->QueryInterface(riid, ppvObject);
        drv->Release();
        return hr;
    }

    HRESULT STDMETHODCALLTYPE LockServer(BOOL fLock) override {
        if (fLock) {
            g_dllRefCount.fetch_add(1, std::memory_order_relaxed);
        } else {
            g_dllRefCount.fetch_sub(1, std::memory_order_relaxed);
        }
        return S_OK;
    }

private:
    std::atomic<ULONG> refCount_{1};
};

LONG setRegSz(HKEY root, const wchar_t* subkey, const wchar_t* valueName, const wchar_t* data) {
    HKEY key = nullptr;
    LONG st = RegCreateKeyExW(root, subkey, 0, nullptr, REG_OPTION_NON_VOLATILE,
        KEY_WRITE | KEY_WOW64_64KEY, nullptr, &key, nullptr);
    if (st != ERROR_SUCCESS) return st;

    DWORD bytes = static_cast<DWORD>((wcslen(data) + 1) * sizeof(wchar_t));
    st = RegSetValueExW(key, valueName, 0, REG_SZ, reinterpret_cast<const BYTE*>(data), bytes);
    RegCloseKey(key);
    return st;
}

} // namespace

void dllObjectCreated() {
    g_dllRefCount.fetch_add(1, std::memory_order_relaxed);
}

void dllObjectDestroyed() {
    g_dllRefCount.fetch_sub(1, std::memory_order_relaxed);
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ulReason, LPVOID /*lpReserved*/) {
    if (ulReason == DLL_PROCESS_ATTACH) {
        g_module = hModule;
        DisableThreadLibraryCalls(hModule);
    }
    return TRUE;
}

STDAPI DllGetClassObject(REFCLSID rclsid, REFIID riid, LPVOID* ppv) {
    if (!ppv) return E_POINTER;
    *ppv = nullptr;
    if (!IsEqualCLSID(rclsid, CLSID_AsioBridge)) {
        return CLASS_E_CLASSNOTAVAILABLE;
    }
    auto* factory = new (std::nothrow) BridgeClassFactory();
    if (!factory) return E_OUTOFMEMORY;
    HRESULT hr = factory->QueryInterface(riid, ppv);
    factory->Release();
    return hr;
}

STDAPI DllCanUnloadNow() {
    return (g_dllRefCount.load(std::memory_order_acquire) == 0) ? S_OK : S_FALSE;
}

STDAPI DllRegisterServer() {
    wchar_t dllPath[MAX_PATH]{};
    if (GetModuleFileNameW(g_module, dllPath, MAX_PATH) == 0) {
        return HRESULT_FROM_WIN32(GetLastError());
    }

    std::wstring clsidKey = std::wstring(L"CLSID\\") + kAsioBridgeClsidStrW;
    std::wstring inprocKey = clsidKey + L"\\InprocServer32";
    std::wstring asioKey = std::wstring(L"SOFTWARE\\ASIO\\") + kAsioBridgeNameW;

    LONG st = setRegSz(HKEY_CLASSES_ROOT, clsidKey.c_str(), nullptr, kAsioBridgeNameW);
    if (st != ERROR_SUCCESS) return HRESULT_FROM_WIN32(st);

    st = setRegSz(HKEY_CLASSES_ROOT, inprocKey.c_str(), nullptr, dllPath);
    if (st != ERROR_SUCCESS) return HRESULT_FROM_WIN32(st);

    st = setRegSz(HKEY_CLASSES_ROOT, inprocKey.c_str(), L"ThreadingModel", L"Apartment");
    if (st != ERROR_SUCCESS) return HRESULT_FROM_WIN32(st);

    st = setRegSz(HKEY_LOCAL_MACHINE, asioKey.c_str(), L"CLSID", kAsioBridgeClsidStrW);
    if (st != ERROR_SUCCESS) return HRESULT_FROM_WIN32(st);

    st = setRegSz(HKEY_LOCAL_MACHINE, asioKey.c_str(), L"Description", kAsioBridgeDescW);
    if (st != ERROR_SUCCESS) return HRESULT_FROM_WIN32(st);

    return S_OK;
}

STDAPI DllUnregisterServer() {
    std::wstring clsidKey = std::wstring(L"CLSID\\") + kAsioBridgeClsidStrW;
    std::wstring asioKey = std::wstring(L"SOFTWARE\\ASIO\\") + kAsioBridgeNameW;

    RegDeleteTreeW(HKEY_CLASSES_ROOT, clsidKey.c_str());
    RegDeleteTreeW(HKEY_LOCAL_MACHINE, asioKey.c_str());
    return S_OK;
}
