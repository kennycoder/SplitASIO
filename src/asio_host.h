#pragma once

#include "asio_abi.h"
#include "asio_registry.h"

#include <string>
#include <vector>

class AsioChildDriver {
public:
    AsioChildDriver() = default;
    ~AsioChildDriver();

    AsioChildDriver(const AsioChildDriver&) = delete;
    AsioChildDriver& operator=(const AsioChildDriver&) = delete;

    bool open(const std::string& driverName, void* sysHandle, std::string& errOut);
    void close();

    bool isOpen() const { return driver_ != nullptr; }
    IASIO* raw() const { return driver_; }
    const std::string& driverName() const { return driverName_; }

    long numInputChannels() const { return numInChannels_; }
    long numOutputChannels() const { return numOutChannels_; }
    long minBufferSize() const { return minBufSize_; }
    long maxBufferSize() const { return maxBufSize_; }
    long preferredBufferSize() const { return prefBufSize_; }
    long bufferGranularity() const { return granularity_; }

    const std::vector<ASIOChannelInfo>& inputChannelInfos() const { return inChannelInfos_; }
    const std::vector<ASIOChannelInfo>& outputChannelInfos() const { return outChannelInfos_; }

    // Clamps/aligns a requested buffer size to what this driver supports.
    long negotiateBufferSize(long requested) const;

    ASIOError createInputBuffers(long numChannels, long bufferSize, ASIOCallbacks* callbacks);
    ASIOError createOutputBuffers(long numChannels, long bufferSize, ASIOCallbacks* callbacks);
    ASIOError disposeBuffers();

    ASIOError start();
    ASIOError stop();

    const std::vector<ASIOBufferInfo>& bufferInfos() const { return bufferInfos_; }
    long activeBufferSize() const { return activeBufferSize_; }
    bool hasBuffers() const { return buffersCreated_; }
    bool isRunning() const { return running_; }

    void refreshCaps();

private:
    IASIO* driver_ = nullptr;
    std::string driverName_;
    long numInChannels_ = 0;
    long numOutChannels_ = 0;
    long minBufSize_ = 64;
    long maxBufSize_ = 2048;
    long prefBufSize_ = 128;
    long granularity_ = -1;

    std::vector<ASIOChannelInfo> inChannelInfos_;
    std::vector<ASIOChannelInfo> outChannelInfos_;

    std::vector<ASIOBufferInfo> bufferInfos_;
    long activeBufferSize_ = 0;
    bool buffersCreated_ = false;
    bool running_ = false;
};
