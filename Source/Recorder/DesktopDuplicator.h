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

#include <unordered_map>

namespace rec
{
//==============================================================================
/** DXGI Desktop Duplication で取得したデスクトップ画像を、
    D3D11 Video Processor で GPU 上のまま NV12 テクスチャへ変換する。

    - マウスカーソルは Desktop Duplication の画像には含まれない（形状は別 API で取得する仕組み）
      ため、合成しなければそのまま「カーソル無し」になる。
    - 解像度変更・UAC 等で複製が失われた場合は自動で作り直す。出力サイズは固定のまま
      Video Processor がスケーリングするので、録画途中でも MP4 の解像度は変わらない。
*/
class DesktopDuplicator
{
public:
    DesktopDuplicator (int adapterIndex, int outputIndex);

    ID3D11Device* getDevice() const noexcept    { return device.Get(); }
    UINT getOutputWidth() const noexcept        { return outputWidth; }
    UINT getOutputHeight() const noexcept       { return outputHeight; }

    enum class Result { newFrame, unchanged };

    /** 画面に更新があれば NV12 に変換して destination へ書き込み newFrame を返す。
        更新が無い・一時的に取得できない場合は unchanged（destination は変更しない）。
    */
    Result captureInto (ID3D11Texture2D* destination, UINT timeoutMs);

    /** 変化の無いフレームを複製するための GPU 上のコピー */
    void copy (ID3D11Texture2D* destination, ID3D11Texture2D* source);

private:
    HRESULT createDuplication();
    void createVideoProcessor (UINT inputWidth, UINT inputHeight);
    void applyRotation();
    void convert (ID3D11Texture2D* desktop, ID3D11Texture2D* destination);
    ID3D11VideoProcessorInputView* getInputView (ID3D11Texture2D* desktop);
    ID3D11VideoProcessorOutputView* getOutputView (ID3D11Texture2D* destination);

    ComPtr<IDXGIOutput1> output;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11VideoDevice> videoDevice;
    ComPtr<ID3D11VideoContext> videoContext;

    ComPtr<IDXGIOutputDuplication> duplication;
    bool isFreshDuplication = false;
    DXGI_MODE_ROTATION rotation = DXGI_MODE_ROTATION_IDENTITY;

    ComPtr<ID3D11VideoProcessorEnumerator> processorEnumerator;
    ComPtr<ID3D11VideoProcessor> videoProcessor;
    UINT inputWidth = 0, inputHeight = 0;
    UINT outputWidth = 0, outputHeight = 0;

    // DDA は通常毎回同じテクスチャを返すので入力ビューはキャッシュする
    ComPtr<ID3D11Texture2D> cachedDesktopTexture;
    ComPtr<ID3D11VideoProcessorInputView> cachedInputView;

    // DDA のテクスチャに直接入力ビューを作れないドライバ向けの中継テクスチャ
    ComPtr<ID3D11Texture2D> intermediateTexture;

    std::unordered_map<ID3D11Texture2D*, ComPtr<ID3D11VideoProcessorOutputView>> outputViews;
};

} // namespace rec
