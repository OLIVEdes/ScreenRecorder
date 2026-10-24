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

#include "NV12SamplePool.h"

#pragma comment (lib, "mfplat.lib")

namespace rec
{
ComPtr<NV12SamplePool> NV12SamplePool::create (ID3D11Device* device, UINT width, UINT height, int numSamples)
{
    ComPtr<NV12SamplePool> pool;
    pool.Attach (new NV12SamplePool());

    D3D11_TEXTURE2D_DESC desc {};
    desc.Width            = width;
    desc.Height           = height;
    desc.MipLevels        = 1;
    desc.ArraySize        = 1;
    desc.Format           = DXGI_FORMAT_NV12;
    desc.SampleDesc.Count = 1;
    desc.Usage            = D3D11_USAGE_DEFAULT;
    desc.BindFlags        = D3D11_BIND_RENDER_TARGET; // Video Processor の出力先にするため

    for (int i = 0; i < numSamples; ++i)
    {
        ComPtr<ID3D11Texture2D> texture;
        REC_CHECK (device->CreateTexture2D (&desc, nullptr, &texture));

        ComPtr<IMFMediaBuffer> buffer;
        REC_CHECK (MFCreateDXGISurfaceBuffer (__uuidof (ID3D11Texture2D), texture.Get(), 0, FALSE, &buffer));

        // ソフトウェアエンコーダにフォールバックした場合、MF が Lock() して読むので長さを設定しておく
        ComPtr<IMF2DBuffer> buffer2D;
        DWORD length = 0;

        if (SUCCEEDED (buffer.As (&buffer2D)) && SUCCEEDED (buffer2D->GetContiguousLength (&length)))
            REC_CHECK (buffer->SetCurrentLength (length));

        ComPtr<IMFTrackedSample> tracked;
        REC_CHECK (MFCreateTrackedSample (&tracked));

        ComPtr<IMFSample> sample;
        REC_CHECK (tracked.As (&sample));
        REC_CHECK (sample->AddBuffer (buffer.Get()));

        pool->freeSamples.push_back (std::move (sample));
    }

    return pool;
}

bool NV12SamplePool::tryAcquire (ComPtr<IMFSample>& sample, ComPtr<ID3D11Texture2D>& texture)
{
    {
        const std::lock_guard<std::mutex> sl (lock);

        if (freeSamples.empty())
            return false;

        sample = std::move (freeSamples.back());
        freeSamples.pop_back();
    }

    // 前回の使用時に付いた属性を消してから、返却通知を再登録する（SetAllocator は 1 回の解放ごとに必要）
    REC_CHECK (sample->DeleteAllItems());

    ComPtr<IMFTrackedSample> tracked;
    REC_CHECK (sample.As (&tracked));
    REC_CHECK (tracked->SetAllocator (this, nullptr));

    ComPtr<IMFMediaBuffer> buffer;
    ComPtr<IMFDXGIBuffer> dxgiBuffer;
    REC_CHECK (sample->GetBufferByIndex (0, &buffer));
    REC_CHECK (buffer.As (&dxgiBuffer));
    REC_CHECK (dxgiBuffer->GetResource (IID_PPV_ARGS (&texture)));
    return true;
}

STDMETHODIMP NV12SamplePool::Invoke (IMFAsyncResult* asyncResult)
{
    ComPtr<IUnknown> object;
    ComPtr<IMFSample> sample;

    if (SUCCEEDED (asyncResult->GetObject (&object)) && SUCCEEDED (object.As (&sample)))
    {
        const std::lock_guard<std::mutex> sl (lock);
        freeSamples.push_back (std::move (sample));
    }

    return S_OK;
}

STDMETHODIMP NV12SamplePool::QueryInterface (REFIID iid, void** result)
{
    if (result == nullptr)
        return E_POINTER;

    if (iid == __uuidof (IUnknown) || iid == __uuidof (IMFAsyncCallback))
    {
        *result = static_cast<IMFAsyncCallback*> (this);
        AddRef();
        return S_OK;
    }

    *result = nullptr;
    return E_NOINTERFACE;
}

STDMETHODIMP_(ULONG) NV12SamplePool::AddRef()
{
    return ++refCount;
}

STDMETHODIMP_(ULONG) NV12SamplePool::Release()
{
    const auto count = --refCount;

    if (count == 0)
        delete this;

    return count;
}

} // namespace rec
