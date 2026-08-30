// Private compatibility bindings for APIs that OpenXR does not standardize.
// These are intentionally namespaced to XR_XRSIM and are never presented as
// Khronos extensions. Applications must explicitly opt in to them.

#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d9.h>
#include <d3d10_1.h>
#include <openxr/openxr.h>

#define XR_XRSIM_D3D9_ENABLE_EXTENSION_NAME "XR_XRSIM_d3d9_enable"
#define XR_XRSIM_D3D9_ENABLE_SPEC_VERSION 1
#define XR_XRSIM_D3D10_ENABLE_EXTENSION_NAME "XR_XRSIM_d3d10_enable"
#define XR_XRSIM_D3D10_ENABLE_SPEC_VERSION 1

// Private structure type values. Khronos-reserved values are never used.
#define XR_TYPE_GRAPHICS_BINDING_D3D9_XRSIM ((XrStructureType)0x58525301)
#define XR_TYPE_SWAPCHAIN_IMAGE_D3D9_XRSIM ((XrStructureType)0x58525302)
#define XR_TYPE_GRAPHICS_REQUIREMENTS_D3D9_XRSIM ((XrStructureType)0x58525303)
#define XR_TYPE_GRAPHICS_BINDING_D3D10_XRSIM ((XrStructureType)0x58525311)
#define XR_TYPE_SWAPCHAIN_IMAGE_D3D10_XRSIM ((XrStructureType)0x58525312)
#define XR_TYPE_GRAPHICS_REQUIREMENTS_D3D10_XRSIM ((XrStructureType)0x58525313)

typedef struct XrGraphicsBindingD3D9XRSIM {
    XrStructureType type;
    const void* next;
    IDirect3DDevice9* device;
} XrGraphicsBindingD3D9XRSIM;

typedef struct XrSwapchainImageD3D9XRSIM {
    XrStructureType type;
    void* next;
    IDirect3DTexture9* texture;
    HANDLE sharedHandle;
} XrSwapchainImageD3D9XRSIM;

typedef struct XrGraphicsRequirementsD3D9XRSIM {
    XrStructureType type;
    void* next;
    uint32_t minimumPixelShaderVersion;
} XrGraphicsRequirementsD3D9XRSIM;

typedef struct XrGraphicsBindingD3D10XRSIM {
    XrStructureType type;
    const void* next;
    ID3D10Device* device;
} XrGraphicsBindingD3D10XRSIM;

typedef struct XrSwapchainImageD3D10XRSIM {
    XrStructureType type;
    void* next;
    ID3D10Texture2D* texture;
} XrSwapchainImageD3D10XRSIM;

typedef struct XrGraphicsRequirementsD3D10XRSIM {
    XrStructureType type;
    void* next;
    LUID adapterLuid;
    D3D10_FEATURE_LEVEL1 minimumFeatureLevel;
} XrGraphicsRequirementsD3D10XRSIM;

typedef XrResult(XRAPI_PTR* PFN_xrGetD3D9GraphicsRequirementsXRSIM)(
    XrInstance, XrSystemId, XrGraphicsRequirementsD3D9XRSIM*);
typedef XrResult(XRAPI_PTR* PFN_xrGetD3D10GraphicsRequirementsXRSIM)(
    XrInstance, XrSystemId, XrGraphicsRequirementsD3D10XRSIM*);
