// Renderer backends for xr-sim. The simulation core never owns an application
// backbuffer; it only creates OpenXR swapchain images on the graphics device or
// context supplied in XrSessionCreateInfo.

#include "xrsim_internal.h"

#include <algorithm>
#include <cstring>

namespace xrsim {
namespace {

struct GraphicsState {
    GraphicsApi api = GraphicsApi::None;
    IDirect3DDevice9* d3d9 = nullptr;
    ID3D10Device* d3d10 = nullptr;
    ID3D11Device* d3d11 = nullptr;
    ID3D11DeviceContext* d3d11Context = nullptr;
    ID3D12Device* d3d12 = nullptr;
    ID3D12CommandQueue* d3d12Queue = nullptr;
    HDC glDC = nullptr;
    HGLRC glRC = nullptr;

    HMODULE vulkanModule = nullptr;
    VkInstance vkInstance = VK_NULL_HANDLE;
    VkPhysicalDevice vkPhysicalDevice = VK_NULL_HANDLE;
    VkDevice vkDevice = VK_NULL_HANDLE;
    VkQueue vkQueue = VK_NULL_HANDLE;
    uint32_t vkQueueFamily = 0;
    PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr = nullptr;
    PFN_vkGetDeviceProcAddr vkGetDeviceProcAddr = nullptr;
    PFN_vkGetPhysicalDeviceMemoryProperties vkGetPhysicalDeviceMemoryProperties = nullptr;
    PFN_vkCreateImage vkCreateImage = nullptr;
    PFN_vkDestroyImage vkDestroyImage = nullptr;
    PFN_vkGetImageMemoryRequirements vkGetImageMemoryRequirements = nullptr;
    PFN_vkAllocateMemory vkAllocateMemory = nullptr;
    PFN_vkFreeMemory vkFreeMemory = nullptr;
    PFN_vkBindImageMemory vkBindImageMemory = nullptr;
    PFN_vkGetDeviceQueue vkGetDeviceQueue = nullptr;
} s;

class GlContextGuard {
public:
    GlContextGuard() : oldDC_(wglGetCurrentDC()), oldRC_(wglGetCurrentContext()) {
        ok_ = (oldDC_ == s.glDC && oldRC_ == s.glRC) || wglMakeCurrent(s.glDC, s.glRC) == TRUE;
        changed_ = ok_ && (oldDC_ != s.glDC || oldRC_ != s.glRC);
    }
    ~GlContextGuard() {
        if (changed_) wglMakeCurrent(oldDC_, oldRC_);
    }
    bool ok() const { return ok_; }

private:
    HDC oldDC_ = nullptr;
    HGLRC oldRC_ = nullptr;
    bool ok_ = false;
    bool changed_ = false;
};

template <typename T>
T vk_instance_proc(const char* name) {
    if (!s.vkGetInstanceProcAddr) return nullptr;
    return reinterpret_cast<T>(s.vkGetInstanceProcAddr(s.vkInstance, name));
}

template <typename T>
T vk_device_proc(const char* name) {
    if (!s.vkGetDeviceProcAddr) return nullptr;
    return reinterpret_cast<T>(s.vkGetDeviceProcAddr(s.vkDevice, name));
}

bool init_vulkan(const XrGraphicsBindingVulkanKHR& b) {
    s.vulkanModule = LoadLibraryW(L"vulkan-1.dll");
    if (!s.vulkanModule) return false;
    s.vkGetInstanceProcAddr = reinterpret_cast<PFN_vkGetInstanceProcAddr>(
        GetProcAddress(s.vulkanModule, "vkGetInstanceProcAddr"));
    if (!s.vkGetInstanceProcAddr) return false;
    s.vkInstance = b.instance;
    s.vkPhysicalDevice = b.physicalDevice;
    s.vkDevice = b.device;
    s.vkQueueFamily = b.queueFamilyIndex;
    s.vkGetDeviceProcAddr = vk_instance_proc<PFN_vkGetDeviceProcAddr>("vkGetDeviceProcAddr");
    s.vkGetPhysicalDeviceMemoryProperties =
        vk_instance_proc<PFN_vkGetPhysicalDeviceMemoryProperties>("vkGetPhysicalDeviceMemoryProperties");
    s.vkCreateImage = vk_device_proc<PFN_vkCreateImage>("vkCreateImage");
    s.vkDestroyImage = vk_device_proc<PFN_vkDestroyImage>("vkDestroyImage");
    s.vkGetImageMemoryRequirements =
        vk_device_proc<PFN_vkGetImageMemoryRequirements>("vkGetImageMemoryRequirements");
    s.vkAllocateMemory = vk_device_proc<PFN_vkAllocateMemory>("vkAllocateMemory");
    s.vkFreeMemory = vk_device_proc<PFN_vkFreeMemory>("vkFreeMemory");
    s.vkBindImageMemory = vk_device_proc<PFN_vkBindImageMemory>("vkBindImageMemory");
    s.vkGetDeviceQueue = vk_device_proc<PFN_vkGetDeviceQueue>("vkGetDeviceQueue");
    if (!s.vkGetDeviceProcAddr || !s.vkGetPhysicalDeviceMemoryProperties || !s.vkCreateImage ||
        !s.vkDestroyImage || !s.vkGetImageMemoryRequirements || !s.vkAllocateMemory ||
        !s.vkFreeMemory || !s.vkBindImageMemory || !s.vkGetDeviceQueue)
        return false;
    s.vkGetDeviceQueue(s.vkDevice, b.queueFamilyIndex, b.queueIndex, &s.vkQueue);
    return s.vkQueue != VK_NULL_HANDLE;
}

uint32_t find_vk_memory_type(uint32_t bits, VkMemoryPropertyFlags wanted) {
    VkPhysicalDeviceMemoryProperties p{};
    s.vkGetPhysicalDeviceMemoryProperties(s.vkPhysicalDevice, &p);
    for (uint32_t i = 0; i < p.memoryTypeCount; ++i)
        if ((bits & (1u << i)) && (p.memoryTypes[i].propertyFlags & wanted) == wanted) return i;
    for (uint32_t i = 0; i < p.memoryTypeCount; ++i)
        if (bits & (1u << i)) return i;
    return UINT32_MAX;
}

const int64_t kD3DFormats[] = {
    DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, DXGI_FORMAT_R8G8B8A8_UNORM,
    DXGI_FORMAT_B8G8R8A8_UNORM_SRGB, DXGI_FORMAT_B8G8R8A8_UNORM,
    DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R10G10B10A2_UNORM,
    DXGI_FORMAT_D32_FLOAT, DXGI_FORMAT_D24_UNORM_S8_UINT};

const int64_t kD3D10Formats[] = {
    DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, DXGI_FORMAT_R8G8B8A8_UNORM,
    DXGI_FORMAT_B8G8R8A8_UNORM_SRGB, DXGI_FORMAT_B8G8R8A8_UNORM,
    DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R10G10B10A2_UNORM,
    DXGI_FORMAT_D32_FLOAT, DXGI_FORMAT_D24_UNORM_S8_UINT};

const int64_t kD3D9Formats[] = {
    D3DFMT_A8R8G8B8, D3DFMT_X8R8G8B8, D3DFMT_A2R10G10B10,
    D3DFMT_A16B16G16R16F, D3DFMT_D24S8, D3DFMT_D32};

#ifndef GL_SRGB8_ALPHA8
#define GL_SRGB8_ALPHA8 0x8C43
#endif
#ifndef GL_RGBA16F
#define GL_RGBA16F 0x881A
#endif
#ifndef GL_RGB10_A2
#define GL_RGB10_A2 0x8059
#endif
#ifndef GL_DEPTH_COMPONENT32F
#define GL_DEPTH_COMPONENT32F 0x8CAC
#endif
#ifndef GL_DEPTH24_STENCIL8
#define GL_DEPTH24_STENCIL8 0x88F0
#endif
#ifndef GL_DEPTH_STENCIL
#define GL_DEPTH_STENCIL 0x84F9
#endif
#ifndef GL_UNSIGNED_INT_24_8
#define GL_UNSIGNED_INT_24_8 0x84FA
#endif
#ifndef GL_HALF_FLOAT
#define GL_HALF_FLOAT 0x140B
#endif
#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812F
#endif

const int64_t kGLFormats[] = {GL_SRGB8_ALPHA8, GL_RGBA8, GL_RGBA16F, GL_RGB10_A2,
                              GL_DEPTH_COMPONENT32F, GL_DEPTH24_STENCIL8};

const int64_t kVkFormats[] = {
    VK_FORMAT_R8G8B8A8_SRGB, VK_FORMAT_R8G8B8A8_UNORM,
    VK_FORMAT_B8G8R8A8_SRGB, VK_FORMAT_B8G8R8A8_UNORM,
    VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_A2B10G10R10_UNORM_PACK32,
    VK_FORMAT_D32_SFLOAT, VK_FORMAT_D24_UNORM_S8_UINT};

template <size_t N>
XrResult copy_formats(const int64_t (&source)[N], uint32_t capacity,
                      uint32_t* countOutput, int64_t* formats) {
    if (!countOutput) return XR_ERROR_VALIDATION_FAILURE;
    *countOutput = static_cast<uint32_t>(N);
    if (capacity == 0) return XR_SUCCESS;
    if (capacity < N || !formats) return XR_ERROR_SIZE_INSUFFICIENT;
    memcpy(formats, source, sizeof(source));
    return XR_SUCCESS;
}

} // namespace

GraphicsApi graphics_api() { return s.api; }

const char* graphics_api_name() {
    switch (s.api) {
    case GraphicsApi::Headless: return "headless";
    case GraphicsApi::D3D9: return "D3D9 (XR_XRSIM)";
    case GraphicsApi::D3D10: return "D3D10 (XR_XRSIM)";
    case GraphicsApi::D3D11: return "D3D11";
    case GraphicsApi::D3D12: return "D3D12";
    case GraphicsApi::OpenGL: return "OpenGL";
    case GraphicsApi::Vulkan: return "Vulkan";
    default: return "none";
    }
}

bool graphics_session_ready() { return s.api != GraphicsApi::None; }
ID3D11Device* graphics_d3d11_device() { return s.d3d11; }
ID3D11DeviceContext* graphics_d3d11_context() { return s.d3d11Context; }

XrResult graphics_create_session(const XrSessionCreateInfo* info) {
    const XrBaseInStructure* binding = nullptr;
    for (const auto* next = static_cast<const XrBaseInStructure*>(info->next); next;
         next = next->next) {
        const bool known = next->type == XR_TYPE_GRAPHICS_BINDING_D3D11_KHR ||
                           next->type == XR_TYPE_GRAPHICS_BINDING_D3D12_KHR ||
                           next->type == XR_TYPE_GRAPHICS_BINDING_OPENGL_WIN32_KHR ||
                           next->type == XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR ||
                           next->type == XR_TYPE_GRAPHICS_BINDING_D3D9_XRSIM ||
                           next->type == XR_TYPE_GRAPHICS_BINDING_D3D10_XRSIM;
        if (!known) continue;
        if (binding) return XR_ERROR_VALIDATION_FAILURE;
        binding = next;
    }

    if (!binding) {
        if (!g.headlessEnabled) return XR_ERROR_GRAPHICS_DEVICE_INVALID;
        s.api = GraphicsApi::Headless;
        return XR_SUCCESS;
    }

    switch (binding->type) {
    case XR_TYPE_GRAPHICS_BINDING_D3D11_KHR: {
        if (!g.d3d11Enabled) return XR_ERROR_FUNCTION_UNSUPPORTED;
        if (!g.d3d11RequirementsCalled) return XR_ERROR_GRAPHICS_REQUIREMENTS_CALL_MISSING;
        const auto* b = reinterpret_cast<const XrGraphicsBindingD3D11KHR*>(binding);
        if (!b->device) return XR_ERROR_GRAPHICS_DEVICE_INVALID;
        s.d3d11 = b->device;
        s.d3d11->AddRef();
        s.d3d11->GetImmediateContext(&s.d3d11Context);
        s.api = GraphicsApi::D3D11;
        break;
    }
    case XR_TYPE_GRAPHICS_BINDING_D3D12_KHR: {
        if (!g.d3d12Enabled) return XR_ERROR_FUNCTION_UNSUPPORTED;
        if (!g.d3d12RequirementsCalled) return XR_ERROR_GRAPHICS_REQUIREMENTS_CALL_MISSING;
        const auto* b = reinterpret_cast<const XrGraphicsBindingD3D12KHR*>(binding);
        if (!b->device || !b->queue) return XR_ERROR_GRAPHICS_DEVICE_INVALID;
        s.d3d12 = b->device;
        s.d3d12Queue = b->queue;
        s.d3d12->AddRef();
        s.d3d12Queue->AddRef();
        s.api = GraphicsApi::D3D12;
        break;
    }
    case XR_TYPE_GRAPHICS_BINDING_OPENGL_WIN32_KHR: {
        if (!g.openGLEnabled) return XR_ERROR_FUNCTION_UNSUPPORTED;
        if (!g.openGLRequirementsCalled) return XR_ERROR_GRAPHICS_REQUIREMENTS_CALL_MISSING;
        const auto* b = reinterpret_cast<const XrGraphicsBindingOpenGLWin32KHR*>(binding);
        if (!b->hDC || !b->hGLRC) return XR_ERROR_GRAPHICS_DEVICE_INVALID;
        s.glDC = b->hDC;
        s.glRC = b->hGLRC;
        s.api = GraphicsApi::OpenGL;
        break;
    }
    case XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR: {
        if (!g.vulkanEnabled && !g.vulkan2Enabled) return XR_ERROR_FUNCTION_UNSUPPORTED;
        if (!g.vulkanRequirementsCalled) return XR_ERROR_GRAPHICS_REQUIREMENTS_CALL_MISSING;
        const auto* b = reinterpret_cast<const XrGraphicsBindingVulkanKHR*>(binding);
        if (!b->instance || !b->physicalDevice || !b->device || !init_vulkan(*b)) {
            graphics_destroy_session();
            return XR_ERROR_GRAPHICS_DEVICE_INVALID;
        }
        s.api = GraphicsApi::Vulkan;
        break;
    }
    default:
        if (binding->type == XR_TYPE_GRAPHICS_BINDING_D3D9_XRSIM) {
            if (!g.d3d9Enabled) return XR_ERROR_FUNCTION_UNSUPPORTED;
            if (!g.d3d9RequirementsCalled) return XR_ERROR_GRAPHICS_REQUIREMENTS_CALL_MISSING;
            const auto* b = reinterpret_cast<const XrGraphicsBindingD3D9XRSIM*>(binding);
            if (!b->device) return XR_ERROR_GRAPHICS_DEVICE_INVALID;
            s.d3d9 = b->device;
            s.d3d9->AddRef();
            s.api = GraphicsApi::D3D9;
        } else if (binding->type == XR_TYPE_GRAPHICS_BINDING_D3D10_XRSIM) {
            if (!g.d3d10Enabled) return XR_ERROR_FUNCTION_UNSUPPORTED;
            if (!g.d3d10RequirementsCalled) return XR_ERROR_GRAPHICS_REQUIREMENTS_CALL_MISSING;
            const auto* b = reinterpret_cast<const XrGraphicsBindingD3D10XRSIM*>(binding);
            if (!b->device) return XR_ERROR_GRAPHICS_DEVICE_INVALID;
            s.d3d10 = b->device;
            s.d3d10->AddRef();
            s.api = GraphicsApi::D3D10;
        } else {
            return XR_ERROR_GRAPHICS_DEVICE_INVALID;
        }
        break;
    }
    return XR_SUCCESS;
}

void graphics_destroy_session() {
    if (s.d3d11Context) s.d3d11Context->Release();
    if (s.d3d11) s.d3d11->Release();
    if (s.d3d12Queue) s.d3d12Queue->Release();
    if (s.d3d12) s.d3d12->Release();
    if (s.d3d10) s.d3d10->Release();
    if (s.d3d9) s.d3d9->Release();
    if (s.vulkanModule) FreeLibrary(s.vulkanModule);
    s = GraphicsState{};
}

XrResult graphics_enumerate_formats(uint32_t capacity, uint32_t* countOutput,
                                    int64_t* formats) {
    switch (s.api) {
    case GraphicsApi::D3D9: return copy_formats(kD3D9Formats, capacity, countOutput, formats);
    case GraphicsApi::D3D10: return copy_formats(kD3D10Formats, capacity, countOutput, formats);
    case GraphicsApi::D3D11:
    case GraphicsApi::D3D12: return copy_formats(kD3DFormats, capacity, countOutput, formats);
    case GraphicsApi::OpenGL: return copy_formats(kGLFormats, capacity, countOutput, formats);
    case GraphicsApi::Vulkan: return copy_formats(kVkFormats, capacity, countOutput, formats);
    case GraphicsApi::Headless:
        if (countOutput) *countOutput = 0;
        return countOutput ? XR_SUCCESS : XR_ERROR_VALIDATION_FAILURE;
    default: return XR_ERROR_SESSION_LOST;
    }
}

XrResult graphics_create_swapchain(SimSwapchain& sc, const XrSwapchainCreateInfo* info) {
    sc.imageCount = 3;
    const UINT mipCount = info->mipCount ? info->mipCount : 1;
    const UINT sampleCount = info->sampleCount ? info->sampleCount : 1;
    const bool depth = (info->usageFlags & XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) != 0;

    if (s.api == GraphicsApi::D3D9) {
        const DWORD usage = depth ? D3DUSAGE_DEPTHSTENCIL : D3DUSAGE_RENDERTARGET;
        for (uint32_t i = 0; i < sc.imageCount; ++i) {
            HANDLE shared = nullptr;
            HRESULT hr = s.d3d9->CreateTexture(info->width, info->height, mipCount, usage,
                static_cast<D3DFORMAT>(info->format), D3DPOOL_DEFAULT, &sc.d3d9Images[i], &shared);
            // Plain IDirect3DDevice9 devices reject a non-null shared-handle
            // parameter. D3D9Ex accepts it, so prefer sharing and fall back to
            // the same-process texture contract used by classic D3D9.
            if (FAILED(hr)) {
                shared = nullptr;
                hr = s.d3d9->CreateTexture(info->width, info->height, mipCount, usage,
                    static_cast<D3DFORMAT>(info->format), D3DPOOL_DEFAULT,
                    &sc.d3d9Images[i], nullptr);
            }
            if (FAILED(hr)) return XR_ERROR_SWAPCHAIN_FORMAT_UNSUPPORTED;
            sc.d3d9SharedHandles[i] = shared;
        }
        return XR_SUCCESS;
    }

    if (s.api == GraphicsApi::D3D10) {
        D3D10_TEXTURE2D_DESC d{};
        d.Width = info->width;
        d.Height = info->height;
        d.MipLevels = mipCount;
        d.ArraySize = 1;
        d.Format = static_cast<DXGI_FORMAT>(info->format);
        d.SampleDesc.Count = sampleCount;
        d.Usage = D3D10_USAGE_DEFAULT;
        d.BindFlags = depth ? D3D10_BIND_DEPTH_STENCIL :
            (D3D10_BIND_RENDER_TARGET | D3D10_BIND_SHADER_RESOURCE);
        for (uint32_t i = 0; i < sc.imageCount; ++i)
            if (FAILED(s.d3d10->CreateTexture2D(&d, nullptr, &sc.d3d10Images[i])))
                return XR_ERROR_SWAPCHAIN_FORMAT_UNSUPPORTED;
        return XR_SUCCESS;
    }

    if (s.api == GraphicsApi::D3D11) {
        D3D11_TEXTURE2D_DESC d{};
        d.Width = info->width;
        d.Height = info->height;
        d.MipLevels = mipCount;
        d.ArraySize = 1;
        d.Format = static_cast<DXGI_FORMAT>(info->format);
        d.SampleDesc.Count = sampleCount;
        d.Usage = D3D11_USAGE_DEFAULT;
        d.BindFlags = depth ? D3D11_BIND_DEPTH_STENCIL :
            (D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE);
        for (uint32_t i = 0; i < sc.imageCount; ++i)
            if (FAILED(s.d3d11->CreateTexture2D(&d, nullptr, &sc.d3d11Images[i])))
                return XR_ERROR_SWAPCHAIN_FORMAT_UNSUPPORTED;
        return XR_SUCCESS;
    }

    if (s.api == GraphicsApi::D3D12) {
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        heap.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
        heap.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
        heap.CreationNodeMask = 1;
        heap.VisibleNodeMask = 1;
        D3D12_RESOURCE_DESC d{};
        d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        d.Width = info->width;
        d.Height = info->height;
        d.DepthOrArraySize = 1;
        d.MipLevels = static_cast<UINT16>(mipCount);
        d.Format = static_cast<DXGI_FORMAT>(info->format);
        d.SampleDesc.Count = sampleCount;
        d.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        d.Flags = depth ? D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL :
                          D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        for (uint32_t i = 0; i < sc.imageCount; ++i)
            if (FAILED(s.d3d12->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &d,
                    D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&sc.d3d12Images[i]))))
                return XR_ERROR_SWAPCHAIN_FORMAT_UNSUPPORTED;
        return XR_SUCCESS;
    }

    if (s.api == GraphicsApi::OpenGL) {
        GlContextGuard guard;
        if (!guard.ok()) return XR_ERROR_GRAPHICS_DEVICE_INVALID;
        glGenTextures(sc.imageCount, sc.glImages);
        GLenum external = GL_RGBA;
        GLenum type = GL_UNSIGNED_BYTE;
        if (info->format == GL_RGBA16F) type = GL_HALF_FLOAT;
        if (info->format == GL_DEPTH_COMPONENT32F) { external = GL_DEPTH_COMPONENT; type = GL_FLOAT; }
        if (info->format == GL_DEPTH24_STENCIL8) { external = GL_DEPTH_STENCIL; type = GL_UNSIGNED_INT_24_8; }
        for (uint32_t i = 0; i < sc.imageCount; ++i) {
            glBindTexture(GL_TEXTURE_2D, sc.glImages[i]);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glTexImage2D(GL_TEXTURE_2D, 0, static_cast<GLint>(info->format), info->width,
                         info->height, 0, external, type, nullptr);
        }
        glBindTexture(GL_TEXTURE_2D, 0);
        return glGetError() == GL_NO_ERROR ? XR_SUCCESS : XR_ERROR_SWAPCHAIN_FORMAT_UNSUPPORTED;
    }

    if (s.api == GraphicsApi::Vulkan) {
        VkImageUsageFlags usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        if (depth) usage |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
        else usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ci.imageType = VK_IMAGE_TYPE_2D;
        ci.format = static_cast<VkFormat>(info->format);
        ci.extent = {info->width, info->height, 1};
        ci.mipLevels = mipCount;
        ci.arrayLayers = 1;
        ci.samples = static_cast<VkSampleCountFlagBits>(sampleCount);
        ci.tiling = VK_IMAGE_TILING_OPTIMAL;
        ci.usage = usage;
        ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        for (uint32_t i = 0; i < sc.imageCount; ++i) {
            if (s.vkCreateImage(s.vkDevice, &ci, nullptr, &sc.vkImages[i]) != VK_SUCCESS)
                return XR_ERROR_SWAPCHAIN_FORMAT_UNSUPPORTED;
            VkMemoryRequirements mr{};
            s.vkGetImageMemoryRequirements(s.vkDevice, sc.vkImages[i], &mr);
            const uint32_t typeIndex = find_vk_memory_type(mr.memoryTypeBits,
                                                            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
            if (typeIndex == UINT32_MAX) return XR_ERROR_RUNTIME_FAILURE;
            VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
            ai.allocationSize = mr.size;
            ai.memoryTypeIndex = typeIndex;
            if (s.vkAllocateMemory(s.vkDevice, &ai, nullptr, &sc.vkMemory[i]) != VK_SUCCESS ||
                s.vkBindImageMemory(s.vkDevice, sc.vkImages[i], sc.vkMemory[i], 0) != VK_SUCCESS)
                return XR_ERROR_RUNTIME_FAILURE;
        }
        return XR_SUCCESS;
    }

    return XR_ERROR_FEATURE_UNSUPPORTED;
}

void graphics_destroy_swapchain(SimSwapchain& sc) {
    if (s.api == GraphicsApi::OpenGL) {
        GlContextGuard guard;
        if (guard.ok()) glDeleteTextures(sc.imageCount, sc.glImages);
    }
    for (uint32_t i = 0; i < sc.imageCount; ++i) {
        if (sc.d3d9Images[i]) sc.d3d9Images[i]->Release();
        if (sc.d3d10Images[i]) sc.d3d10Images[i]->Release();
        if (sc.d3d11Images[i]) sc.d3d11Images[i]->Release();
        if (sc.d3d12Images[i]) sc.d3d12Images[i]->Release();
        if (sc.vkImages[i] && s.vkDestroyImage) s.vkDestroyImage(s.vkDevice, sc.vkImages[i], nullptr);
        if (sc.vkMemory[i] && s.vkFreeMemory) s.vkFreeMemory(s.vkDevice, sc.vkMemory[i], nullptr);
    }
}

XrResult graphics_enumerate_images(SimSwapchain& sc, uint32_t capacity,
                                   XrSwapchainImageBaseHeader* images) {
    if (capacity < sc.imageCount || !images) return XR_ERROR_SIZE_INSUFFICIENT;
    for (uint32_t i = 0; i < sc.imageCount; ++i) {
        switch (s.api) {
        case GraphicsApi::D3D9: {
            auto* out = reinterpret_cast<XrSwapchainImageD3D9XRSIM*>(images);
            if (out[i].type != XR_TYPE_SWAPCHAIN_IMAGE_D3D9_XRSIM)
                return XR_ERROR_VALIDATION_FAILURE;
            out[i].texture = sc.d3d9Images[i];
            out[i].sharedHandle = sc.d3d9SharedHandles[i];
            break;
        }
        case GraphicsApi::D3D10: {
            auto* out = reinterpret_cast<XrSwapchainImageD3D10XRSIM*>(images);
            if (out[i].type != XR_TYPE_SWAPCHAIN_IMAGE_D3D10_XRSIM)
                return XR_ERROR_VALIDATION_FAILURE;
            out[i].texture = sc.d3d10Images[i];
            break;
        }
        case GraphicsApi::D3D11: {
            auto* out = reinterpret_cast<XrSwapchainImageD3D11KHR*>(images);
            if (out[i].type != XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR)
                return XR_ERROR_VALIDATION_FAILURE;
            out[i].texture = sc.d3d11Images[i];
            break;
        }
        case GraphicsApi::D3D12: {
            auto* out = reinterpret_cast<XrSwapchainImageD3D12KHR*>(images);
            if (out[i].type != XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR)
                return XR_ERROR_VALIDATION_FAILURE;
            out[i].texture = sc.d3d12Images[i];
            break;
        }
        case GraphicsApi::OpenGL: {
            auto* out = reinterpret_cast<XrSwapchainImageOpenGLKHR*>(images);
            if (out[i].type != XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_KHR)
                return XR_ERROR_VALIDATION_FAILURE;
            out[i].image = sc.glImages[i];
            break;
        }
        case GraphicsApi::Vulkan: {
            auto* out = reinterpret_cast<XrSwapchainImageVulkanKHR*>(images);
            if (out[i].type != XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR)
                return XR_ERROR_VALIDATION_FAILURE;
            out[i].image = sc.vkImages[i];
            break;
        }
        default: return XR_ERROR_FEATURE_UNSUPPORTED;
        }
    }
    return XR_SUCCESS;
}

} // namespace xrsim
