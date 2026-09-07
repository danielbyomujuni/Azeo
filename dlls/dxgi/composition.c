/*
 * Composition swapchain wrapper
 *
 * Copyright 2026 Daniel Byomujuni
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#include "dxgi_private.h"
#include "d3d11.h"

WINE_DEFAULT_DEBUG_CHANNEL(dxgi);

/* Private handshake with dcomp's present pump: QI for this IID returns a
 * LONG* that is non-zero while the back buffer holds presented content.
 * ResizeBuffers invalidates it; Present validates it.  This lets the pump
 * avoid blitting uninitialised buffers (white flicker during resizes). */
static const GUID IID_wine_composition_content_valid =
        {0x9f002c3a, 0x5c2d, 0x4f42, {0x91, 0x64, 0x7c, 0x2e, 0xd1, 0x8e, 0x1d, 0xab}};

/* QI for this IID returns the ID3D11Texture2D holding the most recently
 * presented frame (referenced), or fails if nothing was presented yet.
 * GetBuffer(0) cannot be used for that: in the flip model it addresses the
 * buffer the application will render NEXT, so for content that presents
 * rarely (a static dialog) it reads back an undrawn, white buffer. */
static const GUID IID_wine_composition_front_buffer =
        {0x1b8aa5c1, 0x33f1, 0x4d6a, {0xb0, 0x62, 0x50, 0x0f, 0x8a, 0x9e, 0x44, 0x21}};

struct composition_swapchain
{
    IDXGISwapChain4 IDXGISwapChain4_iface;
    LONG refcount;
    IDXGISwapChain4 *inner;
    HWND window;
    LONG content_valid;           /* handshake: must stay adjacent to present_count */
    LONG present_count;

    CRITICAL_SECTION cs;
    ID3D11Texture2D *front_copy;  /* last presented frame; guarded by cs */
};

static struct composition_swapchain *impl_from_IDXGISwapChain4(IDXGISwapChain4 *iface)
{
    return CONTAINING_RECORD(iface, struct composition_swapchain, IDXGISwapChain4_iface);
}

static HRESULT STDMETHODCALLTYPE composition_swapchain_QueryInterface(IDXGISwapChain4 *iface,
        REFIID riid, void **object)
{
    struct composition_swapchain *swapchain = impl_from_IDXGISwapChain4(iface);

    TRACE("iface %p, riid %s, object %p.\n", iface, debugstr_guid(riid), object);

    if (IsEqualGUID(riid, &IID_wine_composition_content_valid))
    {
        /* not a COM object; no reference is added.  Points at
         * {LONG content_valid; LONG present_count;} */
        *object = &swapchain->content_valid;
        return S_OK;
    }

    if (IsEqualGUID(riid, &IID_wine_composition_front_buffer))
    {
        EnterCriticalSection(&swapchain->cs);
        if (swapchain->front_copy)
        {
            ID3D11Texture2D_AddRef(swapchain->front_copy);
            *object = swapchain->front_copy;
            LeaveCriticalSection(&swapchain->cs);
            return S_OK;
        }
        LeaveCriticalSection(&swapchain->cs);
        *object = NULL;
        return E_FAIL;
    }

    if (IsEqualGUID(riid, &IID_IUnknown)
            || IsEqualGUID(riid, &IID_IDXGIObject)
            || IsEqualGUID(riid, &IID_IDXGIDeviceSubObject)
            || IsEqualGUID(riid, &IID_IDXGISwapChain)
            || IsEqualGUID(riid, &IID_IDXGISwapChain1)
            || IsEqualGUID(riid, &IID_IDXGISwapChain2)
            || IsEqualGUID(riid, &IID_IDXGISwapChain3)
            || IsEqualGUID(riid, &IID_IDXGISwapChain4))
    {
        IDXGISwapChain4_AddRef(iface);
        *object = iface;
        return S_OK;
    }

    WARN("Unsupported interface %s.\n", debugstr_guid(riid));
    *object = NULL;
    return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE composition_swapchain_AddRef(IDXGISwapChain4 *iface)
{
    struct composition_swapchain *swapchain = impl_from_IDXGISwapChain4(iface);
    return InterlockedIncrement(&swapchain->refcount);
}

static ULONG STDMETHODCALLTYPE composition_swapchain_Release(IDXGISwapChain4 *iface)
{
    struct composition_swapchain *swapchain = impl_from_IDXGISwapChain4(iface);
    ULONG refcount = InterlockedDecrement(&swapchain->refcount);

    if (!refcount)
    {
        if (swapchain->front_copy) ID3D11Texture2D_Release(swapchain->front_copy);
        swapchain->cs.DebugInfo->Spare[0] = 0;
        DeleteCriticalSection(&swapchain->cs);
        IDXGISwapChain4_Release(swapchain->inner);
        /* the backing window belongs to whichever thread created it; WM_CLOSE
         * lets that thread destroy it */
        if (swapchain->window) PostMessageW(swapchain->window, WM_CLOSE, 0, 0);
        free(swapchain);
    }
    return refcount;
}

/* Capture the frame being presented (buffer 0 still holds it at Present
 * time) into front_copy for dcomp's present pump. */
static void composition_swapchain_snapshot(struct composition_swapchain *swapchain)
{
    ID3D11DeviceContext *context = NULL;
    ID3D11Texture2D *back = NULL;
    ID3D11Device *device = NULL;
    D3D11_TEXTURE2D_DESC desc;

    if (FAILED(IDXGISwapChain4_GetBuffer(swapchain->inner, 0, &IID_ID3D11Texture2D, (void **)&back)))
        return;

    ID3D11Texture2D_GetDesc(back, &desc);
    ID3D11Texture2D_GetDevice(back, &device);
    ID3D11Device_GetImmediateContext(device, &context);

    EnterCriticalSection(&swapchain->cs);
    if (swapchain->front_copy)
    {
        D3D11_TEXTURE2D_DESC old_desc;
        ID3D11Texture2D_GetDesc(swapchain->front_copy, &old_desc);
        if (old_desc.Width != desc.Width || old_desc.Height != desc.Height
                || old_desc.Format != desc.Format)
        {
            ID3D11Texture2D_Release(swapchain->front_copy);
            swapchain->front_copy = NULL;
        }
    }
    if (!swapchain->front_copy)
    {
        D3D11_TEXTURE2D_DESC copy_desc = desc;
        copy_desc.Usage = D3D11_USAGE_DEFAULT;
        copy_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        copy_desc.CPUAccessFlags = 0;
        copy_desc.MiscFlags = 0;
        copy_desc.MipLevels = 1;
        copy_desc.ArraySize = 1;
        if (FAILED(ID3D11Device_CreateTexture2D(device, &copy_desc, NULL, &swapchain->front_copy)))
            swapchain->front_copy = NULL;
    }
    if (swapchain->front_copy)
        ID3D11DeviceContext_CopySubresourceRegion(context, (ID3D11Resource *)swapchain->front_copy,
                0, 0, 0, 0, (ID3D11Resource *)back, 0, NULL);
    LeaveCriticalSection(&swapchain->cs);

    ID3D11DeviceContext_Release(context);
    ID3D11Device_Release(device);
    ID3D11Texture2D_Release(back);
}

static HRESULT STDMETHODCALLTYPE composition_swapchain_Present(IDXGISwapChain4 *iface,
        UINT sync_interval, UINT flags)
{
    struct composition_swapchain *swapchain = impl_from_IDXGISwapChain4(iface);
    HRESULT hr;

    if (!(flags & DXGI_PRESENT_TEST))
        composition_swapchain_snapshot(swapchain);
    hr = IDXGISwapChain4_Present(swapchain->inner, sync_interval, flags);

    if (SUCCEEDED(hr) && !(flags & DXGI_PRESENT_TEST))
    {
        InterlockedExchange(&swapchain->content_valid, 1);
        InterlockedIncrement(&swapchain->present_count);
    }
    return hr;
}

static HRESULT STDMETHODCALLTYPE composition_swapchain_Present1(IDXGISwapChain4 *iface,
        UINT sync_interval, UINT flags, const DXGI_PRESENT_PARAMETERS *parameters)
{
    struct composition_swapchain *swapchain = impl_from_IDXGISwapChain4(iface);
    HRESULT hr;

    if (!(flags & DXGI_PRESENT_TEST))
        composition_swapchain_snapshot(swapchain);
    hr = IDXGISwapChain4_Present1(swapchain->inner, sync_interval, flags, parameters);

    if (SUCCEEDED(hr) && !(flags & DXGI_PRESENT_TEST))
    {
        InterlockedExchange(&swapchain->content_valid, 1);
        InterlockedIncrement(&swapchain->present_count);
    }
    return hr;
}

static HRESULT STDMETHODCALLTYPE composition_swapchain_ResizeBuffers(IDXGISwapChain4 *iface,
        UINT buffer_count, UINT width, UINT height, DXGI_FORMAT format, UINT flags)
{
    struct composition_swapchain *swapchain = impl_from_IDXGISwapChain4(iface);

    InterlockedExchange(&swapchain->content_valid, 0);
    return IDXGISwapChain4_ResizeBuffers(swapchain->inner, buffer_count, width, height, format, flags);
}

static HRESULT STDMETHODCALLTYPE composition_swapchain_ResizeBuffers1(IDXGISwapChain4 *iface,
        UINT buffer_count, UINT width, UINT height, DXGI_FORMAT format, UINT flags,
        const UINT *node_mask, IUnknown *const *present_queue)
{
    struct composition_swapchain *swapchain = impl_from_IDXGISwapChain4(iface);

    InterlockedExchange(&swapchain->content_valid, 0);
    return IDXGISwapChain4_ResizeBuffers1(swapchain->inner, buffer_count, width, height, format,
            flags, node_mask, present_queue);
}

static HRESULT STDMETHODCALLTYPE composition_swapchain_GetHwnd(IDXGISwapChain4 *iface, HWND *hwnd)
{
    TRACE("iface %p, hwnd %p.\n", iface, hwnd);

    /* Composition swapchains have no application-visible window. */
    if (hwnd) *hwnd = NULL;
    return DXGI_ERROR_INVALID_CALL;
}

static HRESULT STDMETHODCALLTYPE composition_swapchain_SetPrivateData(IDXGISwapChain4 *iface, REFGUID guid, UINT data_size, const void *data)
{
    struct composition_swapchain *swapchain = impl_from_IDXGISwapChain4(iface);
    return IDXGISwapChain4_SetPrivateData(swapchain->inner, guid, data_size, data);
}

static HRESULT STDMETHODCALLTYPE composition_swapchain_SetPrivateDataInterface(IDXGISwapChain4 *iface, REFGUID guid, const IUnknown *object)
{
    struct composition_swapchain *swapchain = impl_from_IDXGISwapChain4(iface);
    return IDXGISwapChain4_SetPrivateDataInterface(swapchain->inner, guid, object);
}

static HRESULT STDMETHODCALLTYPE composition_swapchain_GetPrivateData(IDXGISwapChain4 *iface, REFGUID guid, UINT *data_size, void *data)
{
    struct composition_swapchain *swapchain = impl_from_IDXGISwapChain4(iface);
    return IDXGISwapChain4_GetPrivateData(swapchain->inner, guid, data_size, data);
}

static HRESULT STDMETHODCALLTYPE composition_swapchain_GetParent(IDXGISwapChain4 *iface, REFIID riid, void **parent)
{
    struct composition_swapchain *swapchain = impl_from_IDXGISwapChain4(iface);
    return IDXGISwapChain4_GetParent(swapchain->inner, riid, parent);
}

static HRESULT STDMETHODCALLTYPE composition_swapchain_GetDevice(IDXGISwapChain4 *iface, REFIID riid, void **device)
{
    struct composition_swapchain *swapchain = impl_from_IDXGISwapChain4(iface);
    return IDXGISwapChain4_GetDevice(swapchain->inner, riid, device);
}

static HRESULT STDMETHODCALLTYPE composition_swapchain_GetBuffer(IDXGISwapChain4 *iface, UINT buffer_idx, REFIID riid, void **surface)
{
    struct composition_swapchain *swapchain = impl_from_IDXGISwapChain4(iface);
    return IDXGISwapChain4_GetBuffer(swapchain->inner, buffer_idx, riid, surface);
}

static HRESULT STDMETHODCALLTYPE composition_swapchain_SetFullscreenState(IDXGISwapChain4 *iface, BOOL fullscreen, IDXGIOutput *target)
{
    struct composition_swapchain *swapchain = impl_from_IDXGISwapChain4(iface);
    return IDXGISwapChain4_SetFullscreenState(swapchain->inner, fullscreen, target);
}

static HRESULT STDMETHODCALLTYPE composition_swapchain_GetFullscreenState(IDXGISwapChain4 *iface, BOOL *fullscreen, IDXGIOutput **target)
{
    struct composition_swapchain *swapchain = impl_from_IDXGISwapChain4(iface);
    return IDXGISwapChain4_GetFullscreenState(swapchain->inner, fullscreen, target);
}

static HRESULT STDMETHODCALLTYPE composition_swapchain_GetDesc(IDXGISwapChain4 *iface, DXGI_SWAP_CHAIN_DESC *desc)
{
    struct composition_swapchain *swapchain = impl_from_IDXGISwapChain4(iface);
    return IDXGISwapChain4_GetDesc(swapchain->inner, desc);
}

static HRESULT STDMETHODCALLTYPE composition_swapchain_ResizeTarget(IDXGISwapChain4 *iface, const DXGI_MODE_DESC *target_mode_desc)
{
    struct composition_swapchain *swapchain = impl_from_IDXGISwapChain4(iface);
    return IDXGISwapChain4_ResizeTarget(swapchain->inner, target_mode_desc);
}

static HRESULT STDMETHODCALLTYPE composition_swapchain_GetContainingOutput(IDXGISwapChain4 *iface, IDXGIOutput **output)
{
    struct composition_swapchain *swapchain = impl_from_IDXGISwapChain4(iface);
    return IDXGISwapChain4_GetContainingOutput(swapchain->inner, output);
}

static HRESULT STDMETHODCALLTYPE composition_swapchain_GetFrameStatistics(IDXGISwapChain4 *iface, DXGI_FRAME_STATISTICS *stats)
{
    struct composition_swapchain *swapchain = impl_from_IDXGISwapChain4(iface);
    return IDXGISwapChain4_GetFrameStatistics(swapchain->inner, stats);
}

static HRESULT STDMETHODCALLTYPE composition_swapchain_GetLastPresentCount(IDXGISwapChain4 *iface, UINT *last_present_count)
{
    struct composition_swapchain *swapchain = impl_from_IDXGISwapChain4(iface);
    return IDXGISwapChain4_GetLastPresentCount(swapchain->inner, last_present_count);
}

static HRESULT STDMETHODCALLTYPE composition_swapchain_GetDesc1(IDXGISwapChain4 *iface, DXGI_SWAP_CHAIN_DESC1 *pDesc)
{
    struct composition_swapchain *swapchain = impl_from_IDXGISwapChain4(iface);
    return IDXGISwapChain4_GetDesc1(swapchain->inner, pDesc);
}

static HRESULT STDMETHODCALLTYPE composition_swapchain_GetFullscreenDesc(IDXGISwapChain4 *iface, DXGI_SWAP_CHAIN_FULLSCREEN_DESC *pDesc)
{
    struct composition_swapchain *swapchain = impl_from_IDXGISwapChain4(iface);
    return IDXGISwapChain4_GetFullscreenDesc(swapchain->inner, pDesc);
}

static HRESULT STDMETHODCALLTYPE composition_swapchain_GetCoreWindow(IDXGISwapChain4 *iface, REFIID refiid, void **ppUnk)
{
    struct composition_swapchain *swapchain = impl_from_IDXGISwapChain4(iface);
    return IDXGISwapChain4_GetCoreWindow(swapchain->inner, refiid, ppUnk);
}

static BOOL STDMETHODCALLTYPE composition_swapchain_IsTemporaryMonoSupported(IDXGISwapChain4 *iface)
{
    struct composition_swapchain *swapchain = impl_from_IDXGISwapChain4(iface);
    return IDXGISwapChain4_IsTemporaryMonoSupported(swapchain->inner);
}

static HRESULT STDMETHODCALLTYPE composition_swapchain_GetRestrictToOutput(IDXGISwapChain4 *iface, IDXGIOutput **ppRestrictToOutput)
{
    struct composition_swapchain *swapchain = impl_from_IDXGISwapChain4(iface);
    return IDXGISwapChain4_GetRestrictToOutput(swapchain->inner, ppRestrictToOutput);
}

static HRESULT STDMETHODCALLTYPE composition_swapchain_SetBackgroundColor(IDXGISwapChain4 *iface, const DXGI_RGBA *pColor)
{
    struct composition_swapchain *swapchain = impl_from_IDXGISwapChain4(iface);
    return IDXGISwapChain4_SetBackgroundColor(swapchain->inner, pColor);
}

static HRESULT STDMETHODCALLTYPE composition_swapchain_GetBackgroundColor(IDXGISwapChain4 *iface, DXGI_RGBA *pColor)
{
    struct composition_swapchain *swapchain = impl_from_IDXGISwapChain4(iface);
    return IDXGISwapChain4_GetBackgroundColor(swapchain->inner, pColor);
}

static HRESULT STDMETHODCALLTYPE composition_swapchain_SetRotation(IDXGISwapChain4 *iface, DXGI_MODE_ROTATION Rotation)
{
    struct composition_swapchain *swapchain = impl_from_IDXGISwapChain4(iface);
    return IDXGISwapChain4_SetRotation(swapchain->inner, Rotation);
}

static HRESULT STDMETHODCALLTYPE composition_swapchain_GetRotation(IDXGISwapChain4 *iface, DXGI_MODE_ROTATION *pRotation)
{
    struct composition_swapchain *swapchain = impl_from_IDXGISwapChain4(iface);
    return IDXGISwapChain4_GetRotation(swapchain->inner, pRotation);
}

static HRESULT STDMETHODCALLTYPE composition_swapchain_SetSourceSize(IDXGISwapChain4 *iface, UINT width, UINT height)
{
    struct composition_swapchain *swapchain = impl_from_IDXGISwapChain4(iface);
    return IDXGISwapChain4_SetSourceSize(swapchain->inner, width, height);
}

static HRESULT STDMETHODCALLTYPE composition_swapchain_GetSourceSize(IDXGISwapChain4 *iface, UINT *width, UINT *height)
{
    struct composition_swapchain *swapchain = impl_from_IDXGISwapChain4(iface);
    return IDXGISwapChain4_GetSourceSize(swapchain->inner, width, height);
}

static HRESULT STDMETHODCALLTYPE composition_swapchain_SetMaximumFrameLatency(IDXGISwapChain4 *iface, UINT max_latency)
{
    struct composition_swapchain *swapchain = impl_from_IDXGISwapChain4(iface);
    return IDXGISwapChain4_SetMaximumFrameLatency(swapchain->inner, max_latency);
}

static HRESULT STDMETHODCALLTYPE composition_swapchain_GetMaximumFrameLatency(IDXGISwapChain4 *iface, UINT *max_latency)
{
    struct composition_swapchain *swapchain = impl_from_IDXGISwapChain4(iface);
    return IDXGISwapChain4_GetMaximumFrameLatency(swapchain->inner, max_latency);
}

static HANDLE STDMETHODCALLTYPE composition_swapchain_GetFrameLatencyWaitableObject(IDXGISwapChain4 *iface)
{
    struct composition_swapchain *swapchain = impl_from_IDXGISwapChain4(iface);
    return IDXGISwapChain4_GetFrameLatencyWaitableObject(swapchain->inner);
}

static HRESULT STDMETHODCALLTYPE composition_swapchain_SetMatrixTransform(IDXGISwapChain4 *iface, const DXGI_MATRIX_3X2_F *matrix)
{
    struct composition_swapchain *swapchain = impl_from_IDXGISwapChain4(iface);
    return IDXGISwapChain4_SetMatrixTransform(swapchain->inner, matrix);
}

static HRESULT STDMETHODCALLTYPE composition_swapchain_GetMatrixTransform(IDXGISwapChain4 *iface, DXGI_MATRIX_3X2_F *matrix)
{
    struct composition_swapchain *swapchain = impl_from_IDXGISwapChain4(iface);
    return IDXGISwapChain4_GetMatrixTransform(swapchain->inner, matrix);
}

static UINT STDMETHODCALLTYPE composition_swapchain_GetCurrentBackBufferIndex(IDXGISwapChain4 *iface)
{
    struct composition_swapchain *swapchain = impl_from_IDXGISwapChain4(iface);
    return IDXGISwapChain4_GetCurrentBackBufferIndex(swapchain->inner);
}

static HRESULT STDMETHODCALLTYPE composition_swapchain_CheckColorSpaceSupport(IDXGISwapChain4 *iface, DXGI_COLOR_SPACE_TYPE colour_space, UINT *colour_space_support)
{
    struct composition_swapchain *swapchain = impl_from_IDXGISwapChain4(iface);
    return IDXGISwapChain4_CheckColorSpaceSupport(swapchain->inner, colour_space, colour_space_support);
}

static HRESULT STDMETHODCALLTYPE composition_swapchain_SetColorSpace1(IDXGISwapChain4 *iface, DXGI_COLOR_SPACE_TYPE colour_space)
{
    struct composition_swapchain *swapchain = impl_from_IDXGISwapChain4(iface);
    return IDXGISwapChain4_SetColorSpace1(swapchain->inner, colour_space);
}

static HRESULT STDMETHODCALLTYPE composition_swapchain_SetHDRMetaData(IDXGISwapChain4 *iface, DXGI_HDR_METADATA_TYPE type, UINT size, void *metadata)
{
    struct composition_swapchain *swapchain = impl_from_IDXGISwapChain4(iface);
    return IDXGISwapChain4_SetHDRMetaData(swapchain->inner, type, size, metadata);
}

static const struct IDXGISwapChain4Vtbl composition_swapchain_vtbl =
{
    composition_swapchain_QueryInterface,
    composition_swapchain_AddRef,
    composition_swapchain_Release,
    composition_swapchain_SetPrivateData,
    composition_swapchain_SetPrivateDataInterface,
    composition_swapchain_GetPrivateData,
    composition_swapchain_GetParent,
    composition_swapchain_GetDevice,
    composition_swapchain_Present,
    composition_swapchain_GetBuffer,
    composition_swapchain_SetFullscreenState,
    composition_swapchain_GetFullscreenState,
    composition_swapchain_GetDesc,
    composition_swapchain_ResizeBuffers,
    composition_swapchain_ResizeTarget,
    composition_swapchain_GetContainingOutput,
    composition_swapchain_GetFrameStatistics,
    composition_swapchain_GetLastPresentCount,
    composition_swapchain_GetDesc1,
    composition_swapchain_GetFullscreenDesc,
    composition_swapchain_GetHwnd,
    composition_swapchain_GetCoreWindow,
    composition_swapchain_Present1,
    composition_swapchain_IsTemporaryMonoSupported,
    composition_swapchain_GetRestrictToOutput,
    composition_swapchain_SetBackgroundColor,
    composition_swapchain_GetBackgroundColor,
    composition_swapchain_SetRotation,
    composition_swapchain_GetRotation,
    composition_swapchain_SetSourceSize,
    composition_swapchain_GetSourceSize,
    composition_swapchain_SetMaximumFrameLatency,
    composition_swapchain_GetMaximumFrameLatency,
    composition_swapchain_GetFrameLatencyWaitableObject,
    composition_swapchain_SetMatrixTransform,
    composition_swapchain_GetMatrixTransform,
    composition_swapchain_GetCurrentBackBufferIndex,
    composition_swapchain_CheckColorSpaceSupport,
    composition_swapchain_SetColorSpace1,
    composition_swapchain_ResizeBuffers1,
    composition_swapchain_SetHDRMetaData,
};

HRESULT composition_swapchain_wrap(IDXGISwapChain1 *inner, HWND window, IDXGISwapChain1 **out)
{
    struct composition_swapchain *object;
    HRESULT hr;

    if (!(object = calloc(1, sizeof(*object)))) return E_OUTOFMEMORY;
    object->IDXGISwapChain4_iface.lpVtbl = &composition_swapchain_vtbl;
    object->refcount = 1;
    object->window = window;
    InitializeCriticalSectionEx(&object->cs, 0, RTL_CRITICAL_SECTION_FLAG_FORCE_DEBUG_INFO);
    object->cs.DebugInfo->Spare[0] = (DWORD_PTR)(__FILE__ ": composition_swapchain.cs");
    if (FAILED(hr = IDXGISwapChain1_QueryInterface(inner, &IID_IDXGISwapChain4, (void **)&object->inner)))
    {
        free(object);
        return hr;
    }

    TRACE("Wrapped composition swapchain %p around %p.\n", object, inner);
    *out = (IDXGISwapChain1 *)&object->IDXGISwapChain4_iface;
    return S_OK;
}
