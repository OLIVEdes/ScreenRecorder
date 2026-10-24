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

#include <atomic>
#include <mutex>
#include <vector>

namespace rec
{
//==============================================================================
/** GPU 上の NV12 テクスチャを IMFSample (MFCreateDXGISurfaceBuffer) で包んだもののプール。

    サンプルは IMFTrackedSample なので、SinkWriter / エンコーダが最後の参照を手放した時点で
    Invoke() が呼ばれ、自動的にプールへ戻る。フレームごとのテクスチャ確保やコピーは発生しない。
    プールが空 = エンコーダが追いついていない、という意味になるので、そのまま流量制御にも使う。
*/
class NV12SamplePool final : public IMFAsyncCallback
{
public:
    static ComPtr<NV12SamplePool> create (ID3D11Device* device, UINT width, UINT height, int numSamples);

    /** 空いているサンプルとその NV12 テクスチャを取り出す。空きが無ければ false。 */
    bool tryAcquire (ComPtr<IMFSample>& sample, ComPtr<ID3D11Texture2D>& texture);

    // IUnknown
    STDMETHODIMP QueryInterface (REFIID iid, void** result) override;
    STDMETHODIMP_(ULONG) AddRef() override;
    STDMETHODIMP_(ULONG) Release() override;

    // IMFAsyncCallback（サンプルが解放されたときに呼ばれる）
    STDMETHODIMP GetParameters (DWORD*, DWORD*) override  { return E_NOTIMPL; }
    STDMETHODIMP Invoke (IMFAsyncResult* asyncResult) override;

private:
    NV12SamplePool() = default;
    ~NV12SamplePool() = default;

    std::atomic<ULONG> refCount { 1 };
    std::mutex lock;
    std::vector<ComPtr<IMFSample>> freeSamples;
};

} // namespace rec
