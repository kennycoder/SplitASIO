#include "wasapi_sender.h"

#include <windows.h>
#include <objbase.h>
#include <initguid.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <avrt.h>
#include <propsys.h>
#include <functiondiscoverykeys_devpkey.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {

std::wstring utf8ToWide(const std::string& s) {
    if (s.empty()) return {};
    int len = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (len <= 0) return {};
    std::wstring out(static_cast<size_t>(len), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), len);
    return out;
}

std::string wideToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    if (len <= 0) return {};
    std::string out(static_cast<size_t>(len), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), out.data(), len, nullptr, nullptr);
    return out;
}

template <typename T>
void safeRelease(T*& ptr) {
    if (ptr) {
        ptr->Release();
        ptr = nullptr;
    }
}

} // namespace

std::vector<WasapiEndpointInfo> enumerateWasapiRenderEndpoints() {
    std::vector<WasapiEndpointInfo> result;

    // Ensure COM is initialized on the calling thread (harmless if already initialized in STA or MTA)
    HRESULT initHr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const bool needUninit = SUCCEEDED(initHr);

    IMMDeviceEnumerator* enumerator = nullptr;
    HRESULT hr = CoCreateInstance(
        __uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
        __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(&enumerator));

    if (SUCCEEDED(hr) && enumerator) {
        IMMDeviceCollection* collection = nullptr;
        if (SUCCEEDED(enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &collection)) && collection) {
            UINT count = 0;
            collection->GetCount(&count);
            for (UINT i = 0; i < count; ++i) {
                IMMDevice* device = nullptr;
                if (FAILED(collection->Item(i, &device)) || !device) continue;

                LPWSTR idW = nullptr;
                std::string idStr;
                if (SUCCEEDED(device->GetId(&idW)) && idW) {
                    idStr = wideToUtf8(idW);
                    CoTaskMemFree(idW);
                }

                std::string friendlyName = idStr;
                IPropertyStore* props = nullptr;
                if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, &props)) && props) {
                    PROPVARIANT var{};
                    PropVariantInit(&var);
                    if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &var)) && var.vt == VT_LPWSTR && var.pwszVal) {
                        friendlyName = wideToUtf8(var.pwszVal);
                    }
                    PropVariantClear(&var);
                    props->Release();
                }

                if (!idStr.empty()) {
                    result.push_back(WasapiEndpointInfo{ std::move(idStr), std::move(friendlyName) });
                }
                device->Release();
            }
            collection->Release();
        }
        enumerator->Release();
    }

    if (needUninit) {
        CoUninitialize();
    }

    std::sort(result.begin(), result.end(), [](const WasapiEndpointInfo& a, const WasapiEndpointInfo& b) {
        return a.name < b.name;
    });
    return result;
}

WasapiSender::~WasapiSender() {
    stop();
}

bool WasapiSender::start(const std::string& deviceId, const std::string& deviceName, double sampleRate, size_t blockFrames) {
    stop();
    if (deviceId.empty()) {
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(statusMutex_);
        deviceName_ = deviceName.empty() ? deviceId : deviceName;
        lastError_.clear();
    }

    // Give WASAPI plenty of headroom (~45 ms, > 2x the 20 ms WASAPI render buffer)
    // so a full-buffer GetBuffer/pull never dips below maxNeeded and trips an underrun.
    // Latency here only affects the secondary WASAPI mirror, never the FiiO ASIO monitor path.
    const double sr = (sampleRate > 0.0) ? sampleRate : 48000.0;
    const size_t targetFill = std::max<size_t>(static_cast<size_t>(sr * 0.045), blockFrames * 8);
    ring_.configure(2, sr, targetFill, 32768);

    stopRequested_.store(false, std::memory_order_release);
    enabled_.store(true, std::memory_order_release);
    active_.store(false, std::memory_order_release);

    worker_ = std::thread(&WasapiSender::workerMain, this, deviceId, sr);
    return true;
}

void WasapiSender::stop() {
    stopRequested_.store(true, std::memory_order_release);
    enabled_.store(false, std::memory_order_release);
    if (worker_.joinable()) {
        worker_.join();
    }
    active_.store(false, std::memory_order_release);
}

void WasapiSender::push(const float* ch0, const float* ch1, size_t numFrames) {
    if (!enabled_.load(std::memory_order_acquire) || numFrames == 0) return;
    const float* ptrs[2] = { ch0, ch1 };
    ring_.push(ptrs, numFrames);
}

void WasapiSender::resetXruns() {
    ring_.resetXruns();
}

WasapiSenderTelemetry WasapiSender::getTelemetry() const {
    WasapiSenderTelemetry t{};
    t.enabled = enabled_.load(std::memory_order_relaxed);
    t.active = active_.load(std::memory_order_relaxed);
    t.ring = ring_.getTelemetry();
    {
        std::lock_guard<std::mutex> lock(statusMutex_);
        t.deviceName = deviceName_;
        t.error = lastError_;
    }
    return t;
}

void WasapiSender::workerMain(std::string deviceId, double sampleRate) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    DWORD taskIndex = 0;
    HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);

    auto setErr = [&](const std::string& msg) {
        std::lock_guard<std::mutex> lock(statusMutex_);
        lastError_ = msg;
    };

    IMMDeviceEnumerator* enumerator = nullptr;
    IMMDevice* device = nullptr;
    IAudioClient* audioClient = nullptr;
    IAudioRenderClient* renderClient = nullptr;
    HANDLE bufferEvent = nullptr;

    do {
        HRESULT hr = CoCreateInstance(
            __uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
            __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(&enumerator));
        if (FAILED(hr) || !enumerator) {
            setErr("Failed to create MMDeviceEnumerator.");
            break;
        }

        std::wstring idW = utf8ToWide(deviceId);
        hr = enumerator->GetDevice(idW.c_str(), &device);
        if (FAILED(hr) || !device) {
            setErr("Selected WASAPI device not found.");
            break;
        }

        hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&audioClient));
        if (FAILED(hr) || !audioClient) {
            setErr("Failed to activate WASAPI IAudioClient.");
            break;
        }

        // Request stereo float32 at the DAW's sample rate and let Windows Audio Engine
        // (AUTOCONVERTPCM) handle any sample-rate mismatch with the virtual cable!
        // Docs: https://learn.microsoft.com/en-us/windows/win32/coreaudio/audclnt-streamflags-xxx-constants
        WAVEFORMATEXTENSIBLE wfx{};
        wfx.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
        wfx.Format.nChannels = 2;
        wfx.Format.nSamplesPerSec = static_cast<DWORD>(sampleRate + 0.5);
        wfx.Format.wBitsPerSample = 32;
        wfx.Format.nBlockAlign = static_cast<WORD>(wfx.Format.nChannels * (wfx.Format.wBitsPerSample / 8));
        wfx.Format.nAvgBytesPerSec = wfx.Format.nSamplesPerSec * wfx.Format.nBlockAlign;
        wfx.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
        wfx.Samples.wValidBitsPerSample = 32;
        wfx.dwChannelMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
        wfx.SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;

        const DWORD streamFlags =
            AUDCLNT_STREAMFLAGS_EVENTCALLBACK |
            AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
            AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;

        // 20 ms shared-mode buffer (200,000 * 100 ns)
        const REFERENCE_TIME bufferDuration = 200000;
        hr = audioClient->Initialize(
            AUDCLNT_SHAREMODE_SHARED,
            streamFlags,
            bufferDuration,
            0,
            reinterpret_cast<const WAVEFORMATEX*>(&wfx),
            nullptr);
        if (FAILED(hr)) {
            char hexBuf[64]{};
            std::snprintf(hexBuf, sizeof(hexBuf), "WASAPI Initialize failed (0x%08lx)", static_cast<unsigned long>(hr));
            setErr(hexBuf);
            break;
        }

        bufferEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!bufferEvent || FAILED(audioClient->SetEventHandle(bufferEvent))) {
            setErr("Failed to configure WASAPI event callback.");
            break;
        }

        UINT32 bufferFrames = 0;
        hr = audioClient->GetBufferSize(&bufferFrames);
        if (FAILED(hr) || bufferFrames == 0) {
            setErr("Failed to query WASAPI buffer size.");
            break;
        }

        hr = audioClient->GetService(__uuidof(IAudioRenderClient), reinterpret_cast<void**>(&renderClient));
        if (FAILED(hr) || !renderClient) {
            setErr("Failed to obtain IAudioRenderClient.");
            break;
        }

        // Pre-fill initial buffer with silence before Start()
        BYTE* initData = nullptr;
        if (SUCCEEDED(renderClient->GetBuffer(bufferFrames, &initData)) && initData) {
            std::memset(initData, 0, static_cast<size_t>(bufferFrames) * 2 * sizeof(float));
            renderClient->ReleaseBuffer(bufferFrames, AUDCLNT_BUFFERFLAGS_SILENT);
        }

        hr = audioClient->Start();
        if (FAILED(hr)) {
            setErr("WASAPI Start() failed.");
            break;
        }

        active_.store(true, std::memory_order_release);

        std::vector<float> pullL(bufferFrames, 0.0f);
        std::vector<float> pullR(bufferFrames, 0.0f);
        float* pullPtrs[2] = { pullL.data(), pullR.data() };

        while (!stopRequested_.load(std::memory_order_acquire)) {
            DWORD waitRes = WaitForSingleObject(bufferEvent, 50);
            if (stopRequested_.load(std::memory_order_acquire)) break;
            if (waitRes != WAIT_OBJECT_0) continue;

            UINT32 padding = 0;
            hr = audioClient->GetCurrentPadding(&padding);
            if (FAILED(hr)) {
                setErr("WASAPI device disconnected.");
                break;
            }

            if (padding >= bufferFrames) continue;
            const UINT32 maxChunk = std::max<UINT32>(64u, static_cast<UINT32>(ring_.targetFillFrames() * 0.5));
            const UINT32 availFrames = std::min<UINT32>(bufferFrames - padding, maxChunk);

            BYTE* outData = nullptr;
            hr = renderClient->GetBuffer(availFrames, &outData);
            if (FAILED(hr) || !outData) {
                setErr("WASAPI GetBuffer failed.");
                break;
            }

            ring_.pull(pullPtrs, availFrames);

            // Interleave L/R into WASAPI float32 stereo buffer
            auto* dst = reinterpret_cast<float*>(outData);
            for (UINT32 i = 0; i < availFrames; ++i) {
                dst[i * 2]     = pullL[i];
                dst[i * 2 + 1] = pullR[i];
            }

            renderClient->ReleaseBuffer(availFrames, 0);
        }

        audioClient->Stop();
    } while (false);

    active_.store(false, std::memory_order_release);
    if (bufferEvent) CloseHandle(bufferEvent);
    safeRelease(renderClient);
    safeRelease(audioClient);
    safeRelease(device);
    safeRelease(enumerator);

    if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
    CoUninitialize();
}
