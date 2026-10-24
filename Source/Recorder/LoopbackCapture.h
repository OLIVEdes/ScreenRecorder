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

#include "RecorderCommon.h"

#include <audioclient.h>
#include <mmdeviceapi.h>

#include <atomic>
#include <functional>
#include <future>
#include <mutex>
#include <thread>
#include <vector>

namespace rec
{
//==============================================================================
/** 既定の再生デバイスを WASAPI ループバックで録音し、16bit ステレオ PCM の IMFSample として渡す。

    - 何も再生されていない間はループバックにパケットが来ないため、同じデバイスへ無音を再生して
      ストリームを途切れさせない。それでも生じた隙間はタイムスタンプを見て無音で埋める。
    - サンプル時刻は「書き出したサンプル数」から計算し、録画開始時刻を 0 とする。
    - 既定デバイスが切り替わった（ヘッドホンを抜いた等）場合は新しい既定デバイスへ自動で繋ぎ直す。
*/
class LoopbackCapture
{
public:
    using SampleCallback = std::function<void (ComPtr<IMFSample>)>;

    /** 音声スレッドを起動し、デバイスの初期化が終わるまで待つ（失敗時は例外）。 */
    LoopbackCapture();
    ~LoopbackCapture();

    UINT32 getSampleRate() const noexcept    { return sampleRate; }
    static constexpr UINT32 numChannels = 2;

    /** timelineStart（now100ns() の値）をファイル上の 0 秒としてサンプルの受け渡しを始める。 */
    void start (int64_t timelineStart, SampleCallback callback);
    void stop();

    std::string getError() const;

private:
    enum class SourceFormat { pcm16Stereo, float32, int16 };

    void run (std::promise<void>* initialised);
    void openDevice();
    void closeDevice();
    void reopenDevice();
    void feedSilence();
    void readPackets();
    void handlePacket (const BYTE* data, UINT32 numFrames, DWORD flags, UINT64 packetTime);
    void writeFrames (const int16_t* frames, UINT32 numFrames);
    void writeSilence (int64_t numFrames);
    void setError (const std::string& message);

    ComPtr<IAudioClient> captureAudioClient, renderAudioClient;
    ComPtr<IAudioCaptureClient> captureClient;
    ComPtr<IAudioRenderClient> renderClient;
    UINT32 renderBufferFrames = 0;

    UINT32 sampleRate = 0;
    SourceFormat sourceFormat = SourceFormat::pcm16Stereo;
    UINT32 sourceChannels = 2;

    SampleCallback sampleCallback;
    std::atomic<int64_t> timelineStart { -1 };
    int64_t framesWritten = 0;
    std::vector<int16_t> conversionBuffer;

    std::atomic<bool> stopRequested { false };
    std::thread thread;

    mutable std::mutex errorLock;
    std::string error;
};

} // namespace rec
