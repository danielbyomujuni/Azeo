/*
 * DirectComposition device
 *
 * Copyright 2020 Nikolay Sivov for CodeWeavers
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

#include <stdarg.h>
#include "windef.h"
#include "initguid.h"
#include "dcomp_private.h"
#include "winuser.h"
#include "wingdi.h"

WINE_DEFAULT_DEBUG_CHANNEL(dcomp);

/* Shared with dlls/dxgi/composition.c: QI on a composition swapchain for this
 * IID yields a LONG* that is non-zero while its back buffer holds presented
 * content.  No reference is added. */
static const GUID IID_wine_composition_content_valid =
        {0x9f002c3a, 0x5c2d, 0x4f42, {0x91, 0x64, 0x7c, 0x2e, 0xd1, 0x8e, 0x1d, 0xab}};
static const GUID IID_wine_composition_front_buffer =
        {0x1b8aa5c1, 0x33f1, 0x4d6a, {0xb0, 0x62, 0x50, 0x0f, 0x8a, 0x9e, 0x44, 0x21}};

/* ------------------------------------------------------------------ */
/* Present pump.
 *
 * There is no real compositor behind this implementation.  Instead, every
 * device Commit() snapshots each target's visual tree into a flat draw list,
 * and a background thread copies the current contents of each visual's
 * content (swapchain backbuffer or composition surface texture) into the
 * target HWND with GDI at ~30fps.  Crude, but it turns a committed visual
 * tree into visible pixels, which is all Chromium needs from us.
 */

struct draw_item
{
    IUnknown *content;   /* referenced */
    float x, y;
};

struct pump_target
{
    struct list entry;
    struct dcomp_target *target; /* NOT referenced - unregistered on destroy */
    HWND hwnd;
    struct draw_item *items;
    unsigned int item_count;
};

static CRITICAL_SECTION pump_cs;
static CRITICAL_SECTION_DEBUG pump_cs_debug =
{
    0, 0, &pump_cs,
    { &pump_cs_debug.ProcessLocksList, &pump_cs_debug.ProcessLocksList },
      0, 0, { (DWORD_PTR)(__FILE__ ": pump_cs") }
};
static CRITICAL_SECTION pump_cs = { &pump_cs_debug, -1, 0, 0, 0, 0 };
static struct list pump_targets = LIST_INIT(pump_targets);
static HANDLE pump_thread;
static HANDLE pump_wake_event;

void pump_wake(void)
{
    if (pump_wake_event) SetEvent(pump_wake_event);
}

static void draw_items_free(struct draw_item *items, unsigned int count)
{
    unsigned int i;
    for (i = 0; i < count; ++i)
        IUnknown_Release(items[i].content);
    free(items);
}

static void blit_texture_to_window(HWND hwnd, ID3D11Texture2D *texture, int dst_x, int dst_y)
{
    D3D11_TEXTURE2D_DESC desc, staging_desc;
    ID3D11DeviceContext *context = NULL;
    ID3D11Texture2D *staging = NULL;
    D3D11_MAPPED_SUBRESOURCE map;
    ID3D11Device *device = NULL;
    BITMAPINFO bmi = {0};
    HDC hdc;

    ID3D11Texture2D_GetDesc(texture, &desc);
    if (!desc.Width || !desc.Height) return;
    if (desc.Format != DXGI_FORMAT_B8G8R8A8_UNORM && desc.Format != DXGI_FORMAT_R8G8B8A8_UNORM
            && desc.Format != DXGI_FORMAT_B8G8R8X8_UNORM)
    {
        WARN("Unhandled format %#x.\n", desc.Format);
        return;
    }

    ID3D11Texture2D_GetDevice(texture, &device);
    ID3D11Device_GetImmediateContext(device, &context);

    staging_desc = desc;
    staging_desc.Usage = D3D11_USAGE_STAGING;
    staging_desc.BindFlags = 0;
    staging_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    staging_desc.MiscFlags = 0;
    staging_desc.MipLevels = 1;
    staging_desc.ArraySize = 1;
    staging_desc.SampleDesc.Count = 1;
    staging_desc.SampleDesc.Quality = 0;

    if (FAILED(ID3D11Device_CreateTexture2D(device, &staging_desc, NULL, &staging)))
        goto done;

    ID3D11DeviceContext_CopySubresourceRegion(context, (ID3D11Resource *)staging, 0, 0, 0, 0,
            (ID3D11Resource *)texture, 0, NULL);

    if (FAILED(ID3D11DeviceContext_Map(context, (ID3D11Resource *)staging, 0, D3D11_MAP_READ, 0, &map)))
        goto done;

    bmi.bmiHeader.biSize = sizeof(bmi.bmiHeader);
    bmi.bmiHeader.biHeight = -(LONG)desc.Height;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    if ((hdc = GetDC(hwnd)))
    {
        if (desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM)
        {
            /* swizzle to BGRA in a scratch buffer; the READ mapping is not ours to write */
            unsigned char *buf = malloc((size_t)desc.Width * desc.Height * 4);
            if (buf)
            {
                unsigned int x, y;
                for (y = 0; y < desc.Height; ++y)
                {
                    const unsigned char *src = (const unsigned char *)map.pData + (size_t)y * map.RowPitch;
                    unsigned char *dst = buf + (size_t)y * desc.Width * 4;
                    for (x = 0; x < desc.Width; ++x)
                    {
                        dst[x * 4] = src[x * 4 + 2];
                        dst[x * 4 + 1] = src[x * 4 + 1];
                        dst[x * 4 + 2] = src[x * 4];
                        dst[x * 4 + 3] = src[x * 4 + 3];
                    }
                }
                bmi.bmiHeader.biWidth = desc.Width;
                StretchDIBits(hdc, dst_x, dst_y, desc.Width, desc.Height,
                        0, 0, desc.Width, desc.Height, buf, &bmi, DIB_RGB_COLORS, SRCCOPY);
                free(buf);
            }
        }
        else
        {
            bmi.bmiHeader.biWidth = map.RowPitch / 4;
            StretchDIBits(hdc, dst_x, dst_y, desc.Width, desc.Height,
                    0, 0, desc.Width, desc.Height, map.pData, &bmi, DIB_RGB_COLORS, SRCCOPY);
        }
        ReleaseDC(hwnd, hdc);
    }

    ID3D11DeviceContext_Unmap(context, (ID3D11Resource *)staging, 0);

done:
    if (staging) ID3D11Texture2D_Release(staging);
    if (context) ID3D11DeviceContext_Release(context);
    if (device) ID3D11Device_Release(device);
}

static void pump_draw_item(HWND hwnd, const struct draw_item *item)
{
    struct dcomp_surface *surface;
    IDXGISwapChain *swapchain;
    ID3D11Texture2D *texture;

    if (SUCCEEDED(IUnknown_QueryInterface(item->content, &IID_IDXGISwapChain, (void **)&swapchain)))
    {
        /* Prefer the wrapper's copy of the last presented frame; buffer 0 is
         * the NEXT buffer to be drawn in the flip model and may be blank.
         * The copy survives ResizeBuffers, so the last good frame keeps
         * being painted until a new one is presented - a compositor never
         * paints "nothing". */
        if (SUCCEEDED(IUnknown_QueryInterface(item->content,
                &IID_wine_composition_front_buffer, (void **)&texture)))
        {
            if (TRACE_ON(dcomp))
            {
                static ULONGLONG last_item_dump;
                ULONGLONG now = GetTickCount64();
                LONG *state;
                if (now - last_item_dump >= 1000 && SUCCEEDED(IUnknown_QueryInterface(item->content,
                        &IID_wine_composition_content_valid, (void **)&state)))
                {
                    D3D11_TEXTURE2D_DESC tdesc;
                    ID3D11Texture2D_GetDesc(texture, &tdesc);
                    last_item_dump = now;
                    TRACE("PUMPSTATE   item %p on hwnd %p at %d,%d size %ux%u valid %ld presents %ld\n",
                            item->content, hwnd, (int)item->x, (int)item->y,
                            tdesc.Width, tdesc.Height, state[0], state[1]);
                }
            }
            blit_texture_to_window(hwnd, texture, (int)item->x, (int)item->y);
            ID3D11Texture2D_Release(texture);
            IDXGISwapChain_Release(swapchain);
            return;
        }
        if (SUCCEEDED(IDXGISwapChain_GetBuffer(swapchain, 0, &IID_ID3D11Texture2D, (void **)&texture)))
        {
            blit_texture_to_window(hwnd, texture, (int)item->x, (int)item->y);
            ID3D11Texture2D_Release(texture);
        }
        IDXGISwapChain_Release(swapchain);
        return;
    }

    if ((surface = unsafe_impl_from_content(item->content)))
    {
        /* A surface that has not been drawn into since it was (re)created
         * holds no meaningful pixels; blitting it flashes garbage during
         * resizes. Keep whatever is on screen until content arrives. */
        if (!surface->written)
            return;
        if ((texture = dcomp_surface_get_texture(surface)))
        {
            blit_texture_to_window(hwnd, texture, (int)item->x, (int)item->y);
            ID3D11Texture2D_Release(texture);
        }
        return;
    }
}

struct blit_snapshot
{
    HWND hwnd;
    struct draw_item *items;
    unsigned int count;
};

/* Overlay windows: the composition content of a target is shown in its own
 * click-through popup owned by the target's root window, stacked above it -
 * the same layering the system compositor provides on Windows.  Painting
 * into the application's windows directly cannot work: with visual hosting
 * the app legitimately paints its own (white) background under the content,
 * and two writers on one surface flicker forever. */
struct pump_overlay
{
    struct list entry;
    HWND target;   /* composition target window (key) */
    HWND overlay;
    HWND owner;
};
static struct list pump_overlays = LIST_INIT(pump_overlays);   /* pump thread only */
static ATOM overlay_class;

static void pump_register_overlay_class(void)
{
    WNDCLASSW wc = {0};
    if (overlay_class) return;
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.lpszClassName = L"WineDCompOverlay";
    overlay_class = RegisterClassW(&wc);
}

static struct pump_overlay *pump_get_overlay(HWND target, HWND owner)
{
    struct pump_overlay *o;

    LIST_FOR_EACH_ENTRY(o, &pump_overlays, struct pump_overlay, entry)
        if (o->target == target) return o;

    pump_register_overlay_class();
    if (!(o = calloc(1, sizeof(*o)))) return NULL;
    o->target = target;
    o->owner = owner;
    o->overlay = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT,
            L"WineDCompOverlay", NULL, WS_POPUP, 0, 0, 1, 1, owner, NULL,
            GetModuleHandleW(NULL), NULL);
    if (!o->overlay)
    {
        free(o);
        return NULL;
    }
    TRACE("Created overlay %p for target %p (owner %p).\n", o->overlay, target, owner);
    list_add_tail(&pump_overlays, &o->entry);
    return o;
}

static void pump_prune_overlays(const struct blit_snapshot *snap, unsigned int snap_count)
{
    struct pump_overlay *o, *next;
    unsigned int j;

    LIST_FOR_EACH_ENTRY_SAFE(o, next, &pump_overlays, struct pump_overlay, entry)
    {
        BOOL found = FALSE;
        for (j = 0; j < snap_count; ++j)
            if (snap[j].hwnd == o->target) found = TRUE;
        if (!found || !IsWindow(o->target))
        {
            TRACE("Destroying overlay %p (target %p gone).\n", o->overlay, o->target);
            DestroyWindow(o->overlay);
            list_remove(&o->entry);
            free(o);
        }
    }
}

static DWORD WINAPI pump_thread_proc(void *arg)
{
    struct pump_target *t;

    TRACE("Present pump started.\n");

    for (;;)
    {
        struct blit_snapshot *snap = NULL;
        unsigned int snap_count = 0, snap_cap = 0, i, j;

        /* Snapshot the draw lists under the lock - hold it for microseconds,
         * never for the duration of a blit, or Chromium's compositor thread
         * stalls behind Commit() and its hang watchdog kills the process. */
        EnterCriticalSection(&pump_cs);
        LIST_FOR_EACH_ENTRY(t, &pump_targets, struct pump_target, entry)
        {
            if (!t->item_count) continue;
            if (snap_count == snap_cap)
            {
                unsigned int new_cap = snap_cap ? snap_cap * 2 : 4;
                struct blit_snapshot *new_snap = realloc(snap, new_cap * sizeof(*snap));
                if (!new_snap) break;
                snap = new_snap;
                snap_cap = new_cap;
            }
            snap[snap_count].items = malloc(t->item_count * sizeof(*t->items));
            if (!snap[snap_count].items) continue;
            snap[snap_count].hwnd = t->hwnd;
            snap[snap_count].count = t->item_count;
            for (i = 0; i < t->item_count; ++i)
            {
                snap[snap_count].items[i] = t->items[i];
                IUnknown_AddRef(snap[snap_count].items[i].content);
            }
            snap_count++;
        }
        LeaveCriticalSection(&pump_cs);

        pump_prune_overlays(snap, snap_count);

        for (j = 0; j < snap_count; ++j)
        {
            HWND shown = snap[j].hwnd;
            struct pump_overlay *o;
            RECT r;

            if (!IsWindow(snap[j].hwnd)) goto next_target;

            /* Climb past never-shown intermediate windows; the first
             * WS_VISIBLE ancestor tells us whether this content should be
             * on screen at all. */
            while (shown && !(GetWindowLongW(shown, GWL_STYLE) & WS_VISIBLE))
                shown = GetParent(shown);

            if (!(o = pump_get_overlay(snap[j].hwnd, GetAncestor(snap[j].hwnd, GA_ROOT))))
                goto next_target;

            if (!shown || !IsWindowVisible(shown) || IsIconic(o->owner))
            {
                ShowWindow(o->overlay, SW_HIDE);
                goto next_target;
            }

            GetWindowRect(snap[j].hwnd, &r);
            if (r.right <= r.left || r.bottom <= r.top)
            {
                ShowWindow(o->overlay, SW_HIDE);
                goto next_target;
            }

            SetWindowPos(o->overlay, NULL, r.left, r.top, r.right - r.left, r.bottom - r.top,
                    SWP_NOACTIVATE | SWP_NOZORDER | SWP_SHOWWINDOW);

            for (i = 0; i < snap[j].count; ++i)
                pump_draw_item(o->overlay, &snap[j].items[i]);

        next_target:
            draw_items_free(snap[j].items, snap[j].count);
        }

        if (TRACE_ON(dcomp))
        {
            static ULONGLONG last_dump;
            ULONGLONG now = GetTickCount64();
            if (now - last_dump >= 1000)
            {
                last_dump = now;
                for (j = 0; j < snap_count; ++j)
                {
                    HWND root = GetAncestor(snap[j].hwnd, GA_ROOT);
                    RECT r = {0};
                    GetWindowRect(snap[j].hwnd, &r);
                    HWND pw = snap[j].hwnd;
                    while (pw && !(GetWindowLongW(pw, GWL_STYLE) & WS_VISIBLE)) pw = GetParent(pw);
                    TRACE("PUMPSTATE target hwnd %p vis %d rect %s items %u root %p rootvis %d paint %p paintvis %d\n",
                            snap[j].hwnd, IsWindowVisible(snap[j].hwnd), wine_dbgstr_rect(&r),
                            snap[j].count, root, root ? IsWindowVisible(root) : 0,
                            pw, pw ? IsWindowVisible(pw) : 0);
                }
            }
        }
        free(snap);

        /* Wake immediately when new content lands (EndDraw/Commit), fall
         * back to a steady tick for swapchain content we cannot observe.
         * The pump thread owns the overlay windows, so their messages are
         * drained here too. */
        if (MsgWaitForMultipleObjects(1, &pump_wake_event, FALSE, 16, QS_ALLINPUT) == WAIT_OBJECT_0 + 1)
        {
            MSG msg;
            while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE))
            {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
        }
    }
    return 0;
}

static void pump_ensure_thread(void)
{
    if (!pump_wake_event)
        pump_wake_event = CreateEventW(NULL, FALSE, FALSE, NULL);
    if (!pump_thread)
        pump_thread = CreateThread(NULL, 0, pump_thread_proc, NULL, 0, NULL);
}

/* Collect (content, x, y) items from a visual subtree, back to front. */
static void collect_visual(struct dcomp_visual *visual, float x, float y,
                           struct draw_item **items, unsigned int *count, unsigned int *capacity)
{
    struct visual_child *child;

    EnterCriticalSection(&visual->cs);
    x += visual->offset_x;
    y += visual->offset_y;

    if (visual->content)
    {
        if (*count == *capacity)
        {
            unsigned int new_cap = *capacity ? *capacity * 2 : 8;
            struct draw_item *new_items = realloc(*items, new_cap * sizeof(**items));
            if (!new_items)
            {
                LeaveCriticalSection(&visual->cs);
                return;
            }
            *items = new_items;
            *capacity = new_cap;
        }
        (*items)[*count].content = visual->content;
        IUnknown_AddRef(visual->content);
        (*items)[*count].x = x;
        (*items)[*count].y = y;
        (*count)++;
    }

    LIST_FOR_EACH_ENTRY(child, &visual->children, struct visual_child, entry)
        collect_visual(child->visual, x, y, items, count, capacity);
    LeaveCriticalSection(&visual->cs);
}

void pump_commit_device(struct dcomp_device *device)
{
    struct dcomp_target *target;

    EnterCriticalSection(&device->cs);
    LIST_FOR_EACH_ENTRY(target, &device->targets, struct dcomp_target, entry)
    {
        struct draw_item *items = NULL;
        unsigned int count = 0, capacity = 0;
        struct pump_target *t, *found = NULL;

        if (target->root)
            collect_visual(target->root, 0.0f, 0.0f, &items, &count, &capacity);

        EnterCriticalSection(&pump_cs);
        LIST_FOR_EACH_ENTRY(t, &pump_targets, struct pump_target, entry)
        {
            if (t->target == target)
            {
                found = t;
                break;
            }
        }
        if (!found)
        {
            if ((found = calloc(1, sizeof(*found))))
            {
                found->target = target;
                found->hwnd = target->hwnd;
                list_add_tail(&pump_targets, &found->entry);
            }
        }
        if (found)
        {
            draw_items_free(found->items, found->item_count);
            found->items = items;
            found->item_count = count;
            TRACE("target %p (hwnd %p): %u draw item(s).\n", target, target->hwnd, count);
        }
        else draw_items_free(items, count);

        if (count) pump_ensure_thread();
        LeaveCriticalSection(&pump_cs);
    }
    LeaveCriticalSection(&device->cs);
}

void pump_forget_target(struct dcomp_target *target)
{
    struct pump_target *t;

    EnterCriticalSection(&pump_cs);
    LIST_FOR_EACH_ENTRY(t, &pump_targets, struct pump_target, entry)
    {
        if (t->target == target)
        {
            list_remove(&t->entry);
            draw_items_free(t->items, t->item_count);
            free(t);
            break;
        }
    }
    LeaveCriticalSection(&pump_cs);
}

/* ------------------------------------------------------------------ */
/* Device object */

static inline struct dcomp_device *impl_from_IDCompositionDesktopDevice(IDCompositionDesktopDevice *iface)
{
    return CONTAINING_RECORD(iface, struct dcomp_device, IDCompositionDesktopDevice_iface);
}

static inline struct dcomp_device *impl_from_IDCompositionDevice(IDCompositionDevice *iface)
{
    return CONTAINING_RECORD(iface, struct dcomp_device, IDCompositionDevice_iface);
}

static inline struct dcomp_device *impl_from_IDCompositionDevice3(IDCompositionDevice3 *iface)
{
    return CONTAINING_RECORD(iface, struct dcomp_device, IDCompositionDevice3_iface);
}

static HRESULT WINAPI dcomp_device_QueryInterface(IDCompositionDesktopDevice *iface, REFIID iid, void **out)
{
    struct dcomp_device *device = impl_from_IDCompositionDesktopDevice(iface);

    TRACE("iface %p, iid %s, out %p.\n", iface, debugstr_guid(iid), out);

    if (IsEqualGUID(iid, &IID_IUnknown)
            || IsEqualGUID(iid, &IID_IDCompositionDevice2)
            || IsEqualGUID(iid, &IID_IDCompositionDesktopDevice))
        *out = &device->IDCompositionDesktopDevice_iface;
    else if (IsEqualGUID(iid, &IID_IDCompositionDevice))
        *out = &device->IDCompositionDevice_iface;
    else if (IsEqualGUID(iid, &IID_IDCompositionDevice3))
        *out = &device->IDCompositionDevice3_iface;
    else
    {
        WARN("Unsupported interface %s.\n", debugstr_guid(iid));
        *out = NULL;
        return E_NOINTERFACE;
    }

    IUnknown_AddRef((IUnknown *)*out);
    return S_OK;
}

static ULONG WINAPI dcomp_device_AddRef(IDCompositionDesktopDevice *iface)
{
    struct dcomp_device *device = impl_from_IDCompositionDesktopDevice(iface);
    ULONG refcount = InterlockedIncrement(&device->refcount);
    TRACE("%p increasing refcount to %lu.\n", device, refcount);
    return refcount;
}

static ULONG WINAPI dcomp_device_Release(IDCompositionDesktopDevice *iface)
{
    struct dcomp_device *device = impl_from_IDCompositionDesktopDevice(iface);
    ULONG refcount = InterlockedDecrement(&device->refcount);

    TRACE("%p decreasing refcount to %lu.\n", device, refcount);

    if (!refcount)
    {
        if (device->d3d_device) ID3D11Device_Release(device->d3d_device);
        if (device->rendering_device) IUnknown_Release(device->rendering_device);
        device->cs.DebugInfo->Spare[0] = 0;
        DeleteCriticalSection(&device->cs);
        free(device);
    }
    return refcount;
}

static HRESULT WINAPI dcomp_device_Commit(IDCompositionDesktopDevice *iface)
{
    struct dcomp_device *device = impl_from_IDCompositionDesktopDevice(iface);

    TRACE("iface %p.\n", iface);

    pump_commit_device(device);
    return S_OK;
}

static HRESULT WINAPI dcomp_device_WaitForCommitCompletion(IDCompositionDesktopDevice *iface)
{
    TRACE("iface %p.\n", iface);
    return S_OK;
}

static HRESULT WINAPI dcomp_device_GetFrameStatistics(IDCompositionDesktopDevice *iface,
        DCOMPOSITION_FRAME_STATISTICS *statistics)
{
    LARGE_INTEGER now, freq;

    TRACE("iface %p, statistics %p.\n", iface, statistics);

    if (!statistics) return E_POINTER;

    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&freq);

    statistics->lastFrameTime = now;
    statistics->currentCompositionRate.Numerator = 60;
    statistics->currentCompositionRate.Denominator = 1;
    statistics->currentTime = now;
    statistics->timeFrequency = freq;
    statistics->nextEstimatedFrameTime.QuadPart = now.QuadPart + freq.QuadPart / 60;
    return S_OK;
}

static HRESULT WINAPI dcomp_device_CreateVisual(IDCompositionDesktopDevice *iface, IDCompositionVisual2 **visual)
{
    struct dcomp_visual *object;
    HRESULT hr;

    TRACE("iface %p, visual %p.\n", iface, visual);

    if (!visual) return E_POINTER;
    if (FAILED(hr = dcomp_visual_create(&object))) return hr;
    *visual = &object->IDCompositionVisual2_iface;
    return S_OK;
}

static HRESULT WINAPI dcomp_device_CreateSurfaceFactory(IDCompositionDesktopDevice *iface,
        IUnknown *rendering_device, IDCompositionSurfaceFactory **factory)
{
    struct dcomp_device *device = impl_from_IDCompositionDesktopDevice(iface);

    TRACE("iface %p, rendering_device %p, factory %p.\n", iface, rendering_device, factory);

    if (!factory) return E_POINTER;
    return dcomp_surface_factory_create(device, rendering_device, factory);
}

static HRESULT WINAPI dcomp_device_CreateSurface(IDCompositionDesktopDevice *iface, UINT width, UINT height,
        DXGI_FORMAT pixel_format, DXGI_ALPHA_MODE alpha_mode, IDCompositionSurface **surface)
{
    struct dcomp_device *device = impl_from_IDCompositionDesktopDevice(iface);
    struct dcomp_surface *object;
    HRESULT hr;

    TRACE("iface %p, width %u, height %u, format %#x, alpha_mode %#x, surface %p.\n",
            iface, width, height, pixel_format, alpha_mode, surface);

    if (!surface) return E_POINTER;
    if (FAILED(hr = dcomp_surface_create(device, width, height, pixel_format, alpha_mode, FALSE, &object)))
        return hr;
    *surface = (IDCompositionSurface *)&object->IDCompositionVirtualSurface_iface;
    return S_OK;
}

static HRESULT WINAPI dcomp_device_CreateVirtualSurface(IDCompositionDesktopDevice *iface, UINT width,
        UINT height, DXGI_FORMAT pixel_format, DXGI_ALPHA_MODE alpha_mode, IDCompositionVirtualSurface **surface)
{
    struct dcomp_device *device = impl_from_IDCompositionDesktopDevice(iface);
    struct dcomp_surface *object;
    HRESULT hr;

    TRACE("iface %p, width %u, height %u, format %#x, alpha_mode %#x, surface %p.\n",
            iface, width, height, pixel_format, alpha_mode, surface);

    if (!surface) return E_POINTER;
    if (FAILED(hr = dcomp_surface_create(device, width, height, pixel_format, alpha_mode, TRUE, &object)))
        return hr;
    *surface = &object->IDCompositionVirtualSurface_iface;
    return S_OK;
}

#define DEVICE_CREATE_STUB(name, type) \
static HRESULT WINAPI dcomp_device_##name(IDCompositionDesktopDevice *iface, type **out) \
{ \
    TRACE("iface %p, out %p.\n", iface, out); \
    if (!out) return E_POINTER; \
    return dcomp_stub_object_create(#name, &IID_##type, (void **)out); \
}

DEVICE_CREATE_STUB(CreateTranslateTransform, IDCompositionTranslateTransform)
DEVICE_CREATE_STUB(CreateScaleTransform, IDCompositionScaleTransform)
DEVICE_CREATE_STUB(CreateRotateTransform, IDCompositionRotateTransform)
DEVICE_CREATE_STUB(CreateSkewTransform, IDCompositionSkewTransform)
DEVICE_CREATE_STUB(CreateMatrixTransform, IDCompositionMatrixTransform)
DEVICE_CREATE_STUB(CreateTranslateTransform3D, IDCompositionTranslateTransform3D)
DEVICE_CREATE_STUB(CreateScaleTransform3D, IDCompositionScaleTransform3D)
DEVICE_CREATE_STUB(CreateRotateTransform3D, IDCompositionRotateTransform3D)
DEVICE_CREATE_STUB(CreateMatrixTransform3D, IDCompositionMatrixTransform3D)
DEVICE_CREATE_STUB(CreateEffectGroup, IDCompositionEffectGroup)
DEVICE_CREATE_STUB(CreateRectangleClip, IDCompositionRectangleClip)
DEVICE_CREATE_STUB(CreateAnimation, IDCompositionAnimation)

static HRESULT WINAPI dcomp_device_CreateTransformGroup(IDCompositionDesktopDevice *iface,
        IDCompositionTransform **transforms, UINT elements, IDCompositionTransform **transform_group)
{
    TRACE("iface %p, transforms %p, elements %u, transform_group %p.\n", iface, transforms, elements, transform_group);
    if (!transform_group) return E_POINTER;
    return dcomp_stub_object_create("TransformGroup", &IID_IDCompositionTransform, (void **)transform_group);
}

static HRESULT WINAPI dcomp_device_CreateTransform3DGroup(IDCompositionDesktopDevice *iface,
        IDCompositionTransform3D **transforms, UINT elements, IDCompositionTransform3D **transform_group)
{
    TRACE("iface %p, transforms %p, elements %u, transform_group %p.\n", iface, transforms, elements, transform_group);
    if (!transform_group) return E_POINTER;
    return dcomp_stub_object_create("Transform3DGroup", &IID_IDCompositionTransform3D, (void **)transform_group);
}

static HRESULT WINAPI dcomp_device_CreateTargetForHwnd(IDCompositionDesktopDevice *iface, HWND hwnd,
        BOOL topmost, IDCompositionTarget **target)
{
    struct dcomp_device *device = impl_from_IDCompositionDesktopDevice(iface);
    struct dcomp_target *object;
    HRESULT hr;

    TRACE("iface %p, hwnd %p, topmost %d, target %p.\n", iface, hwnd, topmost, target);

    if (!target) return E_POINTER;
    if (FAILED(hr = dcomp_target_create(device, hwnd, topmost, &object))) return hr;
    *target = &object->IDCompositionTarget_iface;
    return S_OK;
}

static HRESULT WINAPI dcomp_device_CreateSurfaceFromHandle(IDCompositionDesktopDevice *iface,
        HANDLE handle, IUnknown **surface)
{
    TRACE("iface %p, handle %p, surface %p.\n", iface, handle, surface);
    if (!surface) return E_POINTER;
    return dcomp_stub_object_create("SurfaceFromHandle", &IID_IUnknown, (void **)surface);
}

static HRESULT WINAPI dcomp_device_CreateSurfaceFromHwnd(IDCompositionDesktopDevice *iface,
        HWND hwnd, IUnknown **surface)
{
    TRACE("iface %p, hwnd %p, surface %p.\n", iface, hwnd, surface);
    if (!surface) return E_POINTER;
    return dcomp_stub_object_create("SurfaceFromHwnd", &IID_IUnknown, (void **)surface);
}

static const IDCompositionDesktopDeviceVtbl dcomp_desktop_device_vtbl =
{
    dcomp_device_QueryInterface,
    dcomp_device_AddRef,
    dcomp_device_Release,
    dcomp_device_Commit,
    dcomp_device_WaitForCommitCompletion,
    dcomp_device_GetFrameStatistics,
    dcomp_device_CreateVisual,
    dcomp_device_CreateSurfaceFactory,
    dcomp_device_CreateSurface,
    dcomp_device_CreateVirtualSurface,
    dcomp_device_CreateTranslateTransform,
    dcomp_device_CreateScaleTransform,
    dcomp_device_CreateRotateTransform,
    dcomp_device_CreateSkewTransform,
    dcomp_device_CreateMatrixTransform,
    dcomp_device_CreateTransformGroup,
    dcomp_device_CreateTranslateTransform3D,
    dcomp_device_CreateScaleTransform3D,
    dcomp_device_CreateRotateTransform3D,
    dcomp_device_CreateMatrixTransform3D,
    dcomp_device_CreateTransform3DGroup,
    dcomp_device_CreateEffectGroup,
    dcomp_device_CreateRectangleClip,
    dcomp_device_CreateAnimation,
    dcomp_device_CreateTargetForHwnd,
    dcomp_device_CreateSurfaceFromHandle,
    dcomp_device_CreateSurfaceFromHwnd,
};

/* IDCompositionDevice: thin delegation to the desktop device interface. */

static HRESULT WINAPI dcomp_device1_QueryInterface(IDCompositionDevice *iface, REFIID iid, void **out)
{
    struct dcomp_device *device = impl_from_IDCompositionDevice(iface);
    return dcomp_device_QueryInterface(&device->IDCompositionDesktopDevice_iface, iid, out);
}

static ULONG WINAPI dcomp_device1_AddRef(IDCompositionDevice *iface)
{
    struct dcomp_device *device = impl_from_IDCompositionDevice(iface);
    return dcomp_device_AddRef(&device->IDCompositionDesktopDevice_iface);
}

static ULONG WINAPI dcomp_device1_Release(IDCompositionDevice *iface)
{
    struct dcomp_device *device = impl_from_IDCompositionDevice(iface);
    return dcomp_device_Release(&device->IDCompositionDesktopDevice_iface);
}

#define DEVICE1_FWD(name, ...) \
    struct dcomp_device *device = impl_from_IDCompositionDevice(iface); \
    return dcomp_device_##name(&device->IDCompositionDesktopDevice_iface, ##__VA_ARGS__)

static HRESULT WINAPI dcomp_device1_Commit(IDCompositionDevice *iface)
{
    DEVICE1_FWD(Commit);
}

static HRESULT WINAPI dcomp_device1_WaitForCommitCompletion(IDCompositionDevice *iface)
{
    DEVICE1_FWD(WaitForCommitCompletion);
}

static HRESULT WINAPI dcomp_device1_GetFrameStatistics(IDCompositionDevice *iface,
        DCOMPOSITION_FRAME_STATISTICS *statistics)
{
    DEVICE1_FWD(GetFrameStatistics, statistics);
}

static HRESULT WINAPI dcomp_device1_CreateTargetForHwnd(IDCompositionDevice *iface, HWND hwnd,
        BOOL topmost, IDCompositionTarget **target)
{
    DEVICE1_FWD(CreateTargetForHwnd, hwnd, topmost, target);
}

static HRESULT WINAPI dcomp_device1_CreateVisual(IDCompositionDevice *iface, IDCompositionVisual **visual)
{
    struct dcomp_device *device = impl_from_IDCompositionDevice(iface);
    IDCompositionVisual2 *visual2;
    HRESULT hr;

    if (FAILED(hr = dcomp_device_CreateVisual(&device->IDCompositionDesktopDevice_iface, &visual2)))
        return hr;
    *visual = (IDCompositionVisual *)visual2;
    return S_OK;
}

static HRESULT WINAPI dcomp_device1_CreateSurface(IDCompositionDevice *iface, UINT width, UINT height,
        DXGI_FORMAT pixel_format, DXGI_ALPHA_MODE alpha_mode, IDCompositionSurface **surface)
{
    DEVICE1_FWD(CreateSurface, width, height, pixel_format, alpha_mode, surface);
}

static HRESULT WINAPI dcomp_device1_CreateVirtualSurface(IDCompositionDevice *iface, UINT width, UINT height,
        DXGI_FORMAT pixel_format, DXGI_ALPHA_MODE alpha_mode, IDCompositionVirtualSurface **surface)
{
    DEVICE1_FWD(CreateVirtualSurface, width, height, pixel_format, alpha_mode, surface);
}

static HRESULT WINAPI dcomp_device1_CreateSurfaceFromHandle(IDCompositionDevice *iface, HANDLE handle,
        IUnknown **surface)
{
    DEVICE1_FWD(CreateSurfaceFromHandle, handle, surface);
}

static HRESULT WINAPI dcomp_device1_CreateSurfaceFromHwnd(IDCompositionDevice *iface, HWND hwnd,
        IUnknown **surface)
{
    DEVICE1_FWD(CreateSurfaceFromHwnd, hwnd, surface);
}

static HRESULT WINAPI dcomp_device1_CreateTranslateTransform(IDCompositionDevice *iface,
        IDCompositionTranslateTransform **transform)
{
    DEVICE1_FWD(CreateTranslateTransform, transform);
}

static HRESULT WINAPI dcomp_device1_CreateScaleTransform(IDCompositionDevice *iface,
        IDCompositionScaleTransform **transform)
{
    DEVICE1_FWD(CreateScaleTransform, transform);
}

static HRESULT WINAPI dcomp_device1_CreateRotateTransform(IDCompositionDevice *iface,
        IDCompositionRotateTransform **transform)
{
    DEVICE1_FWD(CreateRotateTransform, transform);
}

static HRESULT WINAPI dcomp_device1_CreateSkewTransform(IDCompositionDevice *iface,
        IDCompositionSkewTransform **transform)
{
    DEVICE1_FWD(CreateSkewTransform, transform);
}

static HRESULT WINAPI dcomp_device1_CreateMatrixTransform(IDCompositionDevice *iface,
        IDCompositionMatrixTransform **transform)
{
    DEVICE1_FWD(CreateMatrixTransform, transform);
}

static HRESULT WINAPI dcomp_device1_CreateTransformGroup(IDCompositionDevice *iface,
        IDCompositionTransform **transforms, UINT elements, IDCompositionTransform **transform_group)
{
    DEVICE1_FWD(CreateTransformGroup, transforms, elements, transform_group);
}

static HRESULT WINAPI dcomp_device1_CreateTranslateTransform3D(IDCompositionDevice *iface,
        IDCompositionTranslateTransform3D **transform_3d)
{
    DEVICE1_FWD(CreateTranslateTransform3D, transform_3d);
}

static HRESULT WINAPI dcomp_device1_CreateScaleTransform3D(IDCompositionDevice *iface,
        IDCompositionScaleTransform3D **transform_3d)
{
    DEVICE1_FWD(CreateScaleTransform3D, transform_3d);
}

static HRESULT WINAPI dcomp_device1_CreateRotateTransform3D(IDCompositionDevice *iface,
        IDCompositionRotateTransform3D **transform_3d)
{
    DEVICE1_FWD(CreateRotateTransform3D, transform_3d);
}

static HRESULT WINAPI dcomp_device1_CreateMatrixTransform3D(IDCompositionDevice *iface,
        IDCompositionMatrixTransform3D **transform_3d)
{
    DEVICE1_FWD(CreateMatrixTransform3D, transform_3d);
}

static HRESULT WINAPI dcomp_device1_CreateTransform3DGroup(IDCompositionDevice *iface,
        IDCompositionTransform3D **transforms_3d, UINT elements, IDCompositionTransform3D **transform_3d_group)
{
    DEVICE1_FWD(CreateTransform3DGroup, transforms_3d, elements, transform_3d_group);
}

static HRESULT WINAPI dcomp_device1_CreateEffectGroup(IDCompositionDevice *iface,
        IDCompositionEffectGroup **effect_group)
{
    DEVICE1_FWD(CreateEffectGroup, effect_group);
}

static HRESULT WINAPI dcomp_device1_CreateRectangleClip(IDCompositionDevice *iface,
        IDCompositionRectangleClip **clip)
{
    DEVICE1_FWD(CreateRectangleClip, clip);
}

static HRESULT WINAPI dcomp_device1_CreateAnimation(IDCompositionDevice *iface,
        IDCompositionAnimation **animation)
{
    DEVICE1_FWD(CreateAnimation, animation);
}

static HRESULT WINAPI dcomp_device1_CheckDeviceState(IDCompositionDevice *iface, BOOL *valid)
{
    TRACE("iface %p, valid %p.\n", iface, valid);
    if (!valid) return E_POINTER;
    *valid = TRUE;
    return S_OK;
}

static const IDCompositionDeviceVtbl dcomp_device1_vtbl =
{
    dcomp_device1_QueryInterface,
    dcomp_device1_AddRef,
    dcomp_device1_Release,
    dcomp_device1_Commit,
    dcomp_device1_WaitForCommitCompletion,
    dcomp_device1_GetFrameStatistics,
    dcomp_device1_CreateTargetForHwnd,
    dcomp_device1_CreateVisual,
    dcomp_device1_CreateSurface,
    dcomp_device1_CreateVirtualSurface,
    dcomp_device1_CreateSurfaceFromHandle,
    dcomp_device1_CreateSurfaceFromHwnd,
    dcomp_device1_CreateTranslateTransform,
    dcomp_device1_CreateScaleTransform,
    dcomp_device1_CreateRotateTransform,
    dcomp_device1_CreateSkewTransform,
    dcomp_device1_CreateMatrixTransform,
    dcomp_device1_CreateTransformGroup,
    dcomp_device1_CreateTranslateTransform3D,
    dcomp_device1_CreateScaleTransform3D,
    dcomp_device1_CreateRotateTransform3D,
    dcomp_device1_CreateMatrixTransform3D,
    dcomp_device1_CreateTransform3DGroup,
    dcomp_device1_CreateEffectGroup,
    dcomp_device1_CreateRectangleClip,
    dcomp_device1_CreateAnimation,
    dcomp_device1_CheckDeviceState,
};

/* IDCompositionDevice3: Device2 prefix delegated, effect creators are stubs. */

static HRESULT WINAPI dcomp_device3_QueryInterface(IDCompositionDevice3 *iface, REFIID iid, void **out)
{
    struct dcomp_device *device = impl_from_IDCompositionDevice3(iface);
    return dcomp_device_QueryInterface(&device->IDCompositionDesktopDevice_iface, iid, out);
}

static ULONG WINAPI dcomp_device3_AddRef(IDCompositionDevice3 *iface)
{
    struct dcomp_device *device = impl_from_IDCompositionDevice3(iface);
    return dcomp_device_AddRef(&device->IDCompositionDesktopDevice_iface);
}

static ULONG WINAPI dcomp_device3_Release(IDCompositionDevice3 *iface)
{
    struct dcomp_device *device = impl_from_IDCompositionDevice3(iface);
    return dcomp_device_Release(&device->IDCompositionDesktopDevice_iface);
}

#define DEVICE3_FWD(name, ...) \
    struct dcomp_device *device = impl_from_IDCompositionDevice3(iface); \
    return dcomp_device_##name(&device->IDCompositionDesktopDevice_iface, ##__VA_ARGS__)

static HRESULT WINAPI dcomp_device3_Commit(IDCompositionDevice3 *iface)
{
    DEVICE3_FWD(Commit);
}

static HRESULT WINAPI dcomp_device3_WaitForCommitCompletion(IDCompositionDevice3 *iface)
{
    DEVICE3_FWD(WaitForCommitCompletion);
}

static HRESULT WINAPI dcomp_device3_GetFrameStatistics(IDCompositionDevice3 *iface,
        DCOMPOSITION_FRAME_STATISTICS *statistics)
{
    DEVICE3_FWD(GetFrameStatistics, statistics);
}

static HRESULT WINAPI dcomp_device3_CreateVisual(IDCompositionDevice3 *iface, IDCompositionVisual2 **visual)
{
    DEVICE3_FWD(CreateVisual, visual);
}

static HRESULT WINAPI dcomp_device3_CreateSurfaceFactory(IDCompositionDevice3 *iface,
        IUnknown *rendering_device, IDCompositionSurfaceFactory **factory)
{
    DEVICE3_FWD(CreateSurfaceFactory, rendering_device, factory);
}

static HRESULT WINAPI dcomp_device3_CreateSurface(IDCompositionDevice3 *iface, UINT width, UINT height,
        DXGI_FORMAT pixel_format, DXGI_ALPHA_MODE alpha_mode, IDCompositionSurface **surface)
{
    DEVICE3_FWD(CreateSurface, width, height, pixel_format, alpha_mode, surface);
}

static HRESULT WINAPI dcomp_device3_CreateVirtualSurface(IDCompositionDevice3 *iface, UINT width, UINT height,
        DXGI_FORMAT pixel_format, DXGI_ALPHA_MODE alpha_mode, IDCompositionVirtualSurface **surface)
{
    DEVICE3_FWD(CreateVirtualSurface, width, height, pixel_format, alpha_mode, surface);
}

static HRESULT WINAPI dcomp_device3_CreateTranslateTransform(IDCompositionDevice3 *iface,
        IDCompositionTranslateTransform **transform)
{
    DEVICE3_FWD(CreateTranslateTransform, transform);
}

static HRESULT WINAPI dcomp_device3_CreateScaleTransform(IDCompositionDevice3 *iface,
        IDCompositionScaleTransform **transform)
{
    DEVICE3_FWD(CreateScaleTransform, transform);
}

static HRESULT WINAPI dcomp_device3_CreateRotateTransform(IDCompositionDevice3 *iface,
        IDCompositionRotateTransform **transform)
{
    DEVICE3_FWD(CreateRotateTransform, transform);
}

static HRESULT WINAPI dcomp_device3_CreateSkewTransform(IDCompositionDevice3 *iface,
        IDCompositionSkewTransform **transform)
{
    DEVICE3_FWD(CreateSkewTransform, transform);
}

static HRESULT WINAPI dcomp_device3_CreateMatrixTransform(IDCompositionDevice3 *iface,
        IDCompositionMatrixTransform **transform)
{
    DEVICE3_FWD(CreateMatrixTransform, transform);
}

static HRESULT WINAPI dcomp_device3_CreateTransformGroup(IDCompositionDevice3 *iface,
        IDCompositionTransform **transforms, UINT elements, IDCompositionTransform **transform_group)
{
    DEVICE3_FWD(CreateTransformGroup, transforms, elements, transform_group);
}

static HRESULT WINAPI dcomp_device3_CreateTranslateTransform3D(IDCompositionDevice3 *iface,
        IDCompositionTranslateTransform3D **transform_3d)
{
    DEVICE3_FWD(CreateTranslateTransform3D, transform_3d);
}

static HRESULT WINAPI dcomp_device3_CreateScaleTransform3D(IDCompositionDevice3 *iface,
        IDCompositionScaleTransform3D **transform_3d)
{
    DEVICE3_FWD(CreateScaleTransform3D, transform_3d);
}

static HRESULT WINAPI dcomp_device3_CreateRotateTransform3D(IDCompositionDevice3 *iface,
        IDCompositionRotateTransform3D **transform_3d)
{
    DEVICE3_FWD(CreateRotateTransform3D, transform_3d);
}

static HRESULT WINAPI dcomp_device3_CreateMatrixTransform3D(IDCompositionDevice3 *iface,
        IDCompositionMatrixTransform3D **transform_3d)
{
    DEVICE3_FWD(CreateMatrixTransform3D, transform_3d);
}

static HRESULT WINAPI dcomp_device3_CreateTransform3DGroup(IDCompositionDevice3 *iface,
        IDCompositionTransform3D **transforms_3d, UINT elements, IDCompositionTransform3D **transform_3d_group)
{
    DEVICE3_FWD(CreateTransform3DGroup, transforms_3d, elements, transform_3d_group);
}

static HRESULT WINAPI dcomp_device3_CreateEffectGroup(IDCompositionDevice3 *iface,
        IDCompositionEffectGroup **effect_group)
{
    DEVICE3_FWD(CreateEffectGroup, effect_group);
}

static HRESULT WINAPI dcomp_device3_CreateRectangleClip(IDCompositionDevice3 *iface,
        IDCompositionRectangleClip **clip)
{
    DEVICE3_FWD(CreateRectangleClip, clip);
}

static HRESULT WINAPI dcomp_device3_CreateAnimation(IDCompositionDevice3 *iface,
        IDCompositionAnimation **animation)
{
    DEVICE3_FWD(CreateAnimation, animation);
}

#define DEVICE3_CREATE_STUB(name, type) \
static HRESULT WINAPI dcomp_device3_##name(IDCompositionDevice3 *iface, type **out) \
{ \
    TRACE("iface %p, out %p.\n", iface, out); \
    if (!out) return E_POINTER; \
    return dcomp_stub_object_create(#name, &IID_##type, (void **)out); \
}

DEVICE3_CREATE_STUB(CreateGaussianBlurEffect, IDCompositionGaussianBlurEffect)
DEVICE3_CREATE_STUB(CreateBrightnessEffect, IDCompositionBrightnessEffect)
DEVICE3_CREATE_STUB(CreateColorMatrixEffect, IDCompositionColorMatrixEffect)
DEVICE3_CREATE_STUB(CreateShadowEffect, IDCompositionShadowEffect)
DEVICE3_CREATE_STUB(CreateHueRotationEffect, IDCompositionHueRotationEffect)
DEVICE3_CREATE_STUB(CreateSaturationEffect, IDCompositionSaturationEffect)
DEVICE3_CREATE_STUB(CreateTurbulenceEffect, IDCompositionTurbulenceEffect)
DEVICE3_CREATE_STUB(CreateLinearTransferEffect, IDCompositionLinearTransferEffect)
DEVICE3_CREATE_STUB(CreateTableTransferEffect, IDCompositionTableTransferEffect)
DEVICE3_CREATE_STUB(CreateCompositeEffect, IDCompositionCompositeEffect)
DEVICE3_CREATE_STUB(CreateBlendEffect, IDCompositionBlendEffect)
DEVICE3_CREATE_STUB(CreateArithmeticCompositeEffect, IDCompositionArithmeticCompositeEffect)
DEVICE3_CREATE_STUB(CreateAffineTransform2DEffect, IDCompositionAffineTransform2DEffect)

static const IDCompositionDevice3Vtbl dcomp_device3_vtbl =
{
    dcomp_device3_QueryInterface,
    dcomp_device3_AddRef,
    dcomp_device3_Release,
    dcomp_device3_Commit,
    dcomp_device3_WaitForCommitCompletion,
    dcomp_device3_GetFrameStatistics,
    dcomp_device3_CreateVisual,
    dcomp_device3_CreateSurfaceFactory,
    dcomp_device3_CreateSurface,
    dcomp_device3_CreateVirtualSurface,
    dcomp_device3_CreateTranslateTransform,
    dcomp_device3_CreateScaleTransform,
    dcomp_device3_CreateRotateTransform,
    dcomp_device3_CreateSkewTransform,
    dcomp_device3_CreateMatrixTransform,
    dcomp_device3_CreateTransformGroup,
    dcomp_device3_CreateTranslateTransform3D,
    dcomp_device3_CreateScaleTransform3D,
    dcomp_device3_CreateRotateTransform3D,
    dcomp_device3_CreateMatrixTransform3D,
    dcomp_device3_CreateTransform3DGroup,
    dcomp_device3_CreateEffectGroup,
    dcomp_device3_CreateRectangleClip,
    dcomp_device3_CreateAnimation,
    dcomp_device3_CreateGaussianBlurEffect,
    dcomp_device3_CreateBrightnessEffect,
    dcomp_device3_CreateColorMatrixEffect,
    dcomp_device3_CreateShadowEffect,
    dcomp_device3_CreateHueRotationEffect,
    dcomp_device3_CreateSaturationEffect,
    dcomp_device3_CreateTurbulenceEffect,
    dcomp_device3_CreateLinearTransferEffect,
    dcomp_device3_CreateTableTransferEffect,
    dcomp_device3_CreateCompositeEffect,
    dcomp_device3_CreateBlendEffect,
    dcomp_device3_CreateArithmeticCompositeEffect,
    dcomp_device3_CreateAffineTransform2DEffect,
};

static HRESULT dcomp_device_create(IUnknown *rendering_device, REFIID iid, void **out)
{
    struct dcomp_device *object;
    HRESULT hr;

    if (!out) return E_POINTER;
    *out = NULL;

    if (!(object = calloc(1, sizeof(*object)))) return E_OUTOFMEMORY;

    object->IDCompositionDesktopDevice_iface.lpVtbl = &dcomp_desktop_device_vtbl;
    object->IDCompositionDevice_iface.lpVtbl = &dcomp_device1_vtbl;
    object->IDCompositionDevice3_iface.lpVtbl = &dcomp_device3_vtbl;
    object->refcount = 1;
    InitializeCriticalSectionEx(&object->cs, 0, RTL_CRITICAL_SECTION_FLAG_FORCE_DEBUG_INFO);
    object->cs.DebugInfo->Spare[0] = (DWORD_PTR)(__FILE__ ": dcomp_device.cs");
    list_init(&object->targets);

    if (rendering_device)
    {
        object->rendering_device = rendering_device;
        IUnknown_AddRef(rendering_device);
        if (FAILED(IUnknown_QueryInterface(rendering_device, &IID_ID3D11Device, (void **)&object->d3d_device)))
            object->d3d_device = NULL;
    }

    hr = dcomp_device_QueryInterface(&object->IDCompositionDesktopDevice_iface, iid, out);
    dcomp_device_Release(&object->IDCompositionDesktopDevice_iface);
    return hr;
}

HRESULT WINAPI DCompositionCreateDevice(IDXGIDevice *dxgi_device, REFIID iid, void **device)
{
    TRACE("%p, %s, %p.\n", dxgi_device, debugstr_guid(iid), device);

    return dcomp_device_create((IUnknown *)dxgi_device, iid, device);
}

HRESULT WINAPI DCompositionCreateDevice2(IUnknown *rendering_device, REFIID iid, void **device)
{
    TRACE("%p, %s, %p.\n", rendering_device, debugstr_guid(iid), device);

    return dcomp_device_create(rendering_device, iid, device);
}

HRESULT WINAPI DCompositionCreateDevice3(IUnknown *rendering_device, REFIID iid, void **device)
{
    TRACE("%p, %s, %p.\n", rendering_device, debugstr_guid(iid), device);

    return dcomp_device_create(rendering_device, iid, device);
}

HRESULT WINAPI DCompositionCreateSurfaceHandle(DWORD desired_access, SECURITY_ATTRIBUTES *security_attributes,
        HANDLE *handle)
{
    TRACE("%#lx, %p, %p.\n", desired_access, security_attributes, handle);

    if (!handle) return E_POINTER;

    /* No real composition surface objects exist; hand out a plain event so
     * callers get a valid, closable handle. */
    *handle = CreateEventW(security_attributes, FALSE, FALSE, NULL);
    return *handle ? S_OK : HRESULT_FROM_WIN32(GetLastError());
}
