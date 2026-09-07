/*
 * ID3D11VideoProcessor and related objects
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

#include "d3d11_private.h"

WINE_DEFAULT_DEBUG_CHANNEL(d3d11);

/* Formats the (blit based) processor can read and write.  YUV formats are
 * accepted as input only; wined3d converts them when blitting. */
static BOOL format_is_rgb(DXGI_FORMAT format)
{
    switch (format)
    {
        case DXGI_FORMAT_B8G8R8A8_UNORM:
        case DXGI_FORMAT_B8G8R8X8_UNORM:
        case DXGI_FORMAT_R8G8B8A8_UNORM:
        case DXGI_FORMAT_R10G10B10A2_UNORM:
        case DXGI_FORMAT_R16G16B16A16_FLOAT:
            return TRUE;
        default:
            return FALSE;
    }
}

static BOOL format_is_video_input(DXGI_FORMAT format)
{
    switch (format)
    {
        case DXGI_FORMAT_NV12:
        case DXGI_FORMAT_YUY2:
        case DXGI_FORMAT_P010:
            return TRUE;
        default:
            return format_is_rgb(format);
    }
}

/* ------------------------------------------------------------------ */
/* Enumerator */

static struct d3d11_video_processor_enumerator *impl_from_ID3D11VideoProcessorEnumerator1(
        ID3D11VideoProcessorEnumerator1 *iface)
{
    return CONTAINING_RECORD(iface, struct d3d11_video_processor_enumerator,
            ID3D11VideoProcessorEnumerator1_iface);
}

static HRESULT STDMETHODCALLTYPE d3d11_video_processor_enumerator_QueryInterface(
        ID3D11VideoProcessorEnumerator1 *iface, REFIID riid, void **object)
{
    TRACE("iface %p, riid %s, object %p.\n", iface, debugstr_guid(riid), object);

    if (IsEqualGUID(riid, &IID_ID3D11VideoProcessorEnumerator1)
            || IsEqualGUID(riid, &IID_ID3D11VideoProcessorEnumerator)
            || IsEqualGUID(riid, &IID_ID3D11DeviceChild)
            || IsEqualGUID(riid, &IID_IUnknown))
    {
        ID3D11VideoProcessorEnumerator1_AddRef(iface);
        *object = iface;
        return S_OK;
    }

    WARN("%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid(riid));
    *object = NULL;
    return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE d3d11_video_processor_enumerator_AddRef(ID3D11VideoProcessorEnumerator1 *iface)
{
    struct d3d11_video_processor_enumerator *enumerator = impl_from_ID3D11VideoProcessorEnumerator1(iface);
    return InterlockedIncrement(&enumerator->refcount);
}

static ULONG STDMETHODCALLTYPE d3d11_video_processor_enumerator_Release(ID3D11VideoProcessorEnumerator1 *iface)
{
    struct d3d11_video_processor_enumerator *enumerator = impl_from_ID3D11VideoProcessorEnumerator1(iface);
    ULONG refcount = InterlockedDecrement(&enumerator->refcount);

    if (!refcount)
    {
        ID3D11Device5_Release(&enumerator->device->ID3D11Device5_iface);
        wined3d_private_store_cleanup(&enumerator->private_store);
        free(enumerator);
    }
    return refcount;
}

static void STDMETHODCALLTYPE d3d11_video_processor_enumerator_GetDevice(
        ID3D11VideoProcessorEnumerator1 *iface, ID3D11Device **device)
{
    struct d3d11_video_processor_enumerator *enumerator = impl_from_ID3D11VideoProcessorEnumerator1(iface);

    TRACE("iface %p, device %p.\n", iface, device);

    *device = (ID3D11Device *)&enumerator->device->ID3D11Device5_iface;
    ID3D11Device_AddRef(*device);
}

static HRESULT STDMETHODCALLTYPE d3d11_video_processor_enumerator_GetPrivateData(
        ID3D11VideoProcessorEnumerator1 *iface, REFGUID guid, UINT *data_size, void *data)
{
    struct d3d11_video_processor_enumerator *enumerator = impl_from_ID3D11VideoProcessorEnumerator1(iface);
    return d3d_get_private_data(&enumerator->private_store, guid, data_size, data);
}

static HRESULT STDMETHODCALLTYPE d3d11_video_processor_enumerator_SetPrivateData(
        ID3D11VideoProcessorEnumerator1 *iface, REFGUID guid, UINT data_size, const void *data)
{
    struct d3d11_video_processor_enumerator *enumerator = impl_from_ID3D11VideoProcessorEnumerator1(iface);
    return d3d_set_private_data(&enumerator->private_store, guid, data_size, data);
}

static HRESULT STDMETHODCALLTYPE d3d11_video_processor_enumerator_SetPrivateDataInterface(
        ID3D11VideoProcessorEnumerator1 *iface, REFGUID guid, const IUnknown *data)
{
    struct d3d11_video_processor_enumerator *enumerator = impl_from_ID3D11VideoProcessorEnumerator1(iface);
    return d3d_set_private_data_interface(&enumerator->private_store, guid, data);
}

static HRESULT STDMETHODCALLTYPE d3d11_video_processor_enumerator_GetVideoProcessorContentDesc(
        ID3D11VideoProcessorEnumerator1 *iface, D3D11_VIDEO_PROCESSOR_CONTENT_DESC *desc)
{
    struct d3d11_video_processor_enumerator *enumerator = impl_from_ID3D11VideoProcessorEnumerator1(iface);

    TRACE("iface %p, desc %p.\n", iface, desc);

    if (!desc) return E_POINTER;
    *desc = enumerator->desc;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE d3d11_video_processor_enumerator_CheckVideoProcessorFormat(
        ID3D11VideoProcessorEnumerator1 *iface, DXGI_FORMAT format, UINT *flags)
{
    TRACE("iface %p, format %#x, flags %p.\n", iface, format, flags);

    if (!flags) return E_POINTER;

    *flags = 0;
    if (format_is_video_input(format))
        *flags |= D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_INPUT;
    if (format_is_rgb(format))
        *flags |= D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_OUTPUT;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE d3d11_video_processor_enumerator_GetVideoProcessorCaps(
        ID3D11VideoProcessorEnumerator1 *iface, D3D11_VIDEO_PROCESSOR_CAPS *caps)
{
    TRACE("iface %p, caps %p.\n", iface, caps);

    if (!caps) return E_POINTER;

    memset(caps, 0, sizeof(*caps));
    caps->RateConversionCapsCount = 1;
    caps->MaxInputStreams = 1;
    caps->MaxStreamStates = 1;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE d3d11_video_processor_enumerator_GetVideoProcessorRateConversionCaps(
        ID3D11VideoProcessorEnumerator1 *iface, UINT index, D3D11_VIDEO_PROCESSOR_RATE_CONVERSION_CAPS *caps)
{
    TRACE("iface %p, index %u, caps %p.\n", iface, index, caps);

    if (!caps) return E_POINTER;
    if (index)
        return E_INVALIDARG;

    memset(caps, 0, sizeof(*caps));
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE d3d11_video_processor_enumerator_GetVideoProcessorCustomRate(
        ID3D11VideoProcessorEnumerator1 *iface, UINT index, UINT custom_rate_index,
        D3D11_VIDEO_PROCESSOR_CUSTOM_RATE *rate)
{
    TRACE("iface %p, index %u, custom_rate_index %u, rate %p.\n", iface, index, custom_rate_index, rate);
    return E_INVALIDARG;
}

static HRESULT STDMETHODCALLTYPE d3d11_video_processor_enumerator_GetVideoProcessorFilterRange(
        ID3D11VideoProcessorEnumerator1 *iface, D3D11_VIDEO_PROCESSOR_FILTER filter,
        D3D11_VIDEO_PROCESSOR_FILTER_RANGE *range)
{
    TRACE("iface %p, filter %#x, range %p.\n", iface, filter, range);

    if (!range) return E_POINTER;

    /* No filters are exposed in FilterCaps; return an inert range for
     * callers that ask anyway. */
    range->Minimum = 0;
    range->Maximum = 0;
    range->Default = 0;
    range->Multiplier = 1.0f;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE d3d11_video_processor_enumerator_CheckVideoProcessorFormatConversion(
        ID3D11VideoProcessorEnumerator1 *iface, DXGI_FORMAT input_format, DXGI_COLOR_SPACE_TYPE input_cs,
        DXGI_FORMAT output_format, DXGI_COLOR_SPACE_TYPE output_cs, BOOL *supported)
{
    TRACE("iface %p, input_format %#x, input_cs %#x, output_format %#x, output_cs %#x, supported %p.\n",
            iface, input_format, input_cs, output_format, output_cs, supported);

    if (!supported) return E_POINTER;
    *supported = format_is_video_input(input_format) && format_is_rgb(output_format);
    return S_OK;
}

static const struct ID3D11VideoProcessorEnumerator1Vtbl d3d11_video_processor_enumerator_vtbl =
{
    d3d11_video_processor_enumerator_QueryInterface,
    d3d11_video_processor_enumerator_AddRef,
    d3d11_video_processor_enumerator_Release,
    d3d11_video_processor_enumerator_GetDevice,
    d3d11_video_processor_enumerator_GetPrivateData,
    d3d11_video_processor_enumerator_SetPrivateData,
    d3d11_video_processor_enumerator_SetPrivateDataInterface,
    d3d11_video_processor_enumerator_GetVideoProcessorContentDesc,
    d3d11_video_processor_enumerator_CheckVideoProcessorFormat,
    d3d11_video_processor_enumerator_GetVideoProcessorCaps,
    d3d11_video_processor_enumerator_GetVideoProcessorRateConversionCaps,
    d3d11_video_processor_enumerator_GetVideoProcessorCustomRate,
    d3d11_video_processor_enumerator_GetVideoProcessorFilterRange,
    d3d11_video_processor_enumerator_CheckVideoProcessorFormatConversion,
};

struct d3d11_video_processor_enumerator *unsafe_impl_from_ID3D11VideoProcessorEnumerator(
        ID3D11VideoProcessorEnumerator *iface)
{
    if (!iface || iface->lpVtbl != (const ID3D11VideoProcessorEnumeratorVtbl *)&d3d11_video_processor_enumerator_vtbl)
        return NULL;
    return impl_from_ID3D11VideoProcessorEnumerator1((ID3D11VideoProcessorEnumerator1 *)iface);
}

HRESULT d3d11_video_processor_enumerator_create(struct d3d_device *device,
        const D3D11_VIDEO_PROCESSOR_CONTENT_DESC *desc, struct d3d11_video_processor_enumerator **enumerator)
{
    struct d3d11_video_processor_enumerator *object;

    if (!(object = calloc(1, sizeof(*object)))) return E_OUTOFMEMORY;
    object->ID3D11VideoProcessorEnumerator1_iface.lpVtbl = &d3d11_video_processor_enumerator_vtbl;
    object->refcount = 1;
    wined3d_private_store_init(&object->private_store);
    object->desc = *desc;
    object->device = device;
    ID3D11Device5_AddRef(&device->ID3D11Device5_iface);

    TRACE("Created video processor enumerator %p.\n", object);
    *enumerator = object;
    return S_OK;
}

/* ------------------------------------------------------------------ */
/* Processor */

static struct d3d11_video_processor *impl_from_ID3D11VideoProcessor(ID3D11VideoProcessor *iface)
{
    return CONTAINING_RECORD(iface, struct d3d11_video_processor, ID3D11VideoProcessor_iface);
}

static HRESULT STDMETHODCALLTYPE d3d11_video_processor_QueryInterface(ID3D11VideoProcessor *iface,
        REFIID riid, void **object)
{
    TRACE("iface %p, riid %s, object %p.\n", iface, debugstr_guid(riid), object);

    if (IsEqualGUID(riid, &IID_ID3D11VideoProcessor)
            || IsEqualGUID(riid, &IID_ID3D11DeviceChild)
            || IsEqualGUID(riid, &IID_IUnknown))
    {
        ID3D11VideoProcessor_AddRef(iface);
        *object = iface;
        return S_OK;
    }

    WARN("%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid(riid));
    *object = NULL;
    return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE d3d11_video_processor_AddRef(ID3D11VideoProcessor *iface)
{
    struct d3d11_video_processor *processor = impl_from_ID3D11VideoProcessor(iface);
    return InterlockedIncrement(&processor->refcount);
}

static ULONG STDMETHODCALLTYPE d3d11_video_processor_Release(ID3D11VideoProcessor *iface)
{
    struct d3d11_video_processor *processor = impl_from_ID3D11VideoProcessor(iface);
    ULONG refcount = InterlockedDecrement(&processor->refcount);

    if (!refcount)
    {
        ID3D11VideoProcessorEnumerator1_Release(&processor->enumerator->ID3D11VideoProcessorEnumerator1_iface);
        wined3d_private_store_cleanup(&processor->private_store);
        free(processor);
    }
    return refcount;
}

static void STDMETHODCALLTYPE d3d11_video_processor_GetDevice(ID3D11VideoProcessor *iface, ID3D11Device **device)
{
    struct d3d11_video_processor *processor = impl_from_ID3D11VideoProcessor(iface);

    TRACE("iface %p, device %p.\n", iface, device);

    *device = (ID3D11Device *)&processor->enumerator->device->ID3D11Device5_iface;
    ID3D11Device_AddRef(*device);
}

static HRESULT STDMETHODCALLTYPE d3d11_video_processor_GetPrivateData(ID3D11VideoProcessor *iface,
        REFGUID guid, UINT *data_size, void *data)
{
    struct d3d11_video_processor *processor = impl_from_ID3D11VideoProcessor(iface);
    return d3d_get_private_data(&processor->private_store, guid, data_size, data);
}

static HRESULT STDMETHODCALLTYPE d3d11_video_processor_SetPrivateData(ID3D11VideoProcessor *iface,
        REFGUID guid, UINT data_size, const void *data)
{
    struct d3d11_video_processor *processor = impl_from_ID3D11VideoProcessor(iface);
    return d3d_set_private_data(&processor->private_store, guid, data_size, data);
}

static HRESULT STDMETHODCALLTYPE d3d11_video_processor_SetPrivateDataInterface(ID3D11VideoProcessor *iface,
        REFGUID guid, const IUnknown *data)
{
    struct d3d11_video_processor *processor = impl_from_ID3D11VideoProcessor(iface);
    return d3d_set_private_data_interface(&processor->private_store, guid, data);
}

static void STDMETHODCALLTYPE d3d11_video_processor_GetContentDesc(ID3D11VideoProcessor *iface,
        D3D11_VIDEO_PROCESSOR_CONTENT_DESC *desc)
{
    struct d3d11_video_processor *processor = impl_from_ID3D11VideoProcessor(iface);

    TRACE("iface %p, desc %p.\n", iface, desc);

    *desc = processor->enumerator->desc;
}

static void STDMETHODCALLTYPE d3d11_video_processor_GetRateConversionCaps(ID3D11VideoProcessor *iface,
        D3D11_VIDEO_PROCESSOR_RATE_CONVERSION_CAPS *caps)
{
    TRACE("iface %p, caps %p.\n", iface, caps);

    memset(caps, 0, sizeof(*caps));
}

static const struct ID3D11VideoProcessorVtbl d3d11_video_processor_vtbl =
{
    d3d11_video_processor_QueryInterface,
    d3d11_video_processor_AddRef,
    d3d11_video_processor_Release,
    d3d11_video_processor_GetDevice,
    d3d11_video_processor_GetPrivateData,
    d3d11_video_processor_SetPrivateData,
    d3d11_video_processor_SetPrivateDataInterface,
    d3d11_video_processor_GetContentDesc,
    d3d11_video_processor_GetRateConversionCaps,
};

struct d3d11_video_processor *unsafe_impl_from_ID3D11VideoProcessor(ID3D11VideoProcessor *iface)
{
    if (!iface || iface->lpVtbl != &d3d11_video_processor_vtbl)
        return NULL;
    return impl_from_ID3D11VideoProcessor(iface);
}

HRESULT d3d11_video_processor_create(struct d3d11_video_processor_enumerator *enumerator, UINT rate_index,
        struct d3d11_video_processor **processor)
{
    struct d3d11_video_processor *object;

    if (!(object = calloc(1, sizeof(*object)))) return E_OUTOFMEMORY;
    object->ID3D11VideoProcessor_iface.lpVtbl = &d3d11_video_processor_vtbl;
    object->refcount = 1;
    wined3d_private_store_init(&object->private_store);
    object->enumerator = enumerator;
    object->rate_index = rate_index;
    ID3D11VideoProcessorEnumerator1_AddRef(&enumerator->ID3D11VideoProcessorEnumerator1_iface);

    TRACE("Created video processor %p.\n", object);
    *processor = object;
    return S_OK;
}

/* ------------------------------------------------------------------ */
/* Input / output views */

static struct d3d11_video_processor_input_view *impl_from_ID3D11VideoProcessorInputView(
        ID3D11VideoProcessorInputView *iface)
{
    return CONTAINING_RECORD(iface, struct d3d11_video_processor_input_view,
            ID3D11VideoProcessorInputView_iface);
}

static HRESULT STDMETHODCALLTYPE d3d11_video_processor_input_view_QueryInterface(
        ID3D11VideoProcessorInputView *iface, REFIID riid, void **object)
{
    TRACE("iface %p, riid %s, object %p.\n", iface, debugstr_guid(riid), object);

    if (IsEqualGUID(riid, &IID_ID3D11VideoProcessorInputView)
            || IsEqualGUID(riid, &IID_ID3D11View)
            || IsEqualGUID(riid, &IID_ID3D11DeviceChild)
            || IsEqualGUID(riid, &IID_IUnknown))
    {
        ID3D11VideoProcessorInputView_AddRef(iface);
        *object = iface;
        return S_OK;
    }

    WARN("%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid(riid));
    *object = NULL;
    return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE d3d11_video_processor_input_view_AddRef(ID3D11VideoProcessorInputView *iface)
{
    struct d3d11_video_processor_input_view *view = impl_from_ID3D11VideoProcessorInputView(iface);
    return InterlockedIncrement(&view->refcount);
}

static ULONG STDMETHODCALLTYPE d3d11_video_processor_input_view_Release(ID3D11VideoProcessorInputView *iface)
{
    struct d3d11_video_processor_input_view *view = impl_from_ID3D11VideoProcessorInputView(iface);
    ULONG refcount = InterlockedDecrement(&view->refcount);

    if (!refcount)
    {
        ID3D11Resource_Release(view->resource);
        ID3D11Device5_Release(&view->device->ID3D11Device5_iface);
        wined3d_private_store_cleanup(&view->private_store);
        free(view);
    }
    return refcount;
}

static void STDMETHODCALLTYPE d3d11_video_processor_input_view_GetDevice(
        ID3D11VideoProcessorInputView *iface, ID3D11Device **device)
{
    struct d3d11_video_processor_input_view *view = impl_from_ID3D11VideoProcessorInputView(iface);

    TRACE("iface %p, device %p.\n", iface, device);

    *device = (ID3D11Device *)&view->device->ID3D11Device5_iface;
    ID3D11Device_AddRef(*device);
}

static HRESULT STDMETHODCALLTYPE d3d11_video_processor_input_view_GetPrivateData(
        ID3D11VideoProcessorInputView *iface, REFGUID guid, UINT *data_size, void *data)
{
    struct d3d11_video_processor_input_view *view = impl_from_ID3D11VideoProcessorInputView(iface);
    return d3d_get_private_data(&view->private_store, guid, data_size, data);
}

static HRESULT STDMETHODCALLTYPE d3d11_video_processor_input_view_SetPrivateData(
        ID3D11VideoProcessorInputView *iface, REFGUID guid, UINT data_size, const void *data)
{
    struct d3d11_video_processor_input_view *view = impl_from_ID3D11VideoProcessorInputView(iface);
    return d3d_set_private_data(&view->private_store, guid, data_size, data);
}

static HRESULT STDMETHODCALLTYPE d3d11_video_processor_input_view_SetPrivateDataInterface(
        ID3D11VideoProcessorInputView *iface, REFGUID guid, const IUnknown *data)
{
    struct d3d11_video_processor_input_view *view = impl_from_ID3D11VideoProcessorInputView(iface);
    return d3d_set_private_data_interface(&view->private_store, guid, data);
}

static void STDMETHODCALLTYPE d3d11_video_processor_input_view_GetResource(
        ID3D11VideoProcessorInputView *iface, ID3D11Resource **resource)
{
    struct d3d11_video_processor_input_view *view = impl_from_ID3D11VideoProcessorInputView(iface);

    TRACE("iface %p, resource %p.\n", iface, resource);

    *resource = view->resource;
    ID3D11Resource_AddRef(*resource);
}

static void STDMETHODCALLTYPE d3d11_video_processor_input_view_GetDesc(
        ID3D11VideoProcessorInputView *iface, D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC *desc)
{
    struct d3d11_video_processor_input_view *view = impl_from_ID3D11VideoProcessorInputView(iface);

    TRACE("iface %p, desc %p.\n", iface, desc);

    *desc = view->desc;
}

static const struct ID3D11VideoProcessorInputViewVtbl d3d11_video_processor_input_view_vtbl =
{
    d3d11_video_processor_input_view_QueryInterface,
    d3d11_video_processor_input_view_AddRef,
    d3d11_video_processor_input_view_Release,
    d3d11_video_processor_input_view_GetDevice,
    d3d11_video_processor_input_view_GetPrivateData,
    d3d11_video_processor_input_view_SetPrivateData,
    d3d11_video_processor_input_view_SetPrivateDataInterface,
    d3d11_video_processor_input_view_GetResource,
    d3d11_video_processor_input_view_GetDesc,
};

struct d3d11_video_processor_input_view *unsafe_impl_from_ID3D11VideoProcessorInputView(
        ID3D11VideoProcessorInputView *iface)
{
    if (!iface || iface->lpVtbl != &d3d11_video_processor_input_view_vtbl)
        return NULL;
    return impl_from_ID3D11VideoProcessorInputView(iface);
}

HRESULT d3d11_video_processor_input_view_create(struct d3d_device *device, ID3D11Resource *resource,
        const D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC *desc, struct d3d11_video_processor_input_view **out)
{
    struct d3d11_video_processor_input_view *object;
    struct d3d_texture2d *texture_impl;
    ID3D11Texture2D *texture;

    if (FAILED(ID3D11Resource_QueryInterface(resource, &IID_ID3D11Texture2D, (void **)&texture)))
    {
        WARN("Resource %p is not a 2D texture.\n", resource);
        return E_INVALIDARG;
    }
    texture_impl = unsafe_impl_from_ID3D11Texture2D(texture);

    if (!(object = calloc(1, sizeof(*object))))
    {
        ID3D11Texture2D_Release(texture);
        return E_OUTOFMEMORY;
    }
    object->ID3D11VideoProcessorInputView_iface.lpVtbl = &d3d11_video_processor_input_view_vtbl;
    object->refcount = 1;
    wined3d_private_store_init(&object->private_store);
    object->resource = resource;
    ID3D11Resource_AddRef(resource);
    object->desc = *desc;
    object->wined3d_texture = texture_impl->wined3d_texture;
    object->sub_idx = desc->Texture2D.ArraySlice * texture_impl->desc.MipLevels + desc->Texture2D.MipSlice;
    object->width = max(1, texture_impl->desc.Width >> desc->Texture2D.MipSlice);
    object->height = max(1, texture_impl->desc.Height >> desc->Texture2D.MipSlice);
    object->device = device;
    ID3D11Device5_AddRef(&device->ID3D11Device5_iface);
    ID3D11Texture2D_Release(texture);

    TRACE("Created video processor input view %p.\n", object);
    *out = object;
    return S_OK;
}

static struct d3d11_video_processor_output_view *impl_from_ID3D11VideoProcessorOutputView(
        ID3D11VideoProcessorOutputView *iface)
{
    return CONTAINING_RECORD(iface, struct d3d11_video_processor_output_view,
            ID3D11VideoProcessorOutputView_iface);
}

static HRESULT STDMETHODCALLTYPE d3d11_video_processor_output_view_QueryInterface(
        ID3D11VideoProcessorOutputView *iface, REFIID riid, void **object)
{
    TRACE("iface %p, riid %s, object %p.\n", iface, debugstr_guid(riid), object);

    if (IsEqualGUID(riid, &IID_ID3D11VideoProcessorOutputView)
            || IsEqualGUID(riid, &IID_ID3D11View)
            || IsEqualGUID(riid, &IID_ID3D11DeviceChild)
            || IsEqualGUID(riid, &IID_IUnknown))
    {
        ID3D11VideoProcessorOutputView_AddRef(iface);
        *object = iface;
        return S_OK;
    }

    WARN("%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid(riid));
    *object = NULL;
    return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE d3d11_video_processor_output_view_AddRef(ID3D11VideoProcessorOutputView *iface)
{
    struct d3d11_video_processor_output_view *view = impl_from_ID3D11VideoProcessorOutputView(iface);
    return InterlockedIncrement(&view->refcount);
}

static ULONG STDMETHODCALLTYPE d3d11_video_processor_output_view_Release(ID3D11VideoProcessorOutputView *iface)
{
    struct d3d11_video_processor_output_view *view = impl_from_ID3D11VideoProcessorOutputView(iface);
    ULONG refcount = InterlockedDecrement(&view->refcount);

    if (!refcount)
    {
        ID3D11Resource_Release(view->resource);
        ID3D11Device5_Release(&view->device->ID3D11Device5_iface);
        wined3d_private_store_cleanup(&view->private_store);
        free(view);
    }
    return refcount;
}

static void STDMETHODCALLTYPE d3d11_video_processor_output_view_GetDevice(
        ID3D11VideoProcessorOutputView *iface, ID3D11Device **device)
{
    struct d3d11_video_processor_output_view *view = impl_from_ID3D11VideoProcessorOutputView(iface);

    TRACE("iface %p, device %p.\n", iface, device);

    *device = (ID3D11Device *)&view->device->ID3D11Device5_iface;
    ID3D11Device_AddRef(*device);
}

static HRESULT STDMETHODCALLTYPE d3d11_video_processor_output_view_GetPrivateData(
        ID3D11VideoProcessorOutputView *iface, REFGUID guid, UINT *data_size, void *data)
{
    struct d3d11_video_processor_output_view *view = impl_from_ID3D11VideoProcessorOutputView(iface);
    return d3d_get_private_data(&view->private_store, guid, data_size, data);
}

static HRESULT STDMETHODCALLTYPE d3d11_video_processor_output_view_SetPrivateData(
        ID3D11VideoProcessorOutputView *iface, REFGUID guid, UINT data_size, const void *data)
{
    struct d3d11_video_processor_output_view *view = impl_from_ID3D11VideoProcessorOutputView(iface);
    return d3d_set_private_data(&view->private_store, guid, data_size, data);
}

static HRESULT STDMETHODCALLTYPE d3d11_video_processor_output_view_SetPrivateDataInterface(
        ID3D11VideoProcessorOutputView *iface, REFGUID guid, const IUnknown *data)
{
    struct d3d11_video_processor_output_view *view = impl_from_ID3D11VideoProcessorOutputView(iface);
    return d3d_set_private_data_interface(&view->private_store, guid, data);
}

static void STDMETHODCALLTYPE d3d11_video_processor_output_view_GetResource(
        ID3D11VideoProcessorOutputView *iface, ID3D11Resource **resource)
{
    struct d3d11_video_processor_output_view *view = impl_from_ID3D11VideoProcessorOutputView(iface);

    TRACE("iface %p, resource %p.\n", iface, resource);

    *resource = view->resource;
    ID3D11Resource_AddRef(*resource);
}

static void STDMETHODCALLTYPE d3d11_video_processor_output_view_GetDesc(
        ID3D11VideoProcessorOutputView *iface, D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC *desc)
{
    struct d3d11_video_processor_output_view *view = impl_from_ID3D11VideoProcessorOutputView(iface);

    TRACE("iface %p, desc %p.\n", iface, desc);

    *desc = view->desc;
}

static const struct ID3D11VideoProcessorOutputViewVtbl d3d11_video_processor_output_view_vtbl =
{
    d3d11_video_processor_output_view_QueryInterface,
    d3d11_video_processor_output_view_AddRef,
    d3d11_video_processor_output_view_Release,
    d3d11_video_processor_output_view_GetDevice,
    d3d11_video_processor_output_view_GetPrivateData,
    d3d11_video_processor_output_view_SetPrivateData,
    d3d11_video_processor_output_view_SetPrivateDataInterface,
    d3d11_video_processor_output_view_GetResource,
    d3d11_video_processor_output_view_GetDesc,
};

struct d3d11_video_processor_output_view *unsafe_impl_from_ID3D11VideoProcessorOutputView(
        ID3D11VideoProcessorOutputView *iface)
{
    if (!iface || iface->lpVtbl != &d3d11_video_processor_output_view_vtbl)
        return NULL;
    return impl_from_ID3D11VideoProcessorOutputView(iface);
}

HRESULT d3d11_video_processor_output_view_create(struct d3d_device *device, ID3D11Resource *resource,
        const D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC *desc, struct d3d11_video_processor_output_view **out)
{
    struct d3d11_video_processor_output_view *object;
    struct d3d_texture2d *texture_impl;
    ID3D11Texture2D *texture;

    if (FAILED(ID3D11Resource_QueryInterface(resource, &IID_ID3D11Texture2D, (void **)&texture)))
    {
        WARN("Resource %p is not a 2D texture.\n", resource);
        return E_INVALIDARG;
    }
    texture_impl = unsafe_impl_from_ID3D11Texture2D(texture);

    if (!(object = calloc(1, sizeof(*object))))
    {
        ID3D11Texture2D_Release(texture);
        return E_OUTOFMEMORY;
    }
    object->ID3D11VideoProcessorOutputView_iface.lpVtbl = &d3d11_video_processor_output_view_vtbl;
    object->refcount = 1;
    wined3d_private_store_init(&object->private_store);
    object->resource = resource;
    ID3D11Resource_AddRef(resource);
    object->desc = *desc;
    object->wined3d_texture = texture_impl->wined3d_texture;
    object->sub_idx = desc->Texture2D.MipSlice;
    object->width = max(1, texture_impl->desc.Width >> desc->Texture2D.MipSlice);
    object->height = max(1, texture_impl->desc.Height >> desc->Texture2D.MipSlice);
    object->device = device;
    ID3D11Device5_AddRef(&device->ID3D11Device5_iface);
    ID3D11Texture2D_Release(texture);

    TRACE("Created video processor output view %p.\n", object);
    *out = object;
    return S_OK;
}
