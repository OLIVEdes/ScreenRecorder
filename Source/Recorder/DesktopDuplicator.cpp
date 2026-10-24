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

#include "DesktopDuplicator.h"

#include <d3d10.h> // ID3D10Multithread

#pragma comment (lib, "d3d11.lib")
#pragma comment (lib, "dxgi.lib")

namespace rec
{
DesktopDuplicator::DesktopDuplicator (int adapterIndex, int outputIndex)
{
    ComPtr<IDXGIFactory1> factory;
    REC_CHECK (CreateDXGIFactory1 (IID_PPV_ARGS (&factory)));

    ComPtr<IDXGIAdapter1> adapter;
    REC_CHECK (factory->EnumAdapters1 (static_cast<UINT> (adapterIndex), &adapter));

    ComPtr<IDXGIOutput> baseOutput;
    REC_CHECK (adapter->EnumOutputs (static_cast<UINT> (outputIndex), &baseOutput));
    REC_CHECK (baseOutput.As (&output));

    // Desktop Duplication は「その出力が接続されているアダプタ」上のデバイスでしか作れない
    const D3D_FEATURE_LEVEL featureLevels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };

    REC_CHECK (D3D11CreateDevice (adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                                  D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT,
                                  featureLevels, ARRAYSIZE (featureLevels), D3D11_SDK_VERSION,
                                  &device, nullptr, &context));

    // キャプチャスレッドと MF のエンコーダが同じデバイス（イミディエイトコンテキスト）を共有するため必須
    ComPtr<ID3D10Multithread> multithread;
    REC_CHECK (device.As (&multithread));
    multithread->SetMultithreadProtected (TRUE);

    REC_CHECK (device.As (&videoDevice));
    REC_CHECK (context.As (&videoContext));

    DXGI_OUTPUT_DESC outputDesc;
    REC_CHECK (output->GetDesc (&outputDesc));

    // NV12 / H.264 は偶数サイズが必要
    const auto& bounds = outputDesc.DesktopCoordinates;
    outputWidth  = static_cast<UINT> (bounds.right - bounds.left) & ~1u;
    outputHeight = static_cast<UINT> (bounds.bottom - bounds.top) & ~1u;

    const HRESULT hr = createDuplication();

    if (hr == DXGI_ERROR_UNSUPPORTED)
        throw std::runtime_error ("Desktop Duplication is not supported on this display. On hybrid-GPU laptops, "
                                  "run the app on the GPU that drives this display (Settings > System > Display > Graphics).");

    throwIfFailed (hr, "IDXGIOutput1::DuplicateOutput");
}

HRESULT DesktopDuplicator::createDuplication()
{
    duplication.Reset();

    if (intermediateTexture == nullptr)
    {
        cachedDesktopTexture.Reset();
        cachedInputView.Reset();
    }

    ComPtr<IDXGIOutputDuplication> newDuplication;
    const HRESULT hr = output->DuplicateOutput (device.Get(), &newDuplication);

    if (FAILED (hr))
        return hr;

    DXGI_OUTDUPL_DESC desc;
    newDuplication->GetDesc (&desc);

    duplication = std::move (newDuplication);
    isFreshDuplication = true;

    if (desc.Rotation != rotation)
    {
        rotation = desc.Rotation;
        applyRotation();
    }

    return S_OK;
}

DesktopDuplicator::Result DesktopDuplicator::captureInto (ID3D11Texture2D* destination, UINT timeoutMs)
{
    // UAC のセキュアデスクトップ表示中などは DuplicateOutput 自体が失敗するので、取れるまで毎回再試行する
    if (duplication == nullptr && FAILED (createDuplication()))
        return Result::unchanged;

    DXGI_OUTDUPL_FRAME_INFO frameInfo {};
    ComPtr<IDXGIResource> resource;
    const HRESULT hr = duplication->AcquireNextFrame (timeoutMs, &frameInfo, &resource);

    if (hr == DXGI_ERROR_WAIT_TIMEOUT)
        return Result::unchanged;

    if (hr == DXGI_ERROR_ACCESS_LOST) // 解像度変更・デスクトップ切り替え・フルスクリーン排他など
    {
        duplication.Reset();
        return Result::unchanged;
    }

    throwIfFailed (hr, "IDXGIOutputDuplication::AcquireNextFrame");

    // ReleaseFrame し忘れると次の AcquireNextFrame が失敗するので、スコープを抜けるときに必ず呼ぶ
    struct FrameReleaser
    {
        IDXGIOutputDuplication* owner;
        ~FrameReleaser() { owner->ReleaseFrame(); }
    } releaser { duplication.Get() };

    // LastPresentTime == 0 はマウスポインタだけが動いたケース（画像は前回と同じ）
    if (frameInfo.LastPresentTime.QuadPart == 0 && ! isFreshDuplication)
        return Result::unchanged;

    ComPtr<ID3D11Texture2D> desktop;
    REC_CHECK (resource.As (&desktop));

    convert (desktop.Get(), destination);
    isFreshDuplication = false;
    return Result::newFrame;
}

void DesktopDuplicator::copy (ID3D11Texture2D* destination, ID3D11Texture2D* source)
{
    context->CopyResource (destination, source);
}

//==============================================================================
void DesktopDuplicator::convert (ID3D11Texture2D* desktop, ID3D11Texture2D* destination)
{
    D3D11_TEXTURE2D_DESC desc;
    desktop->GetDesc (&desc);

    if (videoProcessor == nullptr || desc.Width != inputWidth || desc.Height != inputHeight)
        createVideoProcessor (desc.Width, desc.Height);

    D3D11_VIDEO_PROCESSOR_STREAM stream {};
    stream.Enable        = TRUE;
    stream.pInputSurface = getInputView (desktop);

    REC_CHECK (videoContext->VideoProcessorBlt (videoProcessor.Get(), getOutputView (destination), 0, 1, &stream));
}

void DesktopDuplicator::createVideoProcessor (UINT newInputWidth, UINT newInputHeight)
{
    // ビューはエニュメレータに紐づくので、作り直すときはすべて破棄する
    videoProcessor.Reset();
    processorEnumerator.Reset();
    cachedDesktopTexture.Reset();
    cachedInputView.Reset();
    intermediateTexture.Reset();
    outputViews.clear();

    D3D11_VIDEO_PROCESSOR_CONTENT_DESC contentDesc {};
    contentDesc.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
    contentDesc.InputFrameRate   = { 60, 1 };
    contentDesc.InputWidth       = newInputWidth;
    contentDesc.InputHeight      = newInputHeight;
    contentDesc.OutputFrameRate  = { 60, 1 };
    contentDesc.OutputWidth      = outputWidth;
    contentDesc.OutputHeight     = outputHeight;
    contentDesc.Usage            = D3D11_VIDEO_USAGE_OPTIMAL_SPEED;

    REC_CHECK (videoDevice->CreateVideoProcessorEnumerator (&contentDesc, &processorEnumerator));

    UINT support = 0;
    REC_CHECK (processorEnumerator->CheckVideoProcessorFormat (DXGI_FORMAT_B8G8R8A8_UNORM, &support));

    if ((support & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_INPUT) == 0)
        throw std::runtime_error ("The GPU video processor does not accept BGRA input");

    REC_CHECK (processorEnumerator->CheckVideoProcessorFormat (DXGI_FORMAT_NV12, &support));

    if ((support & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_OUTPUT) == 0)
        throw std::runtime_error ("The GPU video processor cannot output NV12");

    REC_CHECK (videoDevice->CreateVideoProcessor (processorEnumerator.Get(), 0, &videoProcessor));
    auto* processor = videoProcessor.Get();

    // 入力は PC の RGB（フルレンジ）、出力は一般的な HD 動画と同じ BT.709 リミテッドレンジ
    D3D11_VIDEO_PROCESSOR_COLOR_SPACE inputColourSpace {};
    inputColourSpace.RGB_Range = 0;

    D3D11_VIDEO_PROCESSOR_COLOR_SPACE outputColourSpace {};
    outputColourSpace.YCbCr_Matrix  = 1;
    outputColourSpace.Nominal_Range = D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_16_235;

    videoContext->VideoProcessorSetStreamColorSpace (processor, 0, &inputColourSpace);
    videoContext->VideoProcessorSetOutputColorSpace (processor, &outputColourSpace);
    videoContext->VideoProcessorSetStreamFrameFormat (processor, 0, D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
    videoContext->VideoProcessorSetStreamAutoProcessingMode (processor, 0, FALSE);

    const RECT sourceRect { 0, 0, static_cast<LONG> (newInputWidth), static_cast<LONG> (newInputHeight) };
    const RECT targetRect { 0, 0, static_cast<LONG> (outputWidth),   static_cast<LONG> (outputHeight) };

    videoContext->VideoProcessorSetStreamSourceRect (processor, 0, TRUE, &sourceRect);
    videoContext->VideoProcessorSetStreamDestRect (processor, 0, TRUE, &targetRect);
    videoContext->VideoProcessorSetOutputTargetRect (processor, TRUE, &targetRect);

    inputWidth  = newInputWidth;
    inputHeight = newInputHeight;
    applyRotation();
}

void DesktopDuplicator::applyRotation()
{
    if (videoProcessor == nullptr)
        return;

    // 回転させたモニターでは、DDA の画像はパネル本来の向きのまま届くので Video Processor で回して戻す
    auto processorRotation = D3D11_VIDEO_PROCESSOR_ROTATION_IDENTITY;

    switch (rotation)
    {
        case DXGI_MODE_ROTATION_ROTATE90:   processorRotation = D3D11_VIDEO_PROCESSOR_ROTATION_90;  break;
        case DXGI_MODE_ROTATION_ROTATE180:  processorRotation = D3D11_VIDEO_PROCESSOR_ROTATION_180; break;
        case DXGI_MODE_ROTATION_ROTATE270:  processorRotation = D3D11_VIDEO_PROCESSOR_ROTATION_270; break;
        default:                            break;
    }

    videoContext->VideoProcessorSetStreamRotation (videoProcessor.Get(), 0,
                                                   processorRotation != D3D11_VIDEO_PROCESSOR_ROTATION_IDENTITY,
                                                   processorRotation);
}

ID3D11VideoProcessorInputView* DesktopDuplicator::getInputView (ID3D11Texture2D* desktop)
{
    D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC viewDesc {};
    viewDesc.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;

    if (intermediateTexture == nullptr && desktop != cachedDesktopTexture.Get())
    {
        cachedInputView.Reset();
        cachedDesktopTexture.Reset();

        if (SUCCEEDED (videoDevice->CreateVideoProcessorInputView (desktop, processorEnumerator.Get(),
                                                                   &viewDesc, &cachedInputView)))
        {
            cachedDesktopTexture = desktop;
        }
        else
        {
            // 一部のドライバは DDA のテクスチャに直接入力ビューを作れないので、自前のテクスチャを中継する
            D3D11_TEXTURE2D_DESC desc;
            desktop->GetDesc (&desc);
            desc.Usage          = D3D11_USAGE_DEFAULT;
            desc.BindFlags      = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
            desc.CPUAccessFlags = 0;
            desc.MiscFlags      = 0;

            ComPtr<ID3D11Texture2D> texture;
            ComPtr<ID3D11VideoProcessorInputView> view;
            REC_CHECK (device->CreateTexture2D (&desc, nullptr, &texture));
            REC_CHECK (videoDevice->CreateVideoProcessorInputView (texture.Get(), processorEnumerator.Get(),
                                                                   &viewDesc, &view));
            intermediateTexture = std::move (texture);
            cachedInputView = std::move (view);
        }
    }

    if (intermediateTexture != nullptr)
        context->CopyResource (intermediateTexture.Get(), desktop);

    return cachedInputView.Get();
}

ID3D11VideoProcessorOutputView* DesktopDuplicator::getOutputView (ID3D11Texture2D* destination)
{
    auto& view = outputViews[destination];

    if (view == nullptr)
    {
        D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC viewDesc {};
        viewDesc.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;

        REC_CHECK (videoDevice->CreateVideoProcessorOutputView (destination, processorEnumerator.Get(),
                                                                &viewDesc, &view));
    }

    return view.Get();
}

} // namespace rec
