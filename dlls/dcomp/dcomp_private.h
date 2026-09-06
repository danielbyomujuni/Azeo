/*
 * DirectComposition private definitions
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

#ifndef __WINE_DCOMP_PRIVATE_H
#define __WINE_DCOMP_PRIVATE_H

#define COBJMACROS
#include <stdarg.h>
#include <stddef.h>
#include <stdlib.h>

#include "windef.h"
#include "winbase.h"
#include "objidl.h"
#include "d3d11.h"
#include "dcomp.h"

#include "wine/debug.h"
#include "wine/list.h"

struct dcomp_device
{
    IDCompositionDesktopDevice IDCompositionDesktopDevice_iface;
    IDCompositionDevice IDCompositionDevice_iface;
    IDCompositionDevice3 IDCompositionDevice3_iface;
    LONG refcount;

    IUnknown *rendering_device;   /* what the app passed in, may be NULL */
    ID3D11Device *d3d_device;     /* QI'd from rendering_device, may be NULL */

    CRITICAL_SECTION cs;
    struct list targets;          /* struct dcomp_target.entry */
};

struct dcomp_target
{
    IDCompositionTarget IDCompositionTarget_iface;
    LONG refcount;

    HWND hwnd;
    BOOL topmost;
    struct dcomp_visual *root;    /* referenced */
    struct dcomp_device *device;  /* referenced */
    struct list entry;            /* in device->targets */
};

struct dcomp_visual
{
    IDCompositionVisual2 IDCompositionVisual2_iface;
    LONG refcount;

    float offset_x, offset_y;
    IUnknown *content;            /* referenced, usually a swapchain or surface */
    struct list children;         /* struct visual_child.entry, back-to-front */
    CRITICAL_SECTION cs;
};

struct visual_child
{
    struct list entry;
    struct dcomp_visual *visual;  /* referenced */
};

struct dcomp_surface
{
    IDCompositionVirtualSurface IDCompositionVirtualSurface_iface;
    LONG refcount;

    UINT width, height;
    DXGI_FORMAT format;
    DXGI_ALPHA_MODE alpha_mode;
    ID3D11Texture2D *texture;     /* backing texture, may be NULL */
    BOOL drawing;
};

struct dcomp_visual *unsafe_impl_from_IDCompositionVisual(IDCompositionVisual *iface);
struct dcomp_surface *unsafe_impl_from_content(IUnknown *content);

HRESULT dcomp_visual_create(struct dcomp_visual **visual);
HRESULT dcomp_target_create(struct dcomp_device *device, HWND hwnd, BOOL topmost,
                            struct dcomp_target **target);
HRESULT dcomp_surface_create(struct dcomp_device *device, UINT width, UINT height,
                             DXGI_FORMAT format, DXGI_ALPHA_MODE alpha_mode, BOOL virtual_surface,
                             struct dcomp_surface **surface);
HRESULT dcomp_surface_factory_create(struct dcomp_device *device, IUnknown *rendering_device,
                                     IDCompositionSurfaceFactory **factory);
HRESULT dcomp_stub_object_create(const char *name, REFIID iid, void **out);

void dcomp_target_destroyed(struct dcomp_target *target);

/* present pump */
void pump_commit_device(struct dcomp_device *device);
void pump_forget_target(struct dcomp_target *target);

#endif
