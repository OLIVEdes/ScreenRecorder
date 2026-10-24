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

#include "Mp4Writer.h"

#include <strmif.h>   // ICodecAPI
#include <codecapi.h>

#pragma comment (lib, "mfplat.lib")
#pragma comment (lib, "mfreadwrite.lib")
#pragma comment (lib, "mfuuid.lib")

namespace rec
{
namespace
{
    ComPtr<IMFMediaType> createVideoType (const GUID& subtype, const VideoStreamFormat& format)
    {
        ComPtr<IMFMediaType> type;
        REC_CHECK (MFCreateMediaType (&type));
        REC_CHECK (type->SetGUID (MF_MT_MAJOR_TYPE, MFMediaType_Video));
        REC_CHECK (type->SetGUID (MF_MT_SUBTYPE, subtype));
        REC_CHECK (type->SetUINT32 (MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive));
        REC_CHECK (MFSetAttributeSize (type.Get(), MF_MT_FRAME_SIZE, format.width, format.height));
        REC_CHECK (MFSetAttributeRatio (type.Get(), MF_MT_FRAME_RATE, format.frameRate, 1));
        REC_CHECK (MFSetAttributeRatio (type.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1));

        // Video Processor の出力（BT.709 / リミテッドレンジ）と一致させ、プレーヤーにも正しく伝える
        REC_CHECK (type->SetUINT32 (MF_MT_YUV_MATRIX, MFVideoTransferMatrix_BT709));
        REC_CHECK (type->SetUINT32 (MF_MT_VIDEO_PRIMARIES, MFVideoPrimaries_BT709));
        REC_CHECK (type->SetUINT32 (MF_MT_TRANSFER_FUNCTION, MFVideoTransFunc_709));
        REC_CHECK (type->SetUINT32 (MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_16_235));
        return type;
    }

    ComPtr<IMFMediaType> createAudioType (const GUID& subtype, const AudioStreamFormat& format)
    {
        ComPtr<IMFMediaType> type;
        REC_CHECK (MFCreateMediaType (&type));
        REC_CHECK (type->SetGUID (MF_MT_MAJOR_TYPE, MFMediaType_Audio));
        REC_CHECK (type->SetGUID (MF_MT_SUBTYPE, subtype));
        REC_CHECK (type->SetUINT32 (MF_MT_AUDIO_BITS_PER_SAMPLE, 16));
        REC_CHECK (type->SetUINT32 (MF_MT_AUDIO_SAMPLES_PER_SECOND, format.sampleRate));
        REC_CHECK (type->SetUINT32 (MF_MT_AUDIO_NUM_CHANNELS, format.numChannels));
        return type;
    }

    void trySetCodecValue (ICodecAPI* codec, const GUID& key, UINT32 value)
    {
        VARIANT variant {};
        variant.vt    = VT_UI4;
        variant.ulVal = value;

        codec->SetValue (&key, &variant); // 対応していないエンコーダもあるので失敗は無視する
    }
}

//==============================================================================
Mp4Writer::Mp4Writer (const std::wstring& path, IMFDXGIDeviceManager* deviceManager,
                      const VideoStreamFormat& video, const AudioStreamFormat* audio)
{
    ComPtr<IMFAttributes> attributes;
    REC_CHECK (MFCreateAttributes (&attributes, 4));
    REC_CHECK (attributes->SetUINT32 (MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE));
    REC_CHECK (attributes->SetUnknown (MF_SINK_WRITER_D3D_MANAGER, deviceManager));
    REC_CHECK (attributes->SetGUID (MF_TRANSCODE_CONTAINERTYPE, MFTranscodeContainerType_MPEG4));

    // 流量制御は NV12 プールの枯渇で行うので、WriteSample がブロックするスロットリングは無効にする
    REC_CHECK (attributes->SetUINT32 (MF_SINK_WRITER_DISABLE_THROTTLING, TRUE));

    REC_CHECK (MFCreateSinkWriterFromURL (path.c_str(), nullptr, attributes.Get(), &sinkWriter));

    // 映像: NV12 → H.264
    const auto videoOutput = createVideoType (MFVideoFormat_H264, video);
    REC_CHECK (videoOutput->SetUINT32 (MF_MT_AVG_BITRATE, video.bitrate));
    REC_CHECK (videoOutput->SetUINT32 (MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_High));
    REC_CHECK (sinkWriter->AddStream (videoOutput.Get(), &videoStream));
    REC_CHECK (sinkWriter->SetInputMediaType (videoStream, createVideoType (MFVideoFormat_NV12, video).Get(), nullptr));

    findVideoEncoder();

    ComPtr<ICodecAPI> codec;

    if (SUCCEEDED (sinkWriter->GetServiceForStream (videoStream, GUID_NULL, IID_PPV_ARGS (&codec))))
        trySetCodecValue (codec.Get(), CODECAPI_AVEncMPVGOPSize, video.frameRate * 2); // 2 秒ごとにキーフレーム

    if (audio != nullptr)
    {
        // 音声: 16bit PCM → AAC 192kbps
        const auto audioOutput = createAudioType (MFAudioFormat_AAC, *audio);
        REC_CHECK (audioOutput->SetUINT32 (MF_MT_AUDIO_AVG_BYTES_PER_SECOND, 24000));
        REC_CHECK (sinkWriter->AddStream (audioOutput.Get(), &audioStream));

        const auto audioInput = createAudioType (MFAudioFormat_PCM, *audio);
        const UINT32 blockAlign = audio->numChannels * static_cast<UINT32> (sizeof (int16_t));
        REC_CHECK (audioInput->SetUINT32 (MF_MT_AUDIO_BLOCK_ALIGNMENT, blockAlign));
        REC_CHECK (audioInput->SetUINT32 (MF_MT_AUDIO_AVG_BYTES_PER_SECOND, blockAlign * audio->sampleRate));
        REC_CHECK (sinkWriter->SetInputMediaType (audioStream, audioInput.Get(), nullptr));
    }

    REC_CHECK (sinkWriter->BeginWriting());

    thread = std::thread ([this] { run(); });
}

Mp4Writer::~Mp4Writer()
{
    finish();
}

void Mp4Writer::finish()
{
    {
        const std::lock_guard<std::mutex> sl (lock);
        finishRequested = true;
    }

    queueChanged.notify_all();

    if (thread.joinable())
        thread.join();

    sinkWriter.Reset();
}

void Mp4Writer::findVideoEncoder()
{
    ComPtr<IMFSinkWriterEx> sinkWriterEx;

    if (FAILED (sinkWriter.As (&sinkWriterEx)))
        return;

    GUID category {};
    ComPtr<IMFTransform> transform;

    for (DWORD i = 0; SUCCEEDED (sinkWriterEx->GetTransformForStream (videoStream, i, &category, &transform)); ++i)
    {
        if (category != MFT_CATEGORY_VIDEO_ENCODER)
            continue;

        ComPtr<IMFAttributes> attributes;

        if (SUCCEEDED (transform->GetAttributes (&attributes)))
        {
            WCHAR* name = nullptr;
            UINT32 length = 0;

            if (SUCCEEDED (attributes->GetAllocatedString (MFT_FRIENDLY_NAME_Attribute, &name, &length)))
            {
                videoEncoderName = name;
                CoTaskMemFree (name);
            }

            hardwareEncoder = SUCCEEDED (attributes->GetStringLength (MFT_ENUM_HARDWARE_URL_Attribute, &length));
        }

        if (videoEncoderName.empty())
            videoEncoderName = L"Microsoft H.264 software encoder";

        return;
    }
}

std::string Mp4Writer::getError() const
{
    const std::lock_guard<std::mutex> sl (lock);
    return error;
}

void Mp4Writer::setError (const std::string& message)
{
    const std::lock_guard<std::mutex> sl (lock);

    if (error.empty())
        error = message;

    queue.clear(); // 溜まっている NV12 サンプルをプールへ返す
}

void Mp4Writer::enqueue (DWORD streamIndex, ComPtr<IMFSample> sample)
{
    {
        const std::lock_guard<std::mutex> sl (lock);

        if (finishRequested || ! error.empty())
            return;

        queue.emplace_back (streamIndex, std::move (sample));
    }

    queueChanged.notify_one();
}

void Mp4Writer::run()
{
    const ScopedComInitialiser com;

    for (;;)
    {
        std::pair<DWORD, ComPtr<IMFSample>> item;

        {
            std::unique_lock<std::mutex> sl (lock);
            queueChanged.wait (sl, [this] { return finishRequested || ! queue.empty(); });

            if (queue.empty())
                break; // finish() が呼ばれ、キューも書き切った

            item = std::move (queue.front());
            queue.pop_front();
        }

        const HRESULT hr = sinkWriter->WriteSample (item.first, item.second.Get());

        if (FAILED (hr))
        {
            setError (HResultError (hr, "IMFSinkWriter::WriteSample").what());
            return;
        }
    }

    const HRESULT hr = sinkWriter->Finalize();

    if (FAILED (hr))
        setError (HResultError (hr, "IMFSinkWriter::Finalize").what());
}

} // namespace rec
