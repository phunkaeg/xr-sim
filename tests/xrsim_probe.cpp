#ifndef NOMINMAX
#define NOMINMAX
#endif
#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_D3D11
#define XR_USE_GRAPHICS_API_D3D12
#define XR_USE_GRAPHICS_API_OPENGL
#define XR_USE_GRAPHICS_API_VULKAN

#include <windows.h>
#include <d3d9.h>
#include <d3d10_1.h>
#include <d3d11.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <gl/GL.h>
#include <vulkan/vulkan.h>

#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <openxr/openxr_loader_negotiation.h>

#include <xrsim/xrsim_extensions.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace {

struct Api {
    HMODULE runtime = nullptr;
    PFN_xrGetInstanceProcAddr gipa = nullptr;
    XrInstance instance = XR_NULL_HANDLE;

    template <typename T>
    T get(const char* name) const {
        PFN_xrVoidFunction fn = nullptr;
        if (!gipa || XR_FAILED(gipa(instance, name, &fn))) return nullptr;
        return reinterpret_cast<T>(fn);
    }
};

struct Graphics {
    std::string name;
    const char* extension = nullptr;
    const void* binding = nullptr;
    XrStructureType imageType = XR_TYPE_UNKNOWN;
    IDirect3D9* d3d9 = nullptr;
    IDirect3DDevice9* d3d9Device = nullptr;
    ID3D10Device1* d3d10 = nullptr;
    ID3D11Device* d3d11 = nullptr;
    ID3D12Device* d3d12 = nullptr;
    ID3D12CommandQueue* d3d12Queue = nullptr;
    IDXGIFactory4* dxgiFactory = nullptr;
    HWND window = nullptr;
    HDC glDC = nullptr;
    HGLRC glRC = nullptr;
    HMODULE vkModule = nullptr;
    VkInstance vkInstance = VK_NULL_HANDLE;
    VkDevice vkDevice = VK_NULL_HANDLE;
    PFN_vkDestroyInstance vkDestroyInstance = nullptr;
    PFN_vkDestroyDevice vkDestroyDevice = nullptr;

    XrGraphicsBindingD3D9XRSIM b9{XR_TYPE_GRAPHICS_BINDING_D3D9_XRSIM};
    XrGraphicsBindingD3D10XRSIM b10{XR_TYPE_GRAPHICS_BINDING_D3D10_XRSIM};
    XrGraphicsBindingD3D11KHR b11{XR_TYPE_GRAPHICS_BINDING_D3D11_KHR};
    XrGraphicsBindingD3D12KHR b12{XR_TYPE_GRAPHICS_BINDING_D3D12_KHR};
    XrGraphicsBindingOpenGLWin32KHR bgl{XR_TYPE_GRAPHICS_BINDING_OPENGL_WIN32_KHR};
    XrGraphicsBindingVulkanKHR bvk{XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR};

    ~Graphics() {
        if (glRC) { wglMakeCurrent(nullptr, nullptr); wglDeleteContext(glRC); }
        if (glDC && window) ReleaseDC(window, glDC);
        if (window) DestroyWindow(window);
        if (vkDevice && vkDestroyDevice) vkDestroyDevice(vkDevice, nullptr);
        if (vkInstance && vkDestroyInstance) vkDestroyInstance(vkInstance, nullptr);
        if (vkModule) FreeLibrary(vkModule);
        if (d3d12Queue) d3d12Queue->Release();
        if (d3d12) d3d12->Release();
        if (dxgiFactory) dxgiFactory->Release();
        if (d3d11) d3d11->Release();
        if (d3d10) d3d10->Release();
        if (d3d9Device) d3d9Device->Release();
        if (d3d9) d3d9->Release();
    }
};

bool make_window(Graphics& g) {
    static const wchar_t* cls = L"XrSimProbeWindow";
    WNDCLASSW wc{};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = cls;
    RegisterClassW(&wc);
    g.window = CreateWindowW(cls, L"xr-sim probe", WS_OVERLAPPEDWINDOW,
                             0, 0, 64, 64, nullptr, nullptr, wc.hInstance, nullptr);
    return g.window != nullptr;
}

bool init_d3d9(Graphics& g) {
    if (!make_window(g)) {
        std::fprintf(stderr, "D3D9 probe: CreateWindow failed (%lu)\n", GetLastError());
        return false;
    }
    g.d3d9 = Direct3DCreate9(D3D_SDK_VERSION);
    if (!g.d3d9) {
        std::fprintf(stderr, "D3D9 probe: Direct3DCreate9 returned null\n");
        return false;
    }
    D3DPRESENT_PARAMETERS pp{};
    pp.Windowed = TRUE;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.hDeviceWindow = g.window;
    pp.BackBufferFormat = D3DFMT_X8R8G8B8;
    pp.BackBufferWidth = 64;
    pp.BackBufferHeight = 64;
    pp.BackBufferCount = 1;
    const HRESULT h0 = g.d3d9->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, g.window,
        D3DCREATE_HARDWARE_VERTEXPROCESSING | D3DCREATE_MULTITHREADED, &pp, &g.d3d9Device);
    HRESULT h1 = S_OK, h2 = S_OK, h3 = S_OK;
    if (FAILED(h0)) h1 = g.d3d9->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, g.window,
        D3DCREATE_SOFTWARE_VERTEXPROCESSING | D3DCREATE_MULTITHREADED, &pp, &g.d3d9Device);
    if (FAILED(h0) && FAILED(h1)) h2 = g.d3d9->CreateDevice(
        D3DADAPTER_DEFAULT, D3DDEVTYPE_REF, g.window,
        D3DCREATE_SOFTWARE_VERTEXPROCESSING | D3DCREATE_MULTITHREADED, &pp, &g.d3d9Device);
    if (FAILED(h0) && FAILED(h1) && FAILED(h2)) h3 = g.d3d9->CreateDevice(
        D3DADAPTER_DEFAULT, D3DDEVTYPE_NULLREF, g.window,
        D3DCREATE_SOFTWARE_VERTEXPROCESSING | D3DCREATE_MULTITHREADED, &pp, &g.d3d9Device);
    if (!g.d3d9Device) {
        std::fprintf(stderr, "D3D9 probe: CreateDevice HRESULTs %08lx %08lx %08lx %08lx\n",
                     static_cast<unsigned long>(h0), static_cast<unsigned long>(h1),
                     static_cast<unsigned long>(h2), static_cast<unsigned long>(h3));
        return false;
    }
    g.extension = XR_XRSIM_D3D9_ENABLE_EXTENSION_NAME;
    g.b9.device = g.d3d9Device;
    g.binding = &g.b9;
    g.imageType = XR_TYPE_SWAPCHAIN_IMAGE_D3D9_XRSIM;
    return true;
}

bool init_d3d10(Graphics& g) {
    if (FAILED(D3D10CreateDevice1(nullptr, D3D10_DRIVER_TYPE_HARDWARE, nullptr, 0,
            D3D10_FEATURE_LEVEL_10_0, D3D10_1_SDK_VERSION, &g.d3d10)) &&
        FAILED(D3D10CreateDevice1(nullptr, D3D10_DRIVER_TYPE_WARP, nullptr, 0,
            D3D10_FEATURE_LEVEL_10_0, D3D10_1_SDK_VERSION, &g.d3d10))) return false;
    g.extension = XR_XRSIM_D3D10_ENABLE_EXTENSION_NAME;
    g.b10.device = g.d3d10;
    g.binding = &g.b10;
    g.imageType = XR_TYPE_SWAPCHAIN_IMAGE_D3D10_XRSIM;
    return true;
}

bool init_d3d11(Graphics& g) {
    D3D_FEATURE_LEVEL level{};
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
            D3D11_SDK_VERSION, &g.d3d11, &level, nullptr)) &&
        FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0,
            D3D11_SDK_VERSION, &g.d3d11, &level, nullptr))) return false;
    g.extension = XR_KHR_D3D11_ENABLE_EXTENSION_NAME;
    g.b11.device = g.d3d11;
    g.binding = &g.b11;
    g.imageType = XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR;
    return true;
}

bool init_d3d12(Graphics& g) {
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&g.dxgiFactory)))) return false;
    IDXGIAdapter1* adapter = nullptr;
    SIZE_T bestMemory = 0;
    for (UINT i = 0;; ++i) {
        IDXGIAdapter1* candidate = nullptr;
        if (g.dxgiFactory->EnumAdapters1(i, &candidate) != S_OK) break;
        DXGI_ADAPTER_DESC1 desc{};
        candidate->GetDesc1(&desc);
        if (!(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) && desc.DedicatedVideoMemory >= bestMemory) {
            if (adapter) adapter->Release();
            adapter = candidate;
            bestMemory = desc.DedicatedVideoMemory;
        } else {
            candidate->Release();
        }
    }
    if (!adapter) g.dxgiFactory->EnumWarpAdapter(IID_PPV_ARGS(&adapter));
    HRESULT hr = D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&g.d3d12));
    if (adapter) adapter->Release();
    if (FAILED(hr)) return false;
    D3D12_COMMAND_QUEUE_DESC q{};
    q.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (FAILED(g.d3d12->CreateCommandQueue(&q, IID_PPV_ARGS(&g.d3d12Queue)))) return false;
    g.extension = XR_KHR_D3D12_ENABLE_EXTENSION_NAME;
    g.b12.device = g.d3d12;
    g.b12.queue = g.d3d12Queue;
    g.binding = &g.b12;
    g.imageType = XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR;
    return true;
}

bool init_opengl(Graphics& g) {
    if (!make_window(g)) return false;
    g.glDC = GetDC(g.window);
    PIXELFORMATDESCRIPTOR pfd{};
    pfd.nSize = sizeof(pfd);
    pfd.nVersion = 1;
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA;
    pfd.cColorBits = 32;
    pfd.cDepthBits = 24;
    const int pf = ChoosePixelFormat(g.glDC, &pfd);
    if (!pf || !SetPixelFormat(g.glDC, pf, &pfd)) return false;
    g.glRC = wglCreateContext(g.glDC);
    if (!g.glRC || !wglMakeCurrent(g.glDC, g.glRC)) return false;
    g.extension = XR_KHR_OPENGL_ENABLE_EXTENSION_NAME;
    g.bgl.hDC = g.glDC;
    g.bgl.hGLRC = g.glRC;
    g.binding = &g.bgl;
    g.imageType = XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_KHR;
    return true;
}

bool init_vulkan(Graphics& g) {
    g.vkModule = LoadLibraryW(L"vulkan-1.dll");
    if (!g.vkModule) return false;
    const auto gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(
        GetProcAddress(g.vkModule, "vkGetInstanceProcAddr"));
    if (!gipa) return false;
    const auto createInstance = reinterpret_cast<PFN_vkCreateInstance>(gipa(VK_NULL_HANDLE, "vkCreateInstance"));
    if (!createInstance) return false;
    VkApplicationInfo ai{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    ai.pApplicationName = "xrsim_probe";
    ai.apiVersion = VK_API_VERSION_1_0;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &ai;
    if (createInstance(&ici, nullptr, &g.vkInstance) != VK_SUCCESS) return false;
    const auto enumerate = reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(
        gipa(g.vkInstance, "vkEnumeratePhysicalDevices"));
    const auto getQueues = reinterpret_cast<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(
        gipa(g.vkInstance, "vkGetPhysicalDeviceQueueFamilyProperties"));
    const auto createDevice = reinterpret_cast<PFN_vkCreateDevice>(gipa(g.vkInstance, "vkCreateDevice"));
    g.vkDestroyInstance = reinterpret_cast<PFN_vkDestroyInstance>(gipa(g.vkInstance, "vkDestroyInstance"));
    if (!enumerate || !getQueues || !createDevice) return false;
    uint32_t count = 0;
    if (enumerate(g.vkInstance, &count, nullptr) != VK_SUCCESS || count == 0) return false;
    std::vector<VkPhysicalDevice> physical(count);
    if (enumerate(g.vkInstance, &count, physical.data()) != VK_SUCCESS) return false;
    uint32_t qCount = 0;
    getQueues(physical[0], &qCount, nullptr);
    std::vector<VkQueueFamilyProperties> queues(qCount);
    getQueues(physical[0], &qCount, queues.data());
    uint32_t family = UINT32_MAX;
    for (uint32_t i = 0; i < qCount; ++i)
        if (queues[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) { family = i; break; }
    if (family == UINT32_MAX) return false;
    const float priority = 1.0f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = family;
    qci.queueCount = 1;
    qci.pQueuePriorities = &priority;
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    if (createDevice(physical[0], &dci, nullptr, &g.vkDevice) != VK_SUCCESS) return false;
    const auto gdpa = reinterpret_cast<PFN_vkGetDeviceProcAddr>(gipa(g.vkInstance, "vkGetDeviceProcAddr"));
    g.vkDestroyDevice = gdpa ? reinterpret_cast<PFN_vkDestroyDevice>(gdpa(g.vkDevice, "vkDestroyDevice")) : nullptr;
    g.extension = XR_KHR_VULKAN_ENABLE_EXTENSION_NAME;
    g.bvk.instance = g.vkInstance;
    g.bvk.physicalDevice = physical[0];
    g.bvk.device = g.vkDevice;
    g.bvk.queueFamilyIndex = family;
    g.bvk.queueIndex = 0;
    g.binding = &g.bvk;
    g.imageType = XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR;
    return true;
}

bool init_graphics(Graphics& g, const std::string& name) {
    g.name = name;
    if (name == "headless") { g.extension = XR_MND_HEADLESS_EXTENSION_NAME; return true; }
    if (name == "d3d9") return init_d3d9(g);
    if (name == "d3d10") return init_d3d10(g);
    if (name == "d3d11") return init_d3d11(g);
    if (name == "d3d12") return init_d3d12(g);
    if (name == "opengl") return init_opengl(g);
    if (name == "vulkan") return init_vulkan(g);
    return false;
}

bool enumerate_images(const Api& api, XrSwapchain swapchain, const Graphics& g, uint32_t count) {
    const auto enumerate = api.get<PFN_xrEnumerateSwapchainImages>("xrEnumerateSwapchainImages");
    if (!enumerate) return false;
    XrResult result = XR_ERROR_RUNTIME_FAILURE;
    if (g.name == "d3d9") {
        std::vector<XrSwapchainImageD3D9XRSIM> v(count, {XR_TYPE_SWAPCHAIN_IMAGE_D3D9_XRSIM});
        result = enumerate(swapchain, count, &count, reinterpret_cast<XrSwapchainImageBaseHeader*>(v.data()));
    } else if (g.name == "d3d10") {
        std::vector<XrSwapchainImageD3D10XRSIM> v(count, {XR_TYPE_SWAPCHAIN_IMAGE_D3D10_XRSIM});
        result = enumerate(swapchain, count, &count, reinterpret_cast<XrSwapchainImageBaseHeader*>(v.data()));
    } else if (g.name == "d3d11") {
        std::vector<XrSwapchainImageD3D11KHR> v(count, {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
        result = enumerate(swapchain, count, &count, reinterpret_cast<XrSwapchainImageBaseHeader*>(v.data()));
    } else if (g.name == "d3d12") {
        std::vector<XrSwapchainImageD3D12KHR> v(count, {XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR});
        result = enumerate(swapchain, count, &count, reinterpret_cast<XrSwapchainImageBaseHeader*>(v.data()));
    } else if (g.name == "opengl") {
        std::vector<XrSwapchainImageOpenGLKHR> v(count, {XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_KHR});
        result = enumerate(swapchain, count, &count, reinterpret_cast<XrSwapchainImageBaseHeader*>(v.data()));
    } else if (g.name == "vulkan") {
        std::vector<XrSwapchainImageVulkanKHR> v(count, {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR});
        result = enumerate(swapchain, count, &count, reinterpret_cast<XrSwapchainImageBaseHeader*>(v.data()));
    }
    return XR_SUCCEEDED(result);
}

XrResult call_graphics_requirements(const Api& api, const Graphics& g, XrSystemId system) {
    if (g.name == "headless") return XR_SUCCESS;
    if (g.name == "d3d9") {
        XrGraphicsRequirementsD3D9XRSIM r{XR_TYPE_GRAPHICS_REQUIREMENTS_D3D9_XRSIM};
        const auto fn = api.get<PFN_xrGetD3D9GraphicsRequirementsXRSIM>(
            "xrGetD3D9GraphicsRequirementsXRSIM");
        return fn ? fn(api.instance, system, &r) : XR_ERROR_FUNCTION_UNSUPPORTED;
    }
    if (g.name == "d3d10") {
        XrGraphicsRequirementsD3D10XRSIM r{XR_TYPE_GRAPHICS_REQUIREMENTS_D3D10_XRSIM};
        const auto fn = api.get<PFN_xrGetD3D10GraphicsRequirementsXRSIM>(
            "xrGetD3D10GraphicsRequirementsXRSIM");
        return fn ? fn(api.instance, system, &r) : XR_ERROR_FUNCTION_UNSUPPORTED;
    }
    if (g.name == "d3d11") {
        XrGraphicsRequirementsD3D11KHR r{XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR};
        const auto fn = api.get<PFN_xrGetD3D11GraphicsRequirementsKHR>(
            "xrGetD3D11GraphicsRequirementsKHR");
        return fn ? fn(api.instance, system, &r) : XR_ERROR_FUNCTION_UNSUPPORTED;
    }
    if (g.name == "d3d12") {
        XrGraphicsRequirementsD3D12KHR r{XR_TYPE_GRAPHICS_REQUIREMENTS_D3D12_KHR};
        const auto fn = api.get<PFN_xrGetD3D12GraphicsRequirementsKHR>(
            "xrGetD3D12GraphicsRequirementsKHR");
        return fn ? fn(api.instance, system, &r) : XR_ERROR_FUNCTION_UNSUPPORTED;
    }
    if (g.name == "opengl") {
        XrGraphicsRequirementsOpenGLKHR r{XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_KHR};
        const auto fn = api.get<PFN_xrGetOpenGLGraphicsRequirementsKHR>(
            "xrGetOpenGLGraphicsRequirementsKHR");
        return fn ? fn(api.instance, system, &r) : XR_ERROR_FUNCTION_UNSUPPORTED;
    }
    if (g.name == "vulkan") {
        XrGraphicsRequirementsVulkanKHR r{XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN_KHR};
        const auto fn = api.get<PFN_xrGetVulkanGraphicsRequirementsKHR>(
            "xrGetVulkanGraphicsRequirementsKHR");
        return fn ? fn(api.instance, system, &r) : XR_ERROR_FUNCTION_UNSUPPORTED;
    }
    return XR_ERROR_FUNCTION_UNSUPPORTED;
}

int fail(const char* message, XrResult result = XR_SUCCESS) {
    if (result == XR_SUCCESS) std::fprintf(stderr, "FAIL: %s\n", message);
    else std::fprintf(stderr, "FAIL: %s (XrResult %d)\n", message, result);
    return 1;
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 3) {
        std::fprintf(stderr, "usage: xrsim_probe <xrsim.dll> <headless|d3d9|d3d10|d3d11|d3d12|opengl|vulkan>\n");
        return 2;
    }

    Graphics graphics;
    if (!init_graphics(graphics, argv[2])) {
        std::fprintf(stderr, "SKIP: graphics backend '%s' is unavailable on this machine\n", argv[2]);
        return 77;
    }

    Api api;
    api.runtime = LoadLibraryA(argv[1]);
    if (!api.runtime) return fail("LoadLibrary failed");
    const auto negotiate = reinterpret_cast<PFN_xrNegotiateLoaderRuntimeInterface>(
        GetProcAddress(api.runtime, "xrNegotiateLoaderRuntimeInterface"));
    if (!negotiate) return fail("negotiate export missing");
    XrNegotiateLoaderInfo loader{};
    loader.structType = XR_LOADER_INTERFACE_STRUCT_LOADER_INFO;
    loader.structVersion = XR_LOADER_INFO_STRUCT_VERSION;
    loader.structSize = sizeof(loader);
    loader.minInterfaceVersion = 1;
    loader.maxInterfaceVersion = XR_CURRENT_LOADER_RUNTIME_VERSION;
    loader.minApiVersion = XR_MAKE_VERSION(1, 0, 0);
    loader.maxApiVersion = XR_CURRENT_API_VERSION;
    XrNegotiateRuntimeRequest request{};
    request.structType = XR_LOADER_INTERFACE_STRUCT_RUNTIME_REQUEST;
    request.structVersion = XR_RUNTIME_INFO_STRUCT_VERSION;
    request.structSize = sizeof(request);
    XrResult xr = negotiate(&loader, &request);
    if (XR_FAILED(xr) || !request.getInstanceProcAddr) return fail("runtime negotiation", xr);
    api.gipa = request.getInstanceProcAddr;

    PFN_xrVoidFunction createRaw = nullptr;
    xr = api.gipa(XR_NULL_HANDLE, "xrCreateInstance", &createRaw);
    if (XR_FAILED(xr) || !createRaw) return fail("resolve xrCreateInstance", xr);
    const auto createInstance = reinterpret_cast<PFN_xrCreateInstance>(createRaw);
    XrInstanceCreateInfo ici{XR_TYPE_INSTANCE_CREATE_INFO};
    strcpy_s(ici.applicationInfo.applicationName, "xrsim_probe");
    strcpy_s(ici.applicationInfo.engineName, "direct-runtime-test");
    ici.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 34);
    ici.enabledExtensionCount = 1;
    ici.enabledExtensionNames = &graphics.extension;
    xr = createInstance(&ici, &api.instance);
    if (XR_FAILED(xr)) return fail("xrCreateInstance", xr);

    const auto getSystem = api.get<PFN_xrGetSystem>("xrGetSystem");
    XrSystemGetInfo sgi{XR_TYPE_SYSTEM_GET_INFO};
    sgi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    XrSystemId system = XR_NULL_SYSTEM_ID;
    xr = getSystem(api.instance, &sgi, &system);
    if (XR_FAILED(xr)) return fail("xrGetSystem", xr);
    xr = call_graphics_requirements(api, graphics, system);
    if (XR_FAILED(xr)) return fail("graphics requirements", xr);

    const auto createSession = api.get<PFN_xrCreateSession>("xrCreateSession");
    XrSessionCreateInfo sci{XR_TYPE_SESSION_CREATE_INFO};
    sci.next = graphics.binding;
    sci.systemId = system;
    XrSession session = XR_NULL_HANDLE;
    xr = createSession(api.instance, &sci, &session);
    if (XR_FAILED(xr)) return fail("xrCreateSession", xr);

    XrSwapchain swapchain = XR_NULL_HANDLE;
    if (graphics.name != "headless") {
        const auto enumFormats = api.get<PFN_xrEnumerateSwapchainFormats>("xrEnumerateSwapchainFormats");
        uint32_t formatCount = 0;
        xr = enumFormats(session, 0, &formatCount, nullptr);
        if (XR_FAILED(xr) || formatCount == 0) return fail("enumerate formats", xr);
        std::vector<int64_t> formats(formatCount);
        xr = enumFormats(session, formatCount, &formatCount, formats.data());
        if (XR_FAILED(xr)) return fail("read formats", xr);
        XrSwapchainCreateInfo ci{XR_TYPE_SWAPCHAIN_CREATE_INFO};
        ci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
        ci.format = formats[0];
        ci.sampleCount = 1;
        ci.width = 64;
        ci.height = 64;
        ci.faceCount = 1;
        ci.arraySize = 1;
        ci.mipCount = 1;
        xr = api.get<PFN_xrCreateSwapchain>("xrCreateSwapchain")(session, &ci, &swapchain);
        if (XR_FAILED(xr)) return fail("xrCreateSwapchain", xr);
        uint32_t imageCount = 0;
        xr = api.get<PFN_xrEnumerateSwapchainImages>("xrEnumerateSwapchainImages")(
            swapchain, 0, &imageCount, nullptr);
        if (XR_FAILED(xr) || imageCount != 3 || !enumerate_images(api, swapchain, graphics, imageCount))
            return fail("enumerate swapchain images", xr);
    }

    const auto poll = api.get<PFN_xrPollEvent>("xrPollEvent");
    bool ready = false;
    for (int i = 0; i < 100 && !ready; ++i) {
        XrEventDataBuffer event{XR_TYPE_EVENT_DATA_BUFFER};
        while (poll(api.instance, &event) == XR_SUCCESS) {
            if (event.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
                const auto* changed = reinterpret_cast<const XrEventDataSessionStateChanged*>(&event);
                ready = changed->state == XR_SESSION_STATE_READY;
            }
            event = XrEventDataBuffer{XR_TYPE_EVENT_DATA_BUFFER};
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (!ready) return fail("session never became READY");

    XrSessionBeginInfo begin{XR_TYPE_SESSION_BEGIN_INFO};
    begin.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    xr = api.get<PFN_xrBeginSession>("xrBeginSession")(session, &begin);
    if (XR_FAILED(xr)) return fail("xrBeginSession", xr);

    for (int frame = 0; frame < 3; ++frame) {
        XrFrameWaitInfo wi{XR_TYPE_FRAME_WAIT_INFO};
        XrFrameState state{XR_TYPE_FRAME_STATE};
        xr = api.get<PFN_xrWaitFrame>("xrWaitFrame")(session, &wi, &state);
        if (XR_FAILED(xr)) return fail("xrWaitFrame", xr);
        XrFrameBeginInfo bi{XR_TYPE_FRAME_BEGIN_INFO};
        xr = api.get<PFN_xrBeginFrame>("xrBeginFrame")(session, &bi);
        if (XR_FAILED(xr)) return fail("xrBeginFrame", xr);
        if (swapchain != XR_NULL_HANDLE) {
            uint32_t index = 0;
            XrSwapchainImageAcquireInfo ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
            xr = api.get<PFN_xrAcquireSwapchainImage>("xrAcquireSwapchainImage")(swapchain, &ai, &index);
            if (XR_FAILED(xr)) return fail("xrAcquireSwapchainImage", xr);
            XrSwapchainImageWaitInfo swi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
            swi.timeout = 1000000000;
            xr = api.get<PFN_xrWaitSwapchainImage>("xrWaitSwapchainImage")(swapchain, &swi);
            if (XR_FAILED(xr)) return fail("xrWaitSwapchainImage", xr);
            XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
            xr = api.get<PFN_xrReleaseSwapchainImage>("xrReleaseSwapchainImage")(swapchain, &ri);
            if (XR_FAILED(xr)) return fail("xrReleaseSwapchainImage", xr);
        }
        XrFrameEndInfo ei{XR_TYPE_FRAME_END_INFO};
        ei.displayTime = state.predictedDisplayTime;
        ei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
        xr = api.get<PFN_xrEndFrame>("xrEndFrame")(session, &ei);
        if (XR_FAILED(xr)) return fail("xrEndFrame", xr);
    }

    if (swapchain) api.get<PFN_xrDestroySwapchain>("xrDestroySwapchain")(swapchain);
    api.get<PFN_xrDestroySession>("xrDestroySession")(session);
    api.get<PFN_xrDestroyInstance>("xrDestroyInstance")(api.instance);
    api.instance = XR_NULL_HANDLE;
    FreeLibrary(api.runtime);
    std::printf("PASS: %s (%s) exercised instance, session, swapchain, and frame lifecycle\n",
                graphics.name.c_str(), sizeof(void*) == 8 ? "x64" : "x86");
    return 0;
}
