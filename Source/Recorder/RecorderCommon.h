/*
    ScreenRecorder
    Copyright (C) 2026 OLIVEdes

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU Affero General Public License as published
    by the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU Affero General Public License for more details.

    You should have received a copy of the GNU Affero General Public License
    along with this program.  If not, see <https://www.gnu.org/licenses/>.

    SPDX-License-Identifier: AGPL-3.0-or-later
*/

#pragma once

// レコーダー内部 (.cpp) 専用の共通ヘッダ。
// Windows / D3D11 / Media Foundation のヘッダを含むため、JUCE 側のヘッダからは include しないこと。

#ifndef NOMINMAX
 #define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
 #define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>
#include <wrl/client.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>

#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <string>

namespace rec
{
using Microsoft::WRL::ComPtr;

//==============================================================================
/** 失敗した HRESULT と、失敗した式をメッセージに持つ例外 */
class HResultError : public std::runtime_error
{
public:
    HResultError (HRESULT hr, const char* expression)
        : std::runtime_error (describe (hr, expression)), result (hr) {}

    const HRESULT result;

private:
    static std::string describe (HRESULT hr, const char* expression)
    {
        char buffer[512];
        std::snprintf (buffer, sizeof (buffer), "%s failed (HRESULT 0x%08lX)",
                       expression, static_cast<unsigned long> (hr));
        return buffer;
    }
};

inline void throwIfFailed (HRESULT hr, const char* expression)
{
    if (FAILED (hr))
        throw HResultError (hr, expression);
}

#define REC_CHECK(expression) ::rec::throwIfFailed ((expression), #expression)

//==============================================================================
/** QPC を 100ns 単位に変換した現在時刻。
    WASAPI の u64QPCPosition や Media Foundation のサンプル時刻と同じ単位・時間軸になる。
*/
inline int64_t now100ns() noexcept
{
    static const int64_t frequency = []
    {
        LARGE_INTEGER f;
        QueryPerformanceFrequency (&f);
        return f.QuadPart;
    }();

    LARGE_INTEGER counter;
    QueryPerformanceCounter (&counter);

    // counter * 10^7 は長時間稼働で int64 を溢れるため、商と余りに分けて計算する
    return (counter.QuadPart / frequency) * 10'000'000
         + (counter.QuadPart % frequency) * 10'000'000 / frequency;
}

//==============================================================================
/** ワーカースレッド用: COM を MTA で初期化するスコープガード */
struct ScopedComInitialiser
{
    ScopedComInitialiser()  : result (CoInitializeEx (nullptr, COINIT_MULTITHREADED)) {}
    ~ScopedComInitialiser() { if (SUCCEEDED (result)) CoUninitialize(); }

    const HRESULT result;
};

} // namespace rec
