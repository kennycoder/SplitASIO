#include "asio_host.h"

#include <algorithm>
#include <objbase.h>

AsioChildDriver::~AsioChildDriver() {
    close();
}

bool AsioChildDriver::open(const std::string& driverName, void* sysHandle, std::string& errOut) {
    close();

    AsioDriverEntry entry{};
    if (!findAsioDriverByName(driverName, entry, true)) {
        errOut = "ASIO driver not found: " + driverName;
        return false;
    }

    IASIO* drv = nullptr;
    HRESULT hr = CoCreateInstance(entry.clsid, nullptr, CLSCTX_INPROC_SERVER, entry.clsid, reinterpret_cast<void**>(&drv));
    if (FAILED(hr) || !drv) {
        errOut = "CoCreateInstance failed for " + entry.name;
        return false;
    }

    if (!drv->init(sysHandle ? sysHandle : GetDesktopWindow())) {
        char msg[256]{};
        drv->getErrorMessage(msg);
        errOut = entry.name + " init() failed: " + (msg[0] ? msg : "unknown error");
        drv->Release();
        return false;
    }

    driver_ = drv;
    driverName_ = entry.name;
    refreshCaps();
    return true;
}

void AsioChildDriver::refreshCaps() {
    if (!driver_) return;

    numInChannels_ = 0;
    numOutChannels_ = 0;
    driver_->getChannels(&numInChannels_, &numOutChannels_);
    driver_->getBufferSize(&minBufSize_, &maxBufSize_, &prefBufSize_, &granularity_);

    inChannelInfos_.clear();
    for (long c = 0; c < numInChannels_; ++c) {
        ASIOChannelInfo info{};
        info.channel = c;
        info.isInput = ASIOTrue;
        if (driver_->getChannelInfo(&info) == ASE_OK) {
            inChannelInfos_.push_back(info);
        }
    }

    outChannelInfos_.clear();
    for (long c = 0; c < numOutChannels_; ++c) {
        ASIOChannelInfo info{};
        info.channel = c;
        info.isInput = ASIOFalse;
        if (driver_->getChannelInfo(&info) == ASE_OK) {
            outChannelInfos_.push_back(info);
        }
    }
}

void AsioChildDriver::close() {
    if (!driver_) return;
    if (running_) {
        stop();
    }
    if (buffersCreated_) {
        disposeBuffers();
    }
    driver_->Release();
    driver_ = nullptr;
    driverName_.clear();
    numInChannels_ = 0;
    numOutChannels_ = 0;
    inChannelInfos_.clear();
    outChannelInfos_.clear();
    bufferInfos_.clear();
    activeBufferSize_ = 0;
}

long AsioChildDriver::negotiateBufferSize(long requested) const {
    if (requested <= 0) return prefBufSize_;
    if (minBufSize_ == maxBufSize_) return minBufSize_;

    long clamped = std::clamp(requested, minBufSize_, maxBufSize_);
    if (granularity_ == -1) {
        // Per the Steinberg ASIO spec, granularity == -1 means "buffer size must be a power of 2".
        // Because clearly nothing screams "powers of two" quite like negative one.
        long p = 1;
        while (p < clamped && p * 2 <= maxBufSize_) {
            p *= 2;
        }
        return std::clamp(p, minBufSize_, maxBufSize_);
    }
    if (granularity_ > 0) {
        long steps = (clamped - minBufSize_) / granularity_;
        return std::clamp(minBufSize_ + steps * granularity_, minBufSize_, maxBufSize_);
    }
    return prefBufSize_;
}

ASIOError AsioChildDriver::createInputBuffers(long numChannels, long bufferSize, ASIOCallbacks* callbacks) {
    if (!driver_) return ASE_NotPresent;
    if (buffersCreated_) disposeBuffers();

    long count = std::min(numChannels, numInChannels_);
    if (count <= 0) return ASE_InvalidParameter;

    bufferInfos_.clear();
    bufferInfos_.reserve(static_cast<size_t>(count));
    for (long c = 0; c < count; ++c) {
        ASIOBufferInfo bi{};
        bi.isInput = ASIOTrue;
        bi.channelNum = c;
        bufferInfos_.push_back(bi);
    }

    ASIOError err = driver_->createBuffers(bufferInfos_.data(), count, bufferSize, callbacks);
    if (err == ASE_OK) {
        buffersCreated_ = true;
        activeBufferSize_ = bufferSize;
    } else {
        bufferInfos_.clear();
    }
    return err;
}

ASIOError AsioChildDriver::createOutputBuffers(long numChannels, long bufferSize, ASIOCallbacks* callbacks) {
    if (!driver_) return ASE_NotPresent;
    if (buffersCreated_) disposeBuffers();

    long count = std::min(numChannels, numOutChannels_);
    if (count <= 0) return ASE_InvalidParameter;

    bufferInfos_.clear();
    bufferInfos_.reserve(static_cast<size_t>(count));
    for (long c = 0; c < count; ++c) {
        ASIOBufferInfo bi{};
        bi.isInput = ASIOFalse;
        bi.channelNum = c;
        bufferInfos_.push_back(bi);
    }

    ASIOError err = driver_->createBuffers(bufferInfos_.data(), count, bufferSize, callbacks);
    if (err == ASE_OK) {
        buffersCreated_ = true;
        activeBufferSize_ = bufferSize;
    } else {
        bufferInfos_.clear();
    }
    return err;
}

ASIOError AsioChildDriver::disposeBuffers() {
    if (!driver_) return ASE_NotPresent;
    if (running_) stop();
    if (!buffersCreated_) return ASE_OK;

    ASIOError err = driver_->disposeBuffers();
    buffersCreated_ = false;
    activeBufferSize_ = 0;
    bufferInfos_.clear();
    return err;
}

ASIOError AsioChildDriver::start() {
    if (!driver_) return ASE_NotPresent;
    ASIOError err = driver_->start();
    if (err == ASE_OK) {
        running_ = true;
    }
    return err;
}

ASIOError AsioChildDriver::stop() {
    if (!driver_) return ASE_NotPresent;
    if (!running_) return ASE_OK;
    ASIOError err = driver_->stop();
    running_ = false;
    return err;
}
