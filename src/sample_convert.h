#pragma once

#include "asio_abi.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace sample_convert {

inline constexpr float kInt32ScaleInv = 1.0f / 2147483648.0f;
inline constexpr float kInt32Scale    = 2147483647.0f;
inline constexpr float kInt24ScaleInv = 1.0f / 8388608.0f;
inline constexpr float kInt24Scale    = 8388607.0f;
inline constexpr float kInt16ScaleInv = 1.0f / 32768.0f;
inline constexpr float kInt16Scale    = 32767.0f;

inline size_t sampleByteSize(ASIOSampleType type) {
    switch (type) {
        case ASIOSTInt16LSB:
        case ASIOSTInt16MSB:
            return 2;
        case ASIOSTInt24LSB:
        case ASIOSTInt24MSB:
            return 3;
        case ASIOSTInt32LSB:
        case ASIOSTInt32MSB:
        case ASIOSTFloat32LSB:
        case ASIOSTFloat32MSB:
        case ASIOSTInt32LSB16:
        case ASIOSTInt32LSB18:
        case ASIOSTInt32LSB20:
        case ASIOSTInt32LSB24:
        case ASIOSTInt32MSB16:
        case ASIOSTInt32MSB18:
        case ASIOSTInt32MSB20:
        case ASIOSTInt32MSB24:
            return 4;
        case ASIOSTFloat64LSB:
        case ASIOSTFloat64MSB:
            return 8;
        default:
            return 4;
    }
}

inline void asioToFloat(const void* src, float* dst, size_t count, ASIOSampleType type) {
    if (!src || !dst || count == 0) return;

    switch (type) {
        case ASIOSTInt32LSB: {
            const auto* s = static_cast<const int32_t*>(src);
            for (size_t i = 0; i < count; ++i) {
                dst[i] = static_cast<float>(s[i]) * kInt32ScaleInv;
            }
            break;
        }
        case ASIOSTFloat32LSB: {
            std::memcpy(dst, src, count * sizeof(float));
            break;
        }
        case ASIOSTFloat64LSB: {
            const auto* s = static_cast<const double*>(src);
            for (size_t i = 0; i < count; ++i) {
                dst[i] = static_cast<float>(s[i]);
            }
            break;
        }
        case ASIOSTInt24LSB: {
            // Packed 3-byte little-endian signed integers — because nothing says "CPU-friendly"
            // like reading unaligned 24-bit samples one byte at a time.
            // Casting the MSB to int8_t sign-extends bits 24..31 automatically.
            // See: https://en.wikipedia.org/wiki/Sign_extension
            const auto* b = static_cast<const uint8_t*>(src);
            for (size_t i = 0; i < count; ++i) {
                int32_t v = static_cast<int32_t>(b[0]) |
                            (static_cast<int32_t>(b[1]) << 8) |
                            (static_cast<int32_t>(static_cast<int8_t>(b[2])) << 16);
                dst[i] = static_cast<float>(v) * kInt24ScaleInv;
                b += 3;
            }
            break;
        }
        case ASIOSTInt16LSB: {
            const auto* s = static_cast<const int16_t*>(src);
            for (size_t i = 0; i < count; ++i) {
                dst[i] = static_cast<float>(s[i]) * kInt16ScaleInv;
            }
            break;
        }
        case ASIOSTInt32LSB24: {
            // 24-bit audio stuffed into the low bits of a 32-bit int; shift up and arithmetic-shift
            // back down to sign-extend bit 23 cleanly.
            const auto* s = static_cast<const int32_t*>(src);
            for (size_t i = 0; i < count; ++i) {
                int32_t v = (s[i] << 8) >> 8;
                dst[i] = static_cast<float>(v) * kInt24ScaleInv;
            }
            break;
        }
        case ASIOSTInt32LSB16: {
            const auto* s = static_cast<const int32_t*>(src);
            for (size_t i = 0; i < count; ++i) {
                int32_t v = (s[i] << 16) >> 16;
                dst[i] = static_cast<float>(v) * kInt16ScaleInv;
            }
            break;
        }
        default: {
            std::memset(dst, 0, count * sizeof(float));
            break;
        }
    }
}

inline void floatToAsio(const float* src, void* dst, size_t count, ASIOSampleType type) {
    if (!src || !dst || count == 0) return;

    switch (type) {
        case ASIOSTInt32LSB: {
            auto* d = static_cast<int32_t*>(dst);
            for (size_t i = 0; i < count; ++i) {
                float v = std::clamp(src[i], -1.0f, 1.0f);
                d[i] = static_cast<int32_t>(std::lrintf(v * kInt32Scale));
            }
            break;
        }
        case ASIOSTFloat32LSB: {
            std::memcpy(dst, src, count * sizeof(float));
            break;
        }
        case ASIOSTFloat64LSB: {
            auto* d = static_cast<double*>(dst);
            for (size_t i = 0; i < count; ++i) {
                d[i] = static_cast<double>(src[i]);
            }
            break;
        }
        case ASIOSTInt24LSB: {
            auto* b = static_cast<uint8_t*>(dst);
            for (size_t i = 0; i < count; ++i) {
                float v = std::clamp(src[i], -1.0f, 1.0f);
                int32_t s = static_cast<int32_t>(std::lrintf(v * kInt24Scale));
                b[0] = static_cast<uint8_t>(s & 0xFF);
                b[1] = static_cast<uint8_t>((s >> 8) & 0xFF);
                b[2] = static_cast<uint8_t>((s >> 16) & 0xFF);
                b += 3;
            }
            break;
        }
        case ASIOSTInt16LSB: {
            auto* d = static_cast<int16_t*>(dst);
            for (size_t i = 0; i < count; ++i) {
                float v = std::clamp(src[i], -1.0f, 1.0f);
                d[i] = static_cast<int16_t>(std::lrintf(v * kInt16Scale));
            }
            break;
        }
        case ASIOSTInt32LSB24: {
            auto* d = static_cast<int32_t*>(dst);
            for (size_t i = 0; i < count; ++i) {
                float v = std::clamp(src[i], -1.0f, 1.0f);
                d[i] = static_cast<int32_t>(std::lrintf(v * kInt24Scale));
            }
            break;
        }
        case ASIOSTInt32LSB16: {
            auto* d = static_cast<int32_t*>(dst);
            for (size_t i = 0; i < count; ++i) {
                float v = std::clamp(src[i], -1.0f, 1.0f);
                d[i] = static_cast<int32_t>(std::lrintf(v * kInt16Scale));
            }
            break;
        }
        default: {
            std::memset(dst, 0, count * sampleByteSize(type));
            break;
        }
    }
}

} // namespace sample_convert
