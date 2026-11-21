#pragma once

#include <guiddef.h>

// {7A4E9C21-3B8D-4F62-9E15-8C04A2B7D631}
inline constexpr CLSID CLSID_AsioBridge = {
    0x7a4e9c21, 0x3b8d, 0x4f62,
    { 0x9e, 0x15, 0x8c, 0x04, 0xa2, 0xb7, 0xd6, 0x31 }
};

inline constexpr const wchar_t* kAsioBridgeClsidStrW = L"{7A4E9C21-3B8D-4F62-9E15-8C04A2B7D631}";
inline constexpr const char*    kAsioBridgeNameA     = "SplitASIO";
inline constexpr const wchar_t* kAsioBridgeNameW     = L"SplitASIO";
inline constexpr const wchar_t* kAsioBridgeDescW     = L"SplitASIO (Split Input/Output ASIO)";
