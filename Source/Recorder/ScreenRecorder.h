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

// JUCE 側から使う録画エンジンの公開インターフェース。
// Windows / D3D11 / Media Foundation のヘッダには依存しない。

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace rec
{
struct MonitorInfo
{
    std::wstring name;          // 例: "DISPLAY1"
    std::wstring adapterName;
    int adapterIndex = 0, outputIndex = 0;
    int width = 0, height = 0;
    bool isPrimary = false;
};

/** デスクトップに接続されているモニターの一覧 */
std::vector<MonitorInfo> getMonitors();

struct RecorderSettings
{
    std::wstring outputFile;
    int adapterIndex = 0, outputIndex = 0;
    int frameRate = 60;
    int videoBitrate = 16'000'000;
    bool recordSystemAudio = true;
};

struct RecorderStats
{
    double seconds = 0.0;
    int64_t framesWritten = 0, framesDropped = 0;
};

//==============================================================================
/** 画面（DXGI Desktop Duplication）とシステム音（WASAPI ループバック）を MP4 に録画する。

    スレッド構成:
      - 映像キャプチャスレッド: フレーム境界ごとに画面を取得し、GPU 上で NV12 に変換
      - 音声キャプチャスレッド: ループバックのパケットを 16bit PCM にしてサンプル化
      - エンコードスレッド    : 両方のサンプルを IMFSinkWriter へ書き込む
*/
class ScreenRecorder
{
public:
    ScreenRecorder();
    ~ScreenRecorder();

    /** 録画を開始する。失敗した場合は std::exception を投げる。 */
    void start (const RecorderSettings& settings);

    /** 録画を止めて MP4 を確定する（書き出しが終わるまでブロックする）。 */
    void stop();

    bool isRecording() const noexcept;
    RecorderStats getStats() const;

    /** 録画中の H.264 エンコーダ名（例: "NVIDIA H.264 Encoder MFT"）と、それがハードウェアかどうか */
    std::wstring getVideoEncoderName() const;
    bool isHardwareEncoder() const;

    /** 録画中・終了処理中に起きたエラー（無ければ空）。録画中に空でなくなったら stop() すること。 */
    std::string getError() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace rec
