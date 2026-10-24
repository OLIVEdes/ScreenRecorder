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

#include "ScreenRecorder.h"

#include "DesktopDuplicator.h"
#include "LoopbackCapture.h"
#include "Mp4Writer.h"
#include "NV12SamplePool.h"

#include <avrt.h>

#include <atomic>
#include <mutex>
#include <thread>

#pragma comment (lib, "avrt.lib")
#pragma comment (lib, "dxgi.lib")
#pragma comment (lib, "mfplat.lib")

namespace rec
{
namespace
{
    // エンコーダ内部やキューに滞留できるフレーム数。使い切るとフレームを間引く
    constexpr int samplePoolSize = 10;

    ComPtr<ID3D11Texture2D> getTexture (IMFSample* sample)
    {
        ComPtr<IMFMediaBuffer> buffer;
        ComPtr<IMFDXGIBuffer> dxgiBuffer;
        ComPtr<ID3D11Texture2D> texture;
        REC_CHECK (sample->GetBufferByIndex (0, &buffer));
        REC_CHECK (buffer.As (&dxgiBuffer));
        REC_CHECK (dxgiBuffer->GetResource (IID_PPV_ARGS (&texture)));
        return texture;
    }

    void waitUntil (HANDLE timer, int64_t time)
    {
        const auto remaining = time - now100ns();

        if (remaining <= 0)
            return;

        LARGE_INTEGER dueTime;
        dueTime.QuadPart = -remaining; // 負の値は相対時間（100ns 単位）

        if (timer != nullptr && SetWaitableTimer (timer, &dueTime, 0, nullptr, nullptr, FALSE))
            WaitForSingleObject (timer, INFINITE);
        else
            Sleep (static_cast<DWORD> (remaining / 10'000));
    }
}

//==============================================================================
std::vector<MonitorInfo> getMonitors()
{
    std::vector<MonitorInfo> monitors;

    ComPtr<IDXGIFactory1> factory;

    if (FAILED (CreateDXGIFactory1 (IID_PPV_ARGS (&factory))))
        return monitors;

    ComPtr<IDXGIAdapter1> adapter;

    for (UINT adapterIndex = 0; SUCCEEDED (factory->EnumAdapters1 (adapterIndex, &adapter)); ++adapterIndex)
    {
        DXGI_ADAPTER_DESC1 adapterDesc {};
        adapter->GetDesc1 (&adapterDesc);

        ComPtr<IDXGIOutput> output;

        for (UINT outputIndex = 0; SUCCEEDED (adapter->EnumOutputs (outputIndex, &output)); ++outputIndex)
        {
            DXGI_OUTPUT_DESC desc {};

            if (FAILED (output->GetDesc (&desc)) || ! desc.AttachedToDesktop)
                continue;

            MONITORINFO info {};
            info.cbSize = sizeof (info);
            GetMonitorInfoW (desc.Monitor, &info);

            MonitorInfo monitor;
            monitor.name         = desc.DeviceName;
            monitor.adapterName  = adapterDesc.Description;
            monitor.adapterIndex = static_cast<int> (adapterIndex);
            monitor.outputIndex  = static_cast<int> (outputIndex);
            monitor.width        = desc.DesktopCoordinates.right - desc.DesktopCoordinates.left;
            monitor.height       = desc.DesktopCoordinates.bottom - desc.DesktopCoordinates.top;
            monitor.isPrimary    = (info.dwFlags & MONITORINFOF_PRIMARY) != 0;

            if (monitor.name.rfind (L"\\\\.\\", 0) == 0)
                monitor.name.erase (0, 4);

            monitors.push_back (std::move (monitor));
        }
    }

    return monitors;
}

//==============================================================================
struct ScreenRecorder::Impl
{
    Impl()
        : comResult (CoInitializeEx (nullptr, COINIT_APARTMENTTHREADED)),
          mfResult (MFStartup (MF_VERSION))
    {
    }

    ~Impl()
    {
        stop();

        if (SUCCEEDED (mfResult))   MFShutdown();
        if (SUCCEEDED (comResult))  CoUninitialize();
    }

    void start (const RecorderSettings& settings)
    {
        if (recording)
            return;

        throwIfFailed (mfResult, "MFStartup");

        stopRequested = false;
        framesWritten = 0;
        framesDropped = 0;
        frameRate = settings.frameRate;

        {
            const std::lock_guard<std::mutex> sl (errorLock);
            error.clear();
        }

        // 失敗時に、SinkWriter が作りかけたファイルだけを消すため
        const bool fileExisted = GetFileAttributesW (settings.outputFile.c_str()) != INVALID_FILE_ATTRIBUTES;

        try
        {
            duplicator = std::make_unique<DesktopDuplicator> (settings.adapterIndex, settings.outputIndex);
            auto* device = duplicator->getDevice();

            // SinkWriter（ハードウェアエンコーダ）とキャプチャで同じ D3D11 デバイスを共有する
            UINT resetToken = 0;
            REC_CHECK (MFCreateDXGIDeviceManager (&resetToken, &deviceManager));
            REC_CHECK (deviceManager->ResetDevice (device, resetToken));

            const auto width  = duplicator->getOutputWidth();
            const auto height = duplicator->getOutputHeight();
            pool = NV12SamplePool::create (device, width, height, samplePoolSize);

            if (settings.recordSystemAudio)
                audio = std::make_unique<LoopbackCapture>();

            const VideoStreamFormat videoFormat { width, height,
                                                  static_cast<UINT32> (settings.frameRate),
                                                  static_cast<UINT32> (settings.videoBitrate) };
            AudioStreamFormat audioFormat;

            if (audio != nullptr)
                audioFormat.sampleRate = audio->getSampleRate();

            writer = std::make_unique<Mp4Writer> (settings.outputFile, deviceManager.Get(), videoFormat,
                                                  audio != nullptr ? &audioFormat : nullptr);

            auto firstFrame = captureFirstFrame();

            startTime = now100ns();

            if (audio != nullptr)
                audio->start (startTime, [w = writer.get()] (ComPtr<IMFSample> sample) { w->writeAudio (std::move (sample)); });

            videoThread = std::thread ([this, frame = std::move (firstFrame)]() mutable { runVideo (std::move (frame)); });
            recording = true;
        }
        catch (...)
        {
            release();

            if (! fileExisted)
                DeleteFileW (settings.outputFile.c_str());

            throw;
        }
    }

    void stop()
    {
        if (! recording)
            return;

        stopRequested = true;

        if (videoThread.joinable())
            videoThread.join();

        if (audio != nullptr)
            audio->stop(); // これ以降、音声サンプルは届かない

        writer->finish(); // 残りをエンコードして MP4 を確定

        setError (getComponentErrors());
        release();
        recording = false;
    }

    std::string getError() const
    {
        {
            const std::lock_guard<std::mutex> sl (errorLock);

            if (! error.empty())
                return error;
        }

        return getComponentErrors();
    }

    RecorderStats getStats() const
    {
        RecorderStats stats;
        stats.framesWritten = framesWritten;
        stats.framesDropped = framesDropped;

        if (recording)
            stats.seconds = static_cast<double> (now100ns() - startTime) / 10'000'000.0;

        return stats;
    }

    //==============================================================================
    ComPtr<IMFSample> captureFirstFrame()
    {
        ComPtr<IMFSample> sample;
        ComPtr<ID3D11Texture2D> texture;

        if (! pool->tryAcquire (sample, texture))
            throw std::logic_error ("sample pool is empty");

        // 作りたての Desktop Duplication は最初の AcquireNextFrame で現在の画面を返すので、通常はすぐに取れる
        for (int attempt = 0; attempt < 20; ++attempt)
            if (duplicator->captureInto (texture.Get(), 50) == DesktopDuplicator::Result::newFrame)
                return sample;

        throw std::runtime_error ("Could not capture the screen (a secure desktop such as UAC may be active)");
    }

    void runVideo (ComPtr<IMFSample> latest)
    {
        const ScopedComInitialiser com;

        DWORD taskIndex = 0;
        const HANDLE task = AvSetMmThreadCharacteristicsW (L"Capture", &taskIndex);

        // Sleep() は 1ms 単位でぶれるので、高精度の待機可能タイマー（Windows 10 1803 以降）でフレーム境界まで待つ
        HANDLE timer = CreateWaitableTimerExW (nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);

        if (timer == nullptr)
            timer = CreateWaitableTimerExW (nullptr, nullptr, 0, TIMER_ALL_ACCESS);

        try
        {
            auto latestTexture = getTexture (latest.Get());
            bool latestWasWritten = false;

            for (int64_t frame = 0; ! stopRequested; ++frame)
            {
                waitUntil (timer, startTime + frameTime (frame));

                if (stopRequested)
                    break;

                // 処理が遅れてフレーム境界を過ぎていたら、その分は間引いて現在時刻に追いつく
                const auto currentFrame = (now100ns() - startTime) * frameRate / 10'000'000;

                if (currentFrame > frame)
                {
                    framesDropped += currentFrame - frame;
                    frame = currentFrame;
                }

                ComPtr<IMFSample> sample;
                ComPtr<ID3D11Texture2D> texture;
                const bool gotSample = pool->tryAcquire (sample, texture);

                if (gotSample && duplicator->captureInto (texture.Get(), 0) == DesktopDuplicator::Result::newFrame)
                {
                    latest = sample;
                    latestTexture = texture;
                    latestWasWritten = false;
                }

                ComPtr<IMFSample> output;

                if (! latestWasWritten)
                {
                    output = latest;
                }
                else if (gotSample)
                {
                    // 画面に変化が無い: 一定フレームレートを保つため直前のフレームを GPU 上で複製する
                    duplicator->copy (texture.Get(), latestTexture.Get());
                    output = sample;
                }

                if (output == nullptr)
                {
                    ++framesDropped; // プールが空 = エンコーダが追いついていない
                    continue;
                }

                REC_CHECK (output->SetSampleTime (frameTime (frame)));
                REC_CHECK (output->SetSampleDuration (frameTime (frame + 1) - frameTime (frame)));
                writer->writeVideo (std::move (output));

                latestWasWritten = true;
                ++framesWritten;
            }
        }
        catch (const std::exception& e)
        {
            setError (e.what());
        }

        if (timer != nullptr)
            CloseHandle (timer);

        if (task != nullptr)
            AvRevertMmThreadCharacteristics (task);
    }

    int64_t frameTime (int64_t frameIndex) const noexcept
    {
        return frameIndex * 10'000'000 / frameRate;
    }

    std::string getComponentErrors() const
    {
        if (writer != nullptr)
            if (auto e = writer->getError(); ! e.empty())
                return e;

        if (audio != nullptr)
            return audio->getError();

        return {};
    }

    void setError (const std::string& message)
    {
        const std::lock_guard<std::mutex> sl (errorLock);

        if (error.empty())
            error = message;
    }

    void release()
    {
        audio.reset();
        writer.reset();
        pool.Reset();
        deviceManager.Reset();
        duplicator.reset();
    }

    //==============================================================================
    const HRESULT comResult, mfResult;

    std::unique_ptr<DesktopDuplicator> duplicator;
    ComPtr<IMFDXGIDeviceManager> deviceManager;
    ComPtr<NV12SamplePool> pool;
    std::unique_ptr<LoopbackCapture> audio;
    std::unique_ptr<Mp4Writer> writer;

    std::thread videoThread;
    std::atomic<bool> stopRequested { false };
    bool recording = false;

    int64_t startTime = 0;
    int frameRate = 60;
    std::atomic<int64_t> framesWritten { 0 }, framesDropped { 0 };

    mutable std::mutex errorLock;
    std::string error;
};

//==============================================================================
ScreenRecorder::ScreenRecorder()  : impl (std::make_unique<Impl>()) {}
ScreenRecorder::~ScreenRecorder() = default;

void ScreenRecorder::start (const RecorderSettings& settings)  { impl->start (settings); }
void ScreenRecorder::stop()                                     { impl->stop(); }
bool ScreenRecorder::isRecording() const noexcept               { return impl->recording; }
RecorderStats ScreenRecorder::getStats() const                  { return impl->getStats(); }
std::string ScreenRecorder::getError() const                    { return impl->getError(); }

std::wstring ScreenRecorder::getVideoEncoderName() const
{
    return impl->writer != nullptr ? impl->writer->getVideoEncoderName() : std::wstring();
}

bool ScreenRecorder::isHardwareEncoder() const
{
    return impl->writer != nullptr && impl->writer->isHardwareEncoder();
}

} // namespace rec
