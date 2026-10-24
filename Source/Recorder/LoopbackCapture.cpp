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

#include "LoopbackCapture.h"

#include <avrt.h>
#include <mmreg.h>
#include <ksmedia.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <future>
#include <memory>

#pragma comment (lib, "avrt.lib")
#pragma comment (lib, "mfplat.lib")
#pragma comment (lib, "ole32.lib")

namespace rec
{
namespace
{
    constexpr REFERENCE_TIME bufferDuration = 2'000'000; // 200ms
    constexpr UINT32 preferredSampleRate = 48000;

    struct CoTaskMemDeleter
    {
        void operator() (void* p) const noexcept { CoTaskMemFree (p); }
    };

    WAVEFORMATEX makePcm16Format (UINT32 rate, WORD channels)
    {
        WAVEFORMATEX format {};
        format.wFormatTag      = WAVE_FORMAT_PCM;
        format.nChannels       = channels;
        format.nSamplesPerSec  = rate;
        format.wBitsPerSample  = 16;
        format.nBlockAlign     = static_cast<WORD> (channels * sizeof (int16_t));
        format.nAvgBytesPerSec = rate * format.nBlockAlign;
        return format;
    }

    bool hasSubFormat (const WAVEFORMATEX& format, WORD tag, const GUID& subFormat)
    {
        if (format.wFormatTag == tag)
            return true;

        return format.wFormatTag == WAVE_FORMAT_EXTENSIBLE
            && reinterpret_cast<const WAVEFORMATEXTENSIBLE&> (format).SubFormat == subFormat;
    }

    int16_t floatToInt16 (float sample) noexcept
    {
        return static_cast<int16_t> (std::lround (std::clamp (sample, -1.0f, 1.0f) * 32767.0f));
    }

    void activate (IMMDevice* device, ComPtr<IAudioClient>& client)
    {
        REC_CHECK (device->Activate (__uuidof (IAudioClient), CLSCTX_ALL, nullptr,
                                     reinterpret_cast<void**> (client.ReleaseAndGetAddressOf())));
    }
}

//==============================================================================
LoopbackCapture::LoopbackCapture()
{
    std::promise<void> initialised;
    auto initResult = initialised.get_future();

    thread = std::thread ([this, &initialised] { run (&initialised); });

    try
    {
        initResult.get();
    }
    catch (...)
    {
        stop();
        throw;
    }
}

LoopbackCapture::~LoopbackCapture()
{
    stop();
}

void LoopbackCapture::start (int64_t startTime, SampleCallback callback)
{
    // timelineStart を release で書くので、音声スレッドが acquire で読めば callback も確実に見える
    sampleCallback = std::move (callback);
    timelineStart.store (startTime, std::memory_order_release);
}

void LoopbackCapture::stop()
{
    stopRequested = true;

    if (thread.joinable())
        thread.join();
}

std::string LoopbackCapture::getError() const
{
    const std::lock_guard<std::mutex> sl (errorLock);
    return error;
}

void LoopbackCapture::setError (const std::string& message)
{
    const std::lock_guard<std::mutex> sl (errorLock);

    if (error.empty())
        error = "Audio: " + message;
}

//==============================================================================
void LoopbackCapture::run (std::promise<void>* initialised)
{
    const ScopedComInitialiser com;

    try
    {
        openDevice();
    }
    catch (...)
    {
        closeDevice();
        initialised->set_exception (std::current_exception());
        return;
    }

    initialised->set_value();

    DWORD taskIndex = 0;
    const HANDLE task = AvSetMmThreadCharacteristicsW (L"Audio", &taskIndex);
    int64_t nextReopenTime = 0;

    while (! stopRequested)
    {
        Sleep (5);

        try
        {
            if (captureClient == nullptr)
            {
                // 既定デバイスが無くなった後: 新しい既定デバイスが現れるまで 0.5 秒おきに開き直す
                const auto now = now100ns();

                if (now < nextReopenTime)
                    continue;

                nextReopenTime = now + 5'000'000;
                reopenDevice();

                if (captureClient == nullptr)
                    continue;
            }

            feedSilence();
            readPackets();
        }
        catch (const HResultError& e)
        {
            if (e.result != AUDCLNT_E_DEVICE_INVALIDATED)
            {
                setError (e.what());
                break;
            }

            closeDevice(); // ヘッドホンを抜いた・既定デバイスを切り替えた等
        }
        catch (const std::exception& e)
        {
            setError (e.what());
            break;
        }
    }

    closeDevice();

    if (task != nullptr)
        AvRevertMmThreadCharacteristics (task);
}

void LoopbackCapture::openDevice()
{
    ComPtr<IMMDeviceEnumerator> enumerator;
    REC_CHECK (CoCreateInstance (__uuidof (MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS (&enumerator)));

    ComPtr<IMMDevice> device;
    REC_CHECK (enumerator->GetDefaultAudioEndpoint (eRender, eConsole, &device));

    activate (device.Get(), captureAudioClient);

    WAVEFORMATEX* rawMixFormat = nullptr;
    REC_CHECK (captureAudioClient->GetMixFormat (&rawMixFormat));
    const std::unique_ptr<WAVEFORMATEX, CoTaskMemDeleter> mixFormat (rawMixFormat);

    // まず 48kHz / 16bit / ステレオを要求し、ダウンミックスとサンプルレート変換を WASAPI に任せる
    const auto wantedFormat = makePcm16Format (preferredSampleRate, static_cast<WORD> (numChannels));

    const HRESULT autoConvertResult = captureAudioClient->Initialize (AUDCLNT_SHAREMODE_SHARED,
                                                                      AUDCLNT_STREAMFLAGS_LOOPBACK
                                                                        | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM
                                                                        | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
                                                                      bufferDuration, 0, &wantedFormat, nullptr);
    if (SUCCEEDED (autoConvertResult))
    {
        sourceFormat   = SourceFormat::pcm16Stereo;
        sourceChannels = numChannels;
        sampleRate     = preferredSampleRate;
    }
    else
    {
        // 自動変換が使えない場合はミックスフォーマットのまま受け取り、自前で 16bit ステレオにする
        const auto& mix = *mixFormat;

        if (hasSubFormat (mix, WAVE_FORMAT_IEEE_FLOAT, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT) && mix.wBitsPerSample == 32)
            sourceFormat = SourceFormat::float32;
        else if (hasSubFormat (mix, WAVE_FORMAT_PCM, KSDATAFORMAT_SUBTYPE_PCM) && mix.wBitsPerSample == 16)
            sourceFormat = SourceFormat::int16;
        else
            throw std::runtime_error ("unsupported mix format of the default audio device");

        if (mix.nSamplesPerSec != 44100 && mix.nSamplesPerSec != 48000)
            throw std::runtime_error ("the AAC encoder needs 44.1 kHz or 48 kHz; change the device's sample rate");

        // Initialize は 1 つの IAudioClient につき 1 回しか呼べないので取り直す
        activate (device.Get(), captureAudioClient);
        REC_CHECK (captureAudioClient->Initialize (AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK,
                                                   bufferDuration, 0, mixFormat.get(), nullptr));
        sourceChannels = mix.nChannels;
        sampleRate     = mix.nSamplesPerSec;
    }

    REC_CHECK (captureAudioClient->GetService (IID_PPV_ARGS (&captureClient)));

    // ループバックは何も再生されていない間パケットが届かないので、同じデバイスに無音を流し続ける
    activate (device.Get(), renderAudioClient);
    REC_CHECK (renderAudioClient->Initialize (AUDCLNT_SHAREMODE_SHARED, 0, bufferDuration, 0, mixFormat.get(), nullptr));
    REC_CHECK (renderAudioClient->GetBufferSize (&renderBufferFrames));
    REC_CHECK (renderAudioClient->GetService (IID_PPV_ARGS (&renderClient)));

    feedSilence();
    REC_CHECK (renderAudioClient->Start());
    REC_CHECK (captureAudioClient->Start());
}

void LoopbackCapture::reopenDevice()
{
    const auto previousSampleRate = sampleRate;

    try
    {
        openDevice();
    }
    catch (const std::exception&)
    {
        closeDevice();
        return;
    }

    // MP4 の音声フォーマットは途中で変えられない（自動変換が使えれば常に 48kHz なので通常は一致する）
    if (sampleRate != previousSampleRate)
        throw std::runtime_error ("the new default audio device uses a different sample rate");
}

void LoopbackCapture::closeDevice()
{
    if (captureAudioClient != nullptr)  captureAudioClient->Stop();
    if (renderAudioClient != nullptr)   renderAudioClient->Stop();

    captureClient.Reset();
    renderClient.Reset();
    captureAudioClient.Reset();
    renderAudioClient.Reset();
}

//==============================================================================
void LoopbackCapture::feedSilence()
{
    UINT32 padding = 0;
    REC_CHECK (renderAudioClient->GetCurrentPadding (&padding));

    if (padding >= renderBufferFrames)
        return;

    const UINT32 numFrames = renderBufferFrames - padding;
    BYTE* data = nullptr;
    REC_CHECK (renderClient->GetBuffer (numFrames, &data));
    REC_CHECK (renderClient->ReleaseBuffer (numFrames, AUDCLNT_BUFFERFLAGS_SILENT));
}

void LoopbackCapture::readPackets()
{
    for (;;)
    {
        UINT32 packetFrames = 0;
        REC_CHECK (captureClient->GetNextPacketSize (&packetFrames));

        if (packetFrames == 0)
            return;

        BYTE* data = nullptr;
        UINT32 numFrames = 0;
        DWORD flags = 0;
        UINT64 packetTime = 0;
        REC_CHECK (captureClient->GetBuffer (&data, &numFrames, &flags, nullptr, &packetTime));

        handlePacket (data, numFrames, flags, packetTime);

        REC_CHECK (captureClient->ReleaseBuffer (numFrames));
    }
}

void LoopbackCapture::handlePacket (const BYTE* data, UINT32 numFrames, DWORD flags, UINT64 packetTime)
{
    const auto startTime = timelineStart.load (std::memory_order_acquire);

    if (startTime < 0)
        return; // 録画開始前

    UINT32 framesToSkip = 0;

    if ((flags & AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR) == 0)
    {
        // このパケットがタイムライン上で置かれるべき位置と、実際に書き出した位置とのずれ。
        // 先頭は厳密に合わせ、以降はジッタを無視して 50ms 以上ずれたときだけ補正する。
        const auto expectedPosition = (static_cast<int64_t> (packetTime) - startTime) * sampleRate / 10'000'000;
        const auto drift = expectedPosition - framesWritten;
        const auto tolerance = framesWritten == 0 ? 0 : static_cast<int64_t> (sampleRate / 20);

        if (drift > tolerance)
            writeSilence (drift); // パケットが来なかった区間を無音で埋める
        else if (drift < -tolerance)
            framesToSkip = static_cast<UINT32> (std::min<int64_t> (-drift, numFrames)); // 開始前・重複部分を捨てる
    }

    if (framesToSkip >= numFrames)
        return;

    const UINT32 count = numFrames - framesToSkip;
    conversionBuffer.resize (static_cast<size_t> (count) * numChannels);
    auto* out = conversionBuffer.data();

    if ((flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0)
    {
        std::fill (conversionBuffer.begin(), conversionBuffer.end(), int16_t {});
    }
    else if (sourceFormat == SourceFormat::pcm16Stereo)
    {
        std::memcpy (out, data + static_cast<size_t> (framesToSkip) * numChannels * sizeof (int16_t),
                     conversionBuffer.size() * sizeof (int16_t));
    }
    else if (sourceFormat == SourceFormat::float32)
    {
        // 3ch 以上は先頭 2ch（L/R）だけを使う簡易ダウンミックス
        const auto* in = reinterpret_cast<const float*> (data) + static_cast<size_t> (framesToSkip) * sourceChannels;

        for (UINT32 i = 0; i < count; ++i, in += sourceChannels)
        {
            out[i * 2]     = floatToInt16 (in[0]);
            out[i * 2 + 1] = floatToInt16 (in[sourceChannels > 1 ? 1 : 0]);
        }
    }
    else
    {
        const auto* in = reinterpret_cast<const int16_t*> (data) + static_cast<size_t> (framesToSkip) * sourceChannels;

        for (UINT32 i = 0; i < count; ++i, in += sourceChannels)
        {
            out[i * 2]     = in[0];
            out[i * 2 + 1] = in[sourceChannels > 1 ? 1 : 0];
        }
    }

    writeFrames (out, count);
}

void LoopbackCapture::writeSilence (int64_t numFrames)
{
    // 長い隙間でも巨大なバッファを作らないよう 100ms ずつ書く
    const int64_t chunk = sampleRate / 10;

    for (; numFrames > 0; numFrames -= chunk)
        writeFrames (nullptr, static_cast<UINT32> (std::min (numFrames, chunk)));
}

void LoopbackCapture::writeFrames (const int16_t* frames, UINT32 numFrames)
{
    const auto numBytes = static_cast<DWORD> (numFrames * numChannels * sizeof (int16_t));

    ComPtr<IMFMediaBuffer> buffer;
    REC_CHECK (MFCreateMemoryBuffer (numBytes, &buffer));

    BYTE* destination = nullptr;
    REC_CHECK (buffer->Lock (&destination, nullptr, nullptr));

    if (frames != nullptr)
        std::memcpy (destination, frames, numBytes);
    else
        std::memset (destination, 0, numBytes);

    buffer->Unlock();
    REC_CHECK (buffer->SetCurrentLength (numBytes));

    ComPtr<IMFSample> sample;
    REC_CHECK (MFCreateSample (&sample));
    REC_CHECK (sample->AddBuffer (buffer.Get()));
    REC_CHECK (sample->SetSampleTime (framesWritten * 10'000'000 / sampleRate));
    REC_CHECK (sample->SetSampleDuration (static_cast<int64_t> (numFrames) * 10'000'000 / sampleRate));

    framesWritten += numFrames;
    sampleCallback (std::move (sample));
}

} // namespace rec
