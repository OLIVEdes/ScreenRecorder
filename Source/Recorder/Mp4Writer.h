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

#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <utility>

namespace rec
{
struct VideoStreamFormat
{
    UINT32 width = 0, height = 0, frameRate = 60, bitrate = 0;
};

struct AudioStreamFormat
{
    UINT32 sampleRate = 48000, numChannels = 2;
};

//==============================================================================
/** IMFSinkWriter で MP4 (H.264 + AAC) を書き出す。

    SinkWriter には IMFDXGIDeviceManager を渡してあるので、ハードウェアエンコーダは
    NV12 テクスチャを GPU 上のまま受け取る（ゼロコピー）。
    WriteSample は専用スレッドから呼ぶので、キャプチャ側のスレッドはエンコードを待たない。
*/
class Mp4Writer
{
public:
    Mp4Writer (const std::wstring& path, IMFDXGIDeviceManager* deviceManager,
               const VideoStreamFormat& video, const AudioStreamFormat* audio);
    ~Mp4Writer();

    void writeVideo (ComPtr<IMFSample> sample)  { enqueue (videoStream, std::move (sample)); }
    void writeAudio (ComPtr<IMFSample> sample)  { enqueue (audioStream, std::move (sample)); }

    /** キューに残ったサンプルを書き切ってからファイルを確定する（ブロックする）。 */
    void finish();

    /** エンコードスレッドで起きたエラー（無ければ空） */
    std::string getError() const;

    /** SinkWriter が選んだ H.264 エンコーダ（ハードウェアならゼロコピー経路で動いている） */
    std::wstring getVideoEncoderName() const    { return videoEncoderName; }
    bool isHardwareEncoder() const noexcept     { return hardwareEncoder; }

private:
    void enqueue (DWORD streamIndex, ComPtr<IMFSample> sample);
    void run();
    void setError (const std::string& message);
    void findVideoEncoder();

    ComPtr<IMFSinkWriter> sinkWriter;
    DWORD videoStream = 0, audioStream = 0;
    std::wstring videoEncoderName;
    bool hardwareEncoder = false;

    mutable std::mutex lock;
    std::condition_variable queueChanged;
    std::deque<std::pair<DWORD, ComPtr<IMFSample>>> queue;
    bool finishRequested = false;
    std::string error;

    std::thread thread;
};

} // namespace rec
