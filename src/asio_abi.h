#pragma once

#include <cstdint>
#include <unknwn.h>

// Clean-room binary-compatible Steinberg ASIO 2.3 ABI definitions for Windows x64.
//
// OBSCURE ABI WARNING: Steinberg's SDK wraps Windows structs in `#pragma pack(push, 4)`
// ... a glorious relic from 1997 that forces 4-byte alignment even on 64-bit x64.
// Remove this pragma and every DAW on Earth will happily read garbage and explode.
// Docs: https://learn.microsoft.com/en-us/cpp/preprocessor/pack

#pragma pack(push, 4)

using ASIOBool = long;
constexpr ASIOBool ASIOFalse = 0;
constexpr ASIOBool ASIOTrue = 1;

using ASIOSampleRate = double;

struct ASIOSamples {
    unsigned long hi;
    unsigned long lo;
};

struct ASIOTimeStamp {
    unsigned long hi;
    unsigned long lo;
};

using ASIOSampleType = long;
enum : ASIOSampleType {
    ASIOSTInt16MSB   = 0,
    ASIOSTInt24MSB   = 1,
    ASIOSTInt32MSB   = 2,
    ASIOSTFloat32MSB = 3,
    ASIOSTFloat64MSB = 4,

    ASIOSTInt32MSB16 = 8,
    ASIOSTInt32MSB18 = 9,
    ASIOSTInt32MSB20 = 10,
    ASIOSTInt32MSB24 = 11,

    ASIOSTInt16LSB   = 16,
    ASIOSTInt24LSB   = 17,
    ASIOSTInt32LSB   = 18,
    ASIOSTFloat32LSB = 19,
    ASIOSTFloat64LSB = 20,

    ASIOSTInt32LSB16 = 24,
    ASIOSTInt32LSB18 = 25,
    ASIOSTInt32LSB20 = 26,
    ASIOSTInt32LSB24 = 27,

    ASIOSTDSDInt8LSB1 = 32,
    ASIOSTDSDInt8MSB1 = 33,
    ASIOSTDSDInt8NER8 = 40,
};

using ASIOError = long;
enum : ASIOError {
    ASE_OK               = 0,
    // Yes, 0x3f4847a0 really is Steinberg's official "success, but fancier than 0" constant
    // (returned by future() and a handful of methods). Nobody knows why; we just don't question it.
    ASE_SUCCESS          = 0x3f4847a0,
    ASE_NotPresent       = -1000,
    ASE_HWMalfunction    = -999,
    ASE_InvalidParameter = -998,
    ASE_InvalidMode      = -997,
    ASE_SPNotAdvancing   = -996,
    ASE_NoClock          = -995,
    ASE_NoMemory         = -994,
};

struct ASIOClockSource {
    long index;
    long associatedChannel;
    long associatedGroup;
    ASIOBool isCurrentSource;
    char name[32];
};

struct ASIOChannelInfo {
    long channel;
    ASIOBool isInput;
    ASIOBool isActive;
    long channelGroup;
    ASIOSampleType type;
    char name[32];
};

struct ASIOBufferInfo {
    ASIOBool isInput;
    long channelNum;
    void* buffers[2];
};

struct ASIOTimeInfo {
    double speed;
    ASIOTimeStamp systemTime;
    ASIOSamples samplePosition;
    ASIOSampleRate sampleRate;
    unsigned long flags;
    char reserved[12];
};

enum : unsigned long {
    kSystemTimeValid     = 1u << 0,
    kSamplePositionValid = 1u << 1,
    kSampleRateValid     = 1u << 2,
    kSpeedValid          = 1u << 3,
    kSampleRateChanged   = 1u << 4,
    kClockSourceChanged  = 1u << 5,
};

struct ASIOTimeCode {
    double speed;
    ASIOSamples timeCodeSamples;
    unsigned long flags;
    char future[64];
};

struct ASIOTime {
    long reserved[4];
    ASIOTimeInfo timeInfo;
    ASIOTimeCode timeCode;
};

struct ASIOCallbacks {
    void (*bufferSwitch)(long doubleBufferIndex, ASIOBool directProcess);
    void (*sampleRateDidChange)(ASIOSampleRate sRate);
    long (*asioMessage)(long selector, long value, void* message, double* opt);
    ASIOTime* (*bufferSwitchTimeInfo)(ASIOTime* params, long doubleBufferIndex, ASIOBool directProcess);
};

enum : long {
    kAsioSelectorSupported    = 1,
    kAsioEngineVersion        = 2,
    kAsioResetRequest         = 3,
    kAsioBufferSizeChange     = 4,
    kAsioResyncRequest        = 5,
    kAsioLatenciesChanged     = 6,
    kAsioSupportsTimeInfo     = 7,
    kAsioSupportsTimeCode     = 8,
    kAsioMMCCommand           = 9,
    kAsioSupportsInputMonitor = 10,
    kAsioSupportsInputGain    = 11,
    kAsioSupportsInputMeter   = 12,
    kAsioSupportsOutputGain   = 13,
    kAsioSupportsOutputMeter  = 14,
    kAsioOverload             = 15,
};

// IASIO COM interface (inherits from IUnknown; methods use standard x64 calling convention).
// Do NOT reorder these virtual methods unless you enjoy watching DAWs call controlPanel()
// when they meant to call start().
// Also note ASIO's favorite COM eccentricity: there is no shared IID_IASIO.. every driver
// expects its own CLSID to be passed as the IID to CoCreateInstance().
// Docs: https://learn.microsoft.com/en-us/windows/win32/api/combaseapi/nf-combaseapi-cocreateinstance
struct IASIO : public IUnknown {
    virtual ASIOBool init(void* sysHandle) = 0;
    virtual void getDriverName(char* name) = 0;
    virtual long getDriverVersion() = 0;
    virtual void getErrorMessage(char* string) = 0;
    virtual ASIOError start() = 0;
    virtual ASIOError stop() = 0;
    virtual ASIOError getChannels(long* numInputChannels, long* numOutputChannels) = 0;
    virtual ASIOError getLatencies(long* inputLatency, long* outputLatency) = 0;
    virtual ASIOError getBufferSize(long* minSize, long* maxSize, long* preferredSize, long* granularity) = 0;
    virtual ASIOError canSampleRate(ASIOSampleRate sampleRate) = 0;
    virtual ASIOError getSampleRate(ASIOSampleRate* sampleRate) = 0;
    virtual ASIOError setSampleRate(ASIOSampleRate sampleRate) = 0;
    virtual ASIOError getClockSources(ASIOClockSource* clocks, long* numSources) = 0;
    virtual ASIOError setClockSource(long reference) = 0;
    virtual ASIOError getSamplePosition(ASIOSamples* sPos, ASIOTimeStamp* tStamp) = 0;
    virtual ASIOError getChannelInfo(ASIOChannelInfo* info) = 0;
    virtual ASIOError createBuffers(ASIOBufferInfo* bufferInfos, long numChannels, long bufferSize, ASIOCallbacks* callbacks) = 0;
    virtual ASIOError disposeBuffers() = 0;
    virtual ASIOError controlPanel() = 0;
    virtual ASIOError future(long selector, void* opt) = 0;
    virtual ASIOError outputReady() = 0;
};

#pragma pack(pop)

inline const char* asioSampleTypeName(ASIOSampleType type) {
    switch (type) {
        case ASIOSTInt16LSB:   return "Int16LSB";
        case ASIOSTInt24LSB:   return "Int24LSB";
        case ASIOSTInt32LSB:   return "Int32LSB";
        case ASIOSTFloat32LSB: return "Float32LSB";
        case ASIOSTFloat64LSB: return "Float64LSB";
        case ASIOSTInt32LSB16: return "Int32LSB16";
        case ASIOSTInt32LSB18: return "Int32LSB18";
        case ASIOSTInt32LSB20: return "Int32LSB20";
        case ASIOSTInt32LSB24: return "Int32LSB24";
        case ASIOSTInt16MSB:   return "Int16MSB";
        case ASIOSTInt24MSB:   return "Int24MSB";
        case ASIOSTInt32MSB:   return "Int32MSB";
        case ASIOSTFloat32MSB: return "Float32MSB";
        case ASIOSTFloat64MSB: return "Float64MSB";
        default:               return "Unknown";
    }
}

inline uint64_t asioSamplesToU64(const ASIOSamples& s) {
    return (static_cast<uint64_t>(s.hi) << 32) | static_cast<uint64_t>(s.lo);
}

inline ASIOSamples u64ToAsioSamples(uint64_t v) {
    ASIOSamples s{};
    s.hi = static_cast<unsigned long>(v >> 32);
    s.lo = static_cast<unsigned long>(v & 0xFFFFFFFFu);
    return s;
}

inline ASIOTimeStamp u64ToAsioTimeStamp(uint64_t v) {
    ASIOTimeStamp t{};
    t.hi = static_cast<unsigned long>(v >> 32);
    t.lo = static_cast<unsigned long>(v & 0xFFFFFFFFu);
    return t;
}
