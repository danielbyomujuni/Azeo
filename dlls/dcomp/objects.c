/*
 * DirectComposition targets, visuals and surfaces
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

#include "dcomp_private.h"

WINE_DEFAULT_DEBUG_CHANNEL(dcomp);

/* ------------------------------------------------------------------ */
/* Generic inert object.
 *
 * Transforms, clips, effects and animations only need to exist and accept
 * their setters; nothing composites them.  One refcounted object with a
 * wide vtbl of S_OK-returning slots covers all of them.  QueryInterface
 * answers only IUnknown and the interface the object was created as.
 */

#define STUB_VTBL_SIZE 64

struct stub_object
{
    const void **lpVtbl;
    LONG refcount;
    IID iid;
    char name[48];
};

static HRESULT WINAPI stub_QueryInterface(struct stub_object *stub, REFIID iid, void **out)
{
    if (IsEqualGUID(iid, &IID_IUnknown) || IsEqualGUID(iid, &stub->iid))
    {
        InterlockedIncrement(&stub->refcount);
        *out = stub;
        return S_OK;
    }
    WARN("%s: unsupported interface %s.\n", stub->name, debugstr_guid(iid));
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI stub_AddRef(struct stub_object *stub)
{
    return InterlockedIncrement(&stub->refcount);
}

static ULONG WINAPI stub_Release(struct stub_object *stub)
{
    ULONG refcount = InterlockedDecrement(&stub->refcount);
    if (!refcount) free(stub);
    return refcount;
}

static HRESULT WINAPI stub_method(struct stub_object *stub)
{
    TRACE("%s: ignored method call.\n", stub->name);
    return S_OK;
}

static const void *stub_vtbl[STUB_VTBL_SIZE] =
{
    stub_QueryInterface,
    stub_AddRef,
    stub_Release,
    /* remaining slots filled at first use */
};

static void init_stub_vtbl(void)
{
    unsigned int i;
    if (stub_vtbl[3]) return;
    for (i = 3; i < STUB_VTBL_SIZE; ++i)
        stub_vtbl[i] = stub_method;
}

HRESULT dcomp_stub_object_create(const char *name, REFIID iid, void **out)
{
    struct stub_object *stub;

    init_stub_vtbl();

    if (!(stub = calloc(1, sizeof(*stub)))) return E_OUTOFMEMORY;
    stub->lpVtbl = stub_vtbl;
    stub->refcount = 1;
    stub->iid = *iid;
    lstrcpynA(stub->name, name, sizeof(stub->name));

    TRACE("created inert %s %p.\n", name, stub);

    *out = stub;
    return S_OK;
}

/* ------------------------------------------------------------------ */
/* Visual */

static struct dcomp_visual *impl_from_IDCompositionVisual2(IDCompositionVisual2 *iface)
{
    return CONTAINING_RECORD(iface, struct dcomp_visual, IDCompositionVisual2_iface);
}

static HRESULT WINAPI dcomp_visual_QueryInterface(IDCompositionVisual2 *iface, REFIID iid, void **out)
{
    TRACE("iface %p, iid %s, out %p.\n", iface, debugstr_guid(iid), out);

    if (IsEqualGUID(iid, &IID_IUnknown)
            || IsEqualGUID(iid, &IID_IDCompositionVisual)
            || IsEqualGUID(iid, &IID_IDCompositionVisual2))
    {
        IDCompositionVisual2_AddRef(iface);
        *out = iface;
        return S_OK;
    }
    WARN("Unsupported interface %s.\n", debugstr_guid(iid));
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI dcomp_visual_AddRef(IDCompositionVisual2 *iface)
{
    struct dcomp_visual *visual = impl_from_IDCompositionVisual2(iface);
    return InterlockedIncrement(&visual->refcount);
}

static void visual_clear_children(struct dcomp_visual *visual)
{
    struct visual_child *child, *next;

    LIST_FOR_EACH_ENTRY_SAFE(child, next, &visual->children, struct visual_child, entry)
    {
        list_remove(&child->entry);
        IDCompositionVisual2_Release(&child->visual->IDCompositionVisual2_iface);
        free(child);
    }
}

static ULONG WINAPI dcomp_visual_Release(IDCompositionVisual2 *iface)
{
    struct dcomp_visual *visual = impl_from_IDCompositionVisual2(iface);
    ULONG refcount = InterlockedDecrement(&visual->refcount);

    if (!refcount)
    {
        visual_clear_children(visual);
        if (visual->content) IUnknown_Release(visual->content);
        visual->cs.DebugInfo->Spare[0] = 0;
        DeleteCriticalSection(&visual->cs);
        free(visual);
    }
    return refcount;
}

static HRESULT WINAPI dcomp_visual_SetOffsetXAnimation(IDCompositionVisual2 *iface, IDCompositionAnimation *animation)
{
    TRACE("iface %p, animation %p - ignored.\n", iface, animation);
    return S_OK;
}

static HRESULT WINAPI dcomp_visual_SetOffsetX(IDCompositionVisual2 *iface, float offset_x)
{
    struct dcomp_visual *visual = impl_from_IDCompositionVisual2(iface);

    TRACE("iface %p, offset_x %.8e.\n", iface, offset_x);

    EnterCriticalSection(&visual->cs);
    visual->offset_x = offset_x;
    LeaveCriticalSection(&visual->cs);
    return S_OK;
}

static HRESULT WINAPI dcomp_visual_SetOffsetYAnimation(IDCompositionVisual2 *iface, IDCompositionAnimation *animation)
{
    TRACE("iface %p, animation %p - ignored.\n", iface, animation);
    return S_OK;
}

static HRESULT WINAPI dcomp_visual_SetOffsetY(IDCompositionVisual2 *iface, float offset_y)
{
    struct dcomp_visual *visual = impl_from_IDCompositionVisual2(iface);

    TRACE("iface %p, offset_y %.8e.\n", iface, offset_y);

    EnterCriticalSection(&visual->cs);
    visual->offset_y = offset_y;
    LeaveCriticalSection(&visual->cs);
    return S_OK;
}

static HRESULT WINAPI dcomp_visual_SetTransformObject(IDCompositionVisual2 *iface, IDCompositionTransform *transform)
{
    TRACE("iface %p, transform %p - ignored.\n", iface, transform);
    return S_OK;
}

static HRESULT WINAPI dcomp_visual_SetTransform(IDCompositionVisual2 *iface, const D2D_MATRIX_3X2_F *matrix)
{
    TRACE("iface %p, matrix %p - ignored.\n", iface, matrix);
    return S_OK;
}

static HRESULT WINAPI dcomp_visual_SetTransformParent(IDCompositionVisual2 *iface, IDCompositionVisual *visual)
{
    TRACE("iface %p, visual %p - ignored.\n", iface, visual);
    return S_OK;
}

static HRESULT WINAPI dcomp_visual_SetEffect(IDCompositionVisual2 *iface, IDCompositionEffect *effect)
{
    TRACE("iface %p, effect %p - ignored.\n", iface, effect);
    return S_OK;
}

static HRESULT WINAPI dcomp_visual_SetBitmapInterpolationMode(IDCompositionVisual2 *iface,
        enum DCOMPOSITION_BITMAP_INTERPOLATION_MODE interpolation_mode)
{
    TRACE("iface %p, interpolation_mode %#x - ignored.\n", iface, interpolation_mode);
    return S_OK;
}

static HRESULT WINAPI dcomp_visual_SetBorderMode(IDCompositionVisual2 *iface,
        enum DCOMPOSITION_BORDER_MODE border_mode)
{
    TRACE("iface %p, border_mode %#x - ignored.\n", iface, border_mode);
    return S_OK;
}

static HRESULT WINAPI dcomp_visual_SetClipObject(IDCompositionVisual2 *iface, IDCompositionClip *clip)
{
    TRACE("iface %p, clip %p - ignored.\n", iface, clip);
    return S_OK;
}

static HRESULT WINAPI dcomp_visual_SetClip(IDCompositionVisual2 *iface, const D2D_RECT_F *rect)
{
    TRACE("iface %p, rect %p - ignored.\n", iface, rect);
    return S_OK;
}

static HRESULT WINAPI dcomp_visual_SetContent(IDCompositionVisual2 *iface, IUnknown *content)
{
    struct dcomp_visual *visual = impl_from_IDCompositionVisual2(iface);

    TRACE("iface %p, content %p.\n", iface, content);

    EnterCriticalSection(&visual->cs);
    if (content) IUnknown_AddRef(content);
    if (visual->content) IUnknown_Release(visual->content);
    visual->content = content;
    LeaveCriticalSection(&visual->cs);
    return S_OK;
}

struct dcomp_visual *unsafe_impl_from_IDCompositionVisual(IDCompositionVisual *iface)
{
    extern const IDCompositionVisual2Vtbl dcomp_visual_vtbl_ref;
    if (!iface || iface->lpVtbl != (const IDCompositionVisualVtbl *)&dcomp_visual_vtbl_ref) return NULL;
    return CONTAINING_RECORD((IDCompositionVisual2 *)iface, struct dcomp_visual, IDCompositionVisual2_iface);
}

static HRESULT WINAPI dcomp_visual_AddVisual(IDCompositionVisual2 *iface, IDCompositionVisual *visual,
        BOOL insert_above, IDCompositionVisual *reference_visual)
{
    struct dcomp_visual *parent = impl_from_IDCompositionVisual2(iface);
    struct dcomp_visual *child_impl = unsafe_impl_from_IDCompositionVisual(visual);
    struct dcomp_visual *ref_impl = unsafe_impl_from_IDCompositionVisual(reference_visual);
    struct visual_child *child, *ref_entry = NULL, *iter;

    TRACE("iface %p, visual %p, insert_above %d, reference_visual %p.\n",
            iface, visual, insert_above, reference_visual);

    if (!child_impl)
    {
        WARN("Foreign visual %p, ignoring.\n", visual);
        return S_OK;
    }

    if (!(child = calloc(1, sizeof(*child)))) return E_OUTOFMEMORY;
    child->visual = child_impl;
    IDCompositionVisual2_AddRef(&child_impl->IDCompositionVisual2_iface);

    EnterCriticalSection(&parent->cs);
    if (ref_impl)
    {
        LIST_FOR_EACH_ENTRY(iter, &parent->children, struct visual_child, entry)
        {
            if (iter->visual == ref_impl)
            {
                ref_entry = iter;
                break;
            }
        }
    }
    /* children list is kept back-to-front; "above" means drawn later */
    if (ref_entry)
    {
        if (insert_above) list_add_after(&ref_entry->entry, &child->entry);
        else list_add_before(&ref_entry->entry, &child->entry);
    }
    else if (insert_above) list_add_tail(&parent->children, &child->entry);
    else list_add_head(&parent->children, &child->entry);
    LeaveCriticalSection(&parent->cs);
    return S_OK;
}

static HRESULT WINAPI dcomp_visual_RemoveVisual(IDCompositionVisual2 *iface, IDCompositionVisual *visual)
{
    struct dcomp_visual *parent = impl_from_IDCompositionVisual2(iface);
    struct dcomp_visual *child_impl = unsafe_impl_from_IDCompositionVisual(visual);
    struct visual_child *iter;
    HRESULT hr = S_FALSE;

    TRACE("iface %p, visual %p.\n", iface, visual);

    if (!child_impl) return S_OK;

    EnterCriticalSection(&parent->cs);
    LIST_FOR_EACH_ENTRY(iter, &parent->children, struct visual_child, entry)
    {
        if (iter->visual == child_impl)
        {
            list_remove(&iter->entry);
            IDCompositionVisual2_Release(&iter->visual->IDCompositionVisual2_iface);
            free(iter);
            hr = S_OK;
            break;
        }
    }
    LeaveCriticalSection(&parent->cs);
    return hr;
}

static HRESULT WINAPI dcomp_visual_RemoveAllVisuals(IDCompositionVisual2 *iface)
{
    struct dcomp_visual *visual = impl_from_IDCompositionVisual2(iface);

    TRACE("iface %p.\n", iface);

    EnterCriticalSection(&visual->cs);
    visual_clear_children(visual);
    LeaveCriticalSection(&visual->cs);
    return S_OK;
}

static HRESULT WINAPI dcomp_visual_SetCompositeMode(IDCompositionVisual2 *iface,
        enum DCOMPOSITION_COMPOSITE_MODE composite_mode)
{
    TRACE("iface %p, composite_mode %#x - ignored.\n", iface, composite_mode);
    return S_OK;
}

static HRESULT WINAPI dcomp_visual_SetOpacityMode(IDCompositionVisual2 *iface,
        enum DCOMPOSITION_OPACITY_MODE opacity_mode)
{
    TRACE("iface %p, opacity_mode %#x - ignored.\n", iface, opacity_mode);
    return S_OK;
}

static HRESULT WINAPI dcomp_visual_SetBackFaceVisibility(IDCompositionVisual2 *iface,
        enum DCOMPOSITION_BACKFACE_VISIBILITY visibility)
{
    TRACE("iface %p, visibility %#x - ignored.\n", iface, visibility);
    return S_OK;
}

const IDCompositionVisual2Vtbl dcomp_visual_vtbl_ref =
{
    dcomp_visual_QueryInterface,
    dcomp_visual_AddRef,
    dcomp_visual_Release,
    dcomp_visual_SetOffsetXAnimation,
    dcomp_visual_SetOffsetX,
    dcomp_visual_SetOffsetYAnimation,
    dcomp_visual_SetOffsetY,
    dcomp_visual_SetTransformObject,
    dcomp_visual_SetTransform,
    dcomp_visual_SetTransformParent,
    dcomp_visual_SetEffect,
    dcomp_visual_SetBitmapInterpolationMode,
    dcomp_visual_SetBorderMode,
    dcomp_visual_SetClipObject,
    dcomp_visual_SetClip,
    dcomp_visual_SetContent,
    dcomp_visual_AddVisual,
    dcomp_visual_RemoveVisual,
    dcomp_visual_RemoveAllVisuals,
    dcomp_visual_SetCompositeMode,
    dcomp_visual_SetOpacityMode,
    dcomp_visual_SetBackFaceVisibility,
};

HRESULT dcomp_visual_create(struct dcomp_visual **visual)
{
    struct dcomp_visual *object;

    if (!(object = calloc(1, sizeof(*object)))) return E_OUTOFMEMORY;
    object->IDCompositionVisual2_iface.lpVtbl = &dcomp_visual_vtbl_ref;
    object->refcount = 1;
    list_init(&object->children);
    InitializeCriticalSectionEx(&object->cs, 0, RTL_CRITICAL_SECTION_FLAG_FORCE_DEBUG_INFO);
    object->cs.DebugInfo->Spare[0] = (DWORD_PTR)(__FILE__ ": dcomp_visual.cs");

    TRACE("created visual %p.\n", object);

    *visual = object;
    return S_OK;
}

/* ------------------------------------------------------------------ */
/* Target */

static struct dcomp_target *impl_from_IDCompositionTarget(IDCompositionTarget *iface)
{
    return CONTAINING_RECORD(iface, struct dcomp_target, IDCompositionTarget_iface);
}

static HRESULT WINAPI dcomp_target_QueryInterface(IDCompositionTarget *iface, REFIID iid, void **out)
{
    TRACE("iface %p, iid %s, out %p.\n", iface, debugstr_guid(iid), out);

    if (IsEqualGUID(iid, &IID_IUnknown) || IsEqualGUID(iid, &IID_IDCompositionTarget))
    {
        IDCompositionTarget_AddRef(iface);
        *out = iface;
        return S_OK;
    }
    WARN("Unsupported interface %s.\n", debugstr_guid(iid));
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI dcomp_target_AddRef(IDCompositionTarget *iface)
{
    struct dcomp_target *target = impl_from_IDCompositionTarget(iface);
    return InterlockedIncrement(&target->refcount);
}

static ULONG WINAPI dcomp_target_Release(IDCompositionTarget *iface)
{
    struct dcomp_target *target = impl_from_IDCompositionTarget(iface);
    ULONG refcount = InterlockedDecrement(&target->refcount);

    if (!refcount)
    {
        pump_forget_target(target);
        EnterCriticalSection(&target->device->cs);
        list_remove(&target->entry);
        LeaveCriticalSection(&target->device->cs);
        if (target->root)
            IDCompositionVisual2_Release(&target->root->IDCompositionVisual2_iface);
        IDCompositionDesktopDevice_Release(&target->device->IDCompositionDesktopDevice_iface);
        free(target);
    }
    return refcount;
}

static HRESULT WINAPI dcomp_target_SetRoot(IDCompositionTarget *iface, IDCompositionVisual *visual)
{
    struct dcomp_target *target = impl_from_IDCompositionTarget(iface);
    struct dcomp_visual *root = unsafe_impl_from_IDCompositionVisual(visual);

    TRACE("iface %p, visual %p (impl %p).\n", iface, visual, root);

    if (visual && !root)
    {
        WARN("Foreign visual %p.\n", visual);
        return E_INVALIDARG;
    }

    EnterCriticalSection(&target->device->cs);
    if (root) IDCompositionVisual2_AddRef(&root->IDCompositionVisual2_iface);
    if (target->root) IDCompositionVisual2_Release(&target->root->IDCompositionVisual2_iface);
    target->root = root;
    LeaveCriticalSection(&target->device->cs);
    return S_OK;
}

static const IDCompositionTargetVtbl dcomp_target_vtbl =
{
    dcomp_target_QueryInterface,
    dcomp_target_AddRef,
    dcomp_target_Release,
    dcomp_target_SetRoot,
};

HRESULT dcomp_target_create(struct dcomp_device *device, HWND hwnd, BOOL topmost,
        struct dcomp_target **target)
{
    struct dcomp_target *object;

    if (!(object = calloc(1, sizeof(*object)))) return E_OUTOFMEMORY;
    object->IDCompositionTarget_iface.lpVtbl = &dcomp_target_vtbl;
    object->refcount = 1;
    object->hwnd = hwnd;
    object->topmost = topmost;
    object->device = device;
    IDCompositionDesktopDevice_AddRef(&device->IDCompositionDesktopDevice_iface);

    EnterCriticalSection(&device->cs);
    list_add_tail(&device->targets, &object->entry);
    LeaveCriticalSection(&device->cs);

    TRACE("created target %p for hwnd %p.\n", object, hwnd);

    *target = object;
    return S_OK;
}

/* ------------------------------------------------------------------ */
/* Surface */

static struct dcomp_surface *impl_from_IDCompositionVirtualSurface(IDCompositionVirtualSurface *iface)
{
    return CONTAINING_RECORD(iface, struct dcomp_surface, IDCompositionVirtualSurface_iface);
}

extern const IDCompositionVirtualSurfaceVtbl dcomp_surface_vtbl_ref;

struct dcomp_surface *unsafe_impl_from_content(IUnknown *content)
{
    if (!content || ((IDCompositionVirtualSurface *)content)->lpVtbl != &dcomp_surface_vtbl_ref)
        return NULL;
    return impl_from_IDCompositionVirtualSurface((IDCompositionVirtualSurface *)content);
}

static HRESULT WINAPI dcomp_surface_QueryInterface(IDCompositionVirtualSurface *iface, REFIID iid, void **out)
{
    TRACE("iface %p, iid %s, out %p.\n", iface, debugstr_guid(iid), out);

    if (IsEqualGUID(iid, &IID_IUnknown)
            || IsEqualGUID(iid, &IID_IDCompositionSurface)
            || IsEqualGUID(iid, &IID_IDCompositionVirtualSurface))
    {
        IDCompositionVirtualSurface_AddRef(iface);
        *out = iface;
        return S_OK;
    }
    WARN("Unsupported interface %s.\n", debugstr_guid(iid));
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI dcomp_surface_AddRef(IDCompositionVirtualSurface *iface)
{
    struct dcomp_surface *surface = impl_from_IDCompositionVirtualSurface(iface);
    return InterlockedIncrement(&surface->refcount);
}

static ULONG WINAPI dcomp_surface_Release(IDCompositionVirtualSurface *iface)
{
    struct dcomp_surface *surface = impl_from_IDCompositionVirtualSurface(iface);
    ULONG refcount = InterlockedDecrement(&surface->refcount);

    if (!refcount)
    {
        if (surface->texture) ID3D11Texture2D_Release(surface->texture);
        free(surface);
    }
    return refcount;
}

static HRESULT WINAPI dcomp_surface_BeginDraw(IDCompositionVirtualSurface *iface, const RECT *rect,
        REFIID iid, void **object, POINT *offset)
{
    struct dcomp_surface *surface = impl_from_IDCompositionVirtualSurface(iface);
    HRESULT hr;

    TRACE("iface %p, rect %s, iid %s, object %p, offset %p.\n",
            iface, wine_dbgstr_rect(rect), debugstr_guid(iid), object, offset);

    if (!object || !offset) return E_POINTER;
    if (!surface->texture)
    {
        WARN("No backing texture.\n");
        return E_FAIL;
    }
    if (surface->drawing) return E_UNEXPECTED;

    if (FAILED(hr = ID3D11Texture2D_QueryInterface(surface->texture, iid, object)))
    {
        WARN("Backing texture does not expose %s, hr %#lx.\n", debugstr_guid(iid), hr);
        return hr;
    }

    offset->x = rect ? rect->left : 0;
    offset->y = rect ? rect->top : 0;
    surface->drawing = TRUE;
    return S_OK;
}

static HRESULT WINAPI dcomp_surface_EndDraw(IDCompositionVirtualSurface *iface)
{
    struct dcomp_surface *surface = impl_from_IDCompositionVirtualSurface(iface);

    TRACE("iface %p.\n", iface);

    surface->drawing = FALSE;
    return S_OK;
}

static HRESULT WINAPI dcomp_surface_SuspendDraw(IDCompositionVirtualSurface *iface)
{
    TRACE("iface %p.\n", iface);
    return S_OK;
}

static HRESULT WINAPI dcomp_surface_ResumeDraw(IDCompositionVirtualSurface *iface)
{
    TRACE("iface %p.\n", iface);
    return S_OK;
}

static HRESULT WINAPI dcomp_surface_Scroll(IDCompositionVirtualSurface *iface, const RECT *scroll,
        const RECT *clip, int offset_x, int offset_y)
{
    TRACE("iface %p, scroll %s, clip %s, offset_x %d, offset_y %d - ignored.\n",
            iface, wine_dbgstr_rect(scroll), wine_dbgstr_rect(clip), offset_x, offset_y);
    return S_OK;
}

static HRESULT WINAPI dcomp_surface_Resize(IDCompositionVirtualSurface *iface, UINT width, UINT height)
{
    struct dcomp_surface *surface = impl_from_IDCompositionVirtualSurface(iface);

    FIXME("iface %p, width %u, height %u - resizing backing texture not implemented.\n", iface, width, height);

    surface->width = width;
    surface->height = height;
    return S_OK;
}

static HRESULT WINAPI dcomp_surface_Trim(IDCompositionVirtualSurface *iface, const RECT *rectangles, UINT count)
{
    TRACE("iface %p, rectangles %p, count %u - ignored.\n", iface, rectangles, count);
    return S_OK;
}

const IDCompositionVirtualSurfaceVtbl dcomp_surface_vtbl_ref =
{
    dcomp_surface_QueryInterface,
    dcomp_surface_AddRef,
    dcomp_surface_Release,
    dcomp_surface_BeginDraw,
    dcomp_surface_EndDraw,
    dcomp_surface_SuspendDraw,
    dcomp_surface_ResumeDraw,
    dcomp_surface_Scroll,
    dcomp_surface_Resize,
    dcomp_surface_Trim,
};

static ID3D11Device *get_fallback_d3d_device(void)
{
    static ID3D11Device *fallback_device;
    HRESULT (WINAPI *create)(IDXGIAdapter *, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL *,
            UINT, UINT, ID3D11Device **, D3D_FEATURE_LEVEL *, ID3D11DeviceContext **);
    ID3D11Device *device = NULL;
    HMODULE d3d11;

    if (fallback_device) return fallback_device;

    if (!(d3d11 = LoadLibraryW(L"d3d11.dll"))) return NULL;
    if (!(create = (void *)GetProcAddress(d3d11, "D3D11CreateDevice"))) return NULL;

    if (FAILED(create(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
            NULL, 0, D3D11_SDK_VERSION, &device, NULL, NULL))
            && FAILED(create(NULL, D3D_DRIVER_TYPE_WARP, NULL, 0, NULL, 0, D3D11_SDK_VERSION,
            &device, NULL, NULL)))
        return NULL;

    if (InterlockedCompareExchangePointer((void **)&fallback_device, device, NULL))
        ID3D11Device_Release(device);
    return fallback_device;
}

HRESULT dcomp_surface_create(struct dcomp_device *device, UINT width, UINT height,
        DXGI_FORMAT format, DXGI_ALPHA_MODE alpha_mode, BOOL virtual_surface,
        struct dcomp_surface **surface)
{
    struct dcomp_surface *object;
    ID3D11Device *d3d_device;
    D3D11_TEXTURE2D_DESC desc = {0};
    HRESULT hr;

    if (!(object = calloc(1, sizeof(*object)))) return E_OUTOFMEMORY;
    object->IDCompositionVirtualSurface_iface.lpVtbl = &dcomp_surface_vtbl_ref;
    object->refcount = 1;
    object->width = width;
    object->height = height;
    object->format = format;
    object->alpha_mode = alpha_mode;

    d3d_device = device->d3d_device ? device->d3d_device : get_fallback_d3d_device();
    if (d3d_device && width && height)
    {
        desc.Width = width;
        desc.Height = height;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = format ? format : DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

        if (FAILED(hr = ID3D11Device_CreateTexture2D(d3d_device, &desc, NULL, &object->texture)))
        {
            WARN("Failed to create backing texture, hr %#lx.\n", hr);
            object->texture = NULL;
        }
    }

    TRACE("created %ssurface %p, %ux%u, texture %p.\n",
            virtual_surface ? "virtual " : "", object, width, height, object->texture);

    *surface = object;
    return S_OK;
}

/* ------------------------------------------------------------------ */
/* Surface factory */

struct dcomp_surface_factory
{
    IDCompositionSurfaceFactory IDCompositionSurfaceFactory_iface;
    LONG refcount;
    struct dcomp_device *device; /* referenced */
};

static struct dcomp_surface_factory *impl_from_IDCompositionSurfaceFactory(IDCompositionSurfaceFactory *iface)
{
    return CONTAINING_RECORD(iface, struct dcomp_surface_factory, IDCompositionSurfaceFactory_iface);
}

static HRESULT WINAPI dcomp_surface_factory_QueryInterface(IDCompositionSurfaceFactory *iface, REFIID iid, void **out)
{
    TRACE("iface %p, iid %s, out %p.\n", iface, debugstr_guid(iid), out);

    if (IsEqualGUID(iid, &IID_IUnknown) || IsEqualGUID(iid, &IID_IDCompositionSurfaceFactory))
    {
        IDCompositionSurfaceFactory_AddRef(iface);
        *out = iface;
        return S_OK;
    }
    WARN("Unsupported interface %s.\n", debugstr_guid(iid));
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI dcomp_surface_factory_AddRef(IDCompositionSurfaceFactory *iface)
{
    struct dcomp_surface_factory *factory = impl_from_IDCompositionSurfaceFactory(iface);
    return InterlockedIncrement(&factory->refcount);
}

static ULONG WINAPI dcomp_surface_factory_Release(IDCompositionSurfaceFactory *iface)
{
    struct dcomp_surface_factory *factory = impl_from_IDCompositionSurfaceFactory(iface);
    ULONG refcount = InterlockedDecrement(&factory->refcount);

    if (!refcount)
    {
        IDCompositionDesktopDevice_Release(&factory->device->IDCompositionDesktopDevice_iface);
        free(factory);
    }
    return refcount;
}

static HRESULT WINAPI dcomp_surface_factory_CreateSurface(IDCompositionSurfaceFactory *iface, UINT width,
        UINT height, DXGI_FORMAT pixel_format, DXGI_ALPHA_MODE alpha_mode, IDCompositionSurface **surface)
{
    struct dcomp_surface_factory *factory = impl_from_IDCompositionSurfaceFactory(iface);
    struct dcomp_surface *object;
    HRESULT hr;

    TRACE("iface %p, width %u, height %u, format %#x, alpha_mode %#x, surface %p.\n",
            iface, width, height, pixel_format, alpha_mode, surface);

    if (!surface) return E_POINTER;
    if (FAILED(hr = dcomp_surface_create(factory->device, width, height, pixel_format, alpha_mode,
            FALSE, &object)))
        return hr;
    *surface = (IDCompositionSurface *)&object->IDCompositionVirtualSurface_iface;
    return S_OK;
}

static HRESULT WINAPI dcomp_surface_factory_CreateVirtualSurface(IDCompositionSurfaceFactory *iface, UINT width,
        UINT height, DXGI_FORMAT pixel_format, DXGI_ALPHA_MODE alpha_mode, IDCompositionVirtualSurface **surface)
{
    struct dcomp_surface_factory *factory = impl_from_IDCompositionSurfaceFactory(iface);
    struct dcomp_surface *object;
    HRESULT hr;

    TRACE("iface %p, width %u, height %u, format %#x, alpha_mode %#x, surface %p.\n",
            iface, width, height, pixel_format, alpha_mode, surface);

    if (!surface) return E_POINTER;
    if (FAILED(hr = dcomp_surface_create(factory->device, width, height, pixel_format, alpha_mode,
            TRUE, &object)))
        return hr;
    *surface = &object->IDCompositionVirtualSurface_iface;
    return S_OK;
}

static const IDCompositionSurfaceFactoryVtbl dcomp_surface_factory_vtbl =
{
    dcomp_surface_factory_QueryInterface,
    dcomp_surface_factory_AddRef,
    dcomp_surface_factory_Release,
    dcomp_surface_factory_CreateSurface,
    dcomp_surface_factory_CreateVirtualSurface,
};

HRESULT dcomp_surface_factory_create(struct dcomp_device *device, IUnknown *rendering_device,
        IDCompositionSurfaceFactory **factory)
{
    struct dcomp_surface_factory *object;

    if (!(object = calloc(1, sizeof(*object)))) return E_OUTOFMEMORY;
    object->IDCompositionSurfaceFactory_iface.lpVtbl = &dcomp_surface_factory_vtbl;
    object->refcount = 1;
    object->device = device;
    IDCompositionDesktopDevice_AddRef(&device->IDCompositionDesktopDevice_iface);

    TRACE("created surface factory %p.\n", object);

    *factory = &object->IDCompositionSurfaceFactory_iface;
    return S_OK;
}
