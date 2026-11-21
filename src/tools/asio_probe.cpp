#include "../asio_abi.h"
#include "../asio_registry.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <objbase.h>
#include <thread>
#include <vector>
#include <windows.h>

namespace {

std::atomic<uint64_t> g_inCallbacks{0};
std::atomic<uint64_t> g_outCallbacks{0};
std::atomic<uint64_t> g_inFrames{0};
std::atomic<uint64_t> g_outFrames{0};
long g_inBufSize = 0;
long g_outBufSize = 0;
IASIO* g_outDriver = nullptr;

void inBufferSwitch(long index, ASIOBool) {
    (void)index;
    g_inCallbacks.fetch_add(1, std::memory_order_relaxed);
    g_inFrames.fetch_add(static_cast<uint64_t>(g_inBufSize), std::memory_order_relaxed);
}
void inSampleRateDidChange(ASIOSampleRate sr) {
    std::printf("  [IN] sampleRateDidChange: %.1f\n", sr);
}
long inAsioMessage(long selector, long value, void*, double*) {
    if (selector == kAsioSelectorSupported) {
        return (value == kAsioEngineVersion || value == kAsioSupportsTimeInfo) ? 1 : 0;
    }
    if (selector == kAsioEngineVersion) return 2;
    if (selector == kAsioSupportsTimeInfo) return 1;
    return 0;
}
ASIOTime* inBufferSwitchTimeInfo(ASIOTime* params, long index, ASIOBool directProcess) {
    inBufferSwitch(index, directProcess);
    return params;
}

void outBufferSwitch(long index, ASIOBool) {
    (void)index;
    g_outCallbacks.fetch_add(1, std::memory_order_relaxed);
    g_outFrames.fetch_add(static_cast<uint64_t>(g_outBufSize), std::memory_order_relaxed);
    if (g_outDriver) {
        g_outDriver->outputReady();
    }
}
void outSampleRateDidChange(ASIOSampleRate sr) {
    std::printf("  [OUT] sampleRateDidChange: %.1f\n", sr);
}
long outAsioMessage(long selector, long value, void*, double*) {
    if (selector == kAsioSelectorSupported) {
        return (value == kAsioEngineVersion || value == kAsioSupportsTimeInfo) ? 1 : 0;
    }
    if (selector == kAsioEngineVersion) return 2;
    if (selector == kAsioSupportsTimeInfo) return 1;
    return 0;
}
ASIOTime* outBufferSwitchTimeInfo(ASIOTime* params, long index, ASIOBool directProcess) {
    outBufferSwitch(index, directProcess);
    return params;
}

void inspectDriver(const char* label, IASIO* drv) {
    char name[128]{};
    drv->getDriverName(name);
    long ver = drv->getDriverVersion();
    std::printf("\n=== %s: %s (ver %ld) ===\n", label, name, ver);

    long inCh = 0, outCh = 0;
    if (drv->getChannels(&inCh, &outCh) == ASE_OK) {
        std::printf("  Channels: %ld in, %ld out\n", inCh, outCh);
    }

    long minSz = 0, maxSz = 0, prefSz = 0, gran = 0;
    if (drv->getBufferSize(&minSz, &maxSz, &prefSz, &gran) == ASE_OK) {
        std::printf("  BufferSize: min=%ld, max=%ld, preferred=%ld, granularity=%ld\n", minSz, maxSz, prefSz, gran);
    }

    ASIOSampleRate sr = 0;
    if (drv->getSampleRate(&sr) == ASE_OK) {
        std::printf("  Current SampleRate: %.1f Hz\n", sr);
    }

    const double rates[] = { 44100.0, 48000.0, 88200.0, 96000.0, 192000.0 };
    std::printf("  Supported Rates:");
    for (double r : rates) {
        if (drv->canSampleRate(r) == ASE_OK) {
            std::printf(" %.0f", r);
        }
    }
    std::printf("\n");

    for (long c = 0; c < inCh; ++c) {
        ASIOChannelInfo info{};
        info.channel = c;
        info.isInput = ASIOTrue;
        if (drv->getChannelInfo(&info) == ASE_OK) {
            std::printf("  IN  [%ld] '%s' type=%s (%ld)\n", c, info.name, asioSampleTypeName(info.type), info.type);
        }
    }
    for (long c = 0; c < outCh; ++c) {
        ASIOChannelInfo info{};
        info.channel = c;
        info.isInput = ASIOFalse;
        if (drv->getChannelInfo(&info) == ASE_OK) {
            std::printf("  OUT [%ld] '%s' type=%s (%ld)\n", c, info.name, asioSampleTypeName(info.type), info.type);
        }
    }
}

} // namespace

int main() {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(hr)) {
        std::printf("CoInitializeEx failed: 0x%08lx\n", static_cast<unsigned long>(hr));
        return 1;
    }

    auto drivers = enumerateAsioDrivers(false);
    std::printf("Found %zu 64-bit ASIO driver(s):\n", drivers.size());
    for (const auto& d : drivers) {
        std::printf("  - %s | %s | %s\n", d.name.c_str(), d.description.c_str(), d.clsidStr.c_str());
    }

    AsioDriverEntry inEntry{}, outEntry{};
    if (!findAsioDriverByName("Focusrite USB ASIO", inEntry, true)) {
        std::printf("Could not find 'Focusrite USB ASIO'\n");
        CoUninitialize();
        return 1;
    }
    if (!findAsioDriverByName("FiiO ASIO Driver", outEntry, true)) {
        std::printf("Could not find 'FiiO ASIO Driver'\n");
        CoUninitialize();
        return 1;
    }

    IASIO* inDrv = nullptr;
    IASIO* outDrv = nullptr;

    hr = CoCreateInstance(inEntry.clsid, nullptr, CLSCTX_INPROC_SERVER, inEntry.clsid, reinterpret_cast<void**>(&inDrv));
    if (FAILED(hr) || !inDrv) {
        std::printf("CoCreateInstance(Focusrite) failed: 0x%08lx\n", static_cast<unsigned long>(hr));
        CoUninitialize();
        return 1;
    }

    hr = CoCreateInstance(outEntry.clsid, nullptr, CLSCTX_INPROC_SERVER, outEntry.clsid, reinterpret_cast<void**>(&outDrv));
    if (FAILED(hr) || !outDrv) {
        std::printf("CoCreateInstance(FiiO) failed: 0x%08lx\n", static_cast<unsigned long>(hr));
        inDrv->Release();
        CoUninitialize();
        return 1;
    }

    if (!inDrv->init(GetDesktopWindow())) {
        char err[256]{};
        inDrv->getErrorMessage(err);
        std::printf("Focusrite init() failed: %s\n", err);
    }
    if (!outDrv->init(GetDesktopWindow())) {
        char err[256]{};
        outDrv->getErrorMessage(err);
        std::printf("FiiO init() failed: %s\n", err);
    }

    inspectDriver("INPUT", inDrv);
    inspectDriver("OUTPUT", outDrv);

    // Attempt simultaneous streaming test at 48000 Hz
    std::printf("\n=== Simultaneous Streaming Test (2.0s @ 48000 Hz) ===\n");
    inDrv->setSampleRate(48000.0);
    outDrv->setSampleRate(48000.0);

    long inCh = 0, inOutCh = 0;
    inDrv->getChannels(&inCh, &inOutCh);
    long outInCh = 0, outCh = 0;
    outDrv->getChannels(&outInCh, &outCh);

    long minSz = 0, maxSz = 0, prefSz = 0, gran = 0;
    inDrv->getBufferSize(&minSz, &maxSz, &prefSz, &gran);
    g_inBufSize = prefSz;
    outDrv->getBufferSize(&minSz, &maxSz, &prefSz, &gran);
    g_outBufSize = prefSz;

    std::vector<ASIOBufferInfo> inBufs;
    for (long c = 0; c < inCh; ++c) {
        ASIOBufferInfo bi{};
        bi.isInput = ASIOTrue;
        bi.channelNum = c;
        inBufs.push_back(bi);
    }
    std::vector<ASIOBufferInfo> outBufs;
    for (long c = 0; c < outCh; ++c) {
        ASIOBufferInfo bi{};
        bi.isInput = ASIOFalse;
        bi.channelNum = c;
        outBufs.push_back(bi);
    }

    ASIOCallbacks inCbs{ inBufferSwitch, inSampleRateDidChange, inAsioMessage, inBufferSwitchTimeInfo };
    ASIOCallbacks outCbs{ outBufferSwitch, outSampleRateDidChange, outAsioMessage, outBufferSwitchTimeInfo };

    ASIOError eIn = inDrv->createBuffers(inBufs.data(), static_cast<long>(inBufs.size()), g_inBufSize, &inCbs);
    std::printf("  Focusrite createBuffers(%zu ch, %ld frames) -> %ld\n", inBufs.size(), g_inBufSize, eIn);

    g_outDriver = outDrv;
    ASIOError eOut = outDrv->createBuffers(outBufs.data(), static_cast<long>(outBufs.size()), g_outBufSize, &outCbs);
    std::printf("  FiiO      createBuffers(%zu ch, %ld frames) -> %ld\n", outBufs.size(), g_outBufSize, eOut);

    if (eIn == ASE_OK && eOut == ASE_OK) {
        long inLatIn = 0, inLatOut = 0, outLatIn = 0, outLatOut = 0;
        inDrv->getLatencies(&inLatIn, &inLatOut);
        outDrv->getLatencies(&outLatIn, &outLatOut);
        std::printf("  Focusrite latencies: in=%ld, out=%ld\n", inLatIn, inLatOut);
        std::printf("  FiiO      latencies: in=%ld, out=%ld\n", outLatIn, outLatOut);

        ASIOError sIn = inDrv->start();
        ASIOError sOut = outDrv->start();
        std::printf("  start() -> Focusrite=%ld, FiiO=%ld\n", sIn, sOut);

        if (sIn == ASE_OK && sOut == ASE_OK) {
            std::this_thread::sleep_for(std::chrono::seconds(2));
            uint64_t ic = g_inCallbacks.load();
            uint64_t oc = g_outCallbacks.load();
            uint64_t ifr = g_inFrames.load();
            uint64_t ofr = g_outFrames.load();
            std::printf("  After 2s: Focusrite=%llu cbs (%llu frames), FiiO=%llu cbs (%llu frames)\n",
                ic, ifr, oc, ofr);
        }

        outDrv->stop();
        inDrv->stop();
    }

    if (eOut == ASE_OK) outDrv->disposeBuffers();
    if (eIn == ASE_OK) inDrv->disposeBuffers();

    g_outDriver = nullptr;
    outDrv->Release();
    inDrv->Release();
    CoUninitialize();
    std::printf("\nProbe complete!\n");
    return 0;
}
