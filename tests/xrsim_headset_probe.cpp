// Direct boot-profile regression probe. Each case is a separate process so the
// runtime's deliberately process-frozen profile cannot leak across cases.

#define XR_USE_PLATFORM_WIN32
#include <windows.h>

#include <openxr/openxr.h>
#include <openxr/openxr_loader_negotiation.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <string>
#include <thread>

namespace {

int fail(const char* what, XrResult result = XR_ERROR_RUNTIME_FAILURE) {
    std::fprintf(stderr, "FAIL: %s (XrResult %d)\n", what, static_cast<int>(result));
    return 1;
}

bool floats_close(float a, float b) { return std::fabs(a - b) < 1e-5f; }

std::wstring state_path(const wchar_t* leaf) {
    wchar_t dir[32768] = {};
    const DWORD n = GetEnvironmentVariableW(L"XRSIM_DIR", dir, static_cast<DWORD>(std::size(dir)));
    if (n == 0 || n >= std::size(dir)) return {};
    std::wstring result(dir);
    result += L"\\";
    result += leaf;
    return result;
}

bool write_text(const std::wstring& path, const char* text) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    const DWORD length = static_cast<DWORD>(std::strlen(text));
    DWORD written = 0;
    const BOOL ok = WriteFile(file, text, length, &written, nullptr);
    CloseHandle(file);
    return ok && written == length;
}

const char* valid_profile() {
    return R"json({
  "schemaVersion": 1,
  "systemName": "Configured HMD",
  "views": [
    {
      "recommendedImageRectWidth": 1832,
      "recommendedImageRectHeight": 1920,
      "maxImageRectWidth": 4096,
      "maxImageRectHeight": 4100,
      "fov": {"angleLeft": -0.91, "angleRight": 0.67, "angleUp": 0.79, "angleDown": -0.83}
    },
    {
      "recommendedImageRectWidth": 1840,
      "recommendedImageRectHeight": 1936,
      "maxImageRectWidth": 4200,
      "maxImageRectHeight": 4300,
      "fov": {"angleLeft": -0.63, "angleRight": 0.94, "angleUp": 0.81, "angleDown": -0.82}
    }
  ]
})json";
}

std::string invalid_profile(const std::string& mode) {
    if (mode == "invalid-json") return "{\"schemaVersion\":1,\"systemName\":\"broken\",\"views\":[";
    std::string text = valid_profile();
    if (mode == "invalid-fov") {
        const std::string from = "\"angleLeft\": -0.91, \"angleRight\": 0.67";
        text.replace(text.find(from), from.size(), "\"angleLeft\": 0.91, \"angleRight\": 0.67");
    } else if (mode == "invalid-nonfinite") {
        const std::string from = "\"angleLeft\": -0.91";
        text.replace(text.find(from), from.size(), "\"angleLeft\": 1e999");
    } else if (mode == "invalid-size") {
        const std::string from = "\"recommendedImageRectWidth\": 1832";
        text.replace(text.find(from), from.size(), "\"recommendedImageRectWidth\": 0");
    }
    return text;
}

template <typename T>
T get_proc(PFN_xrGetInstanceProcAddr gipa, XrInstance instance, const char* name) {
    PFN_xrVoidFunction raw = nullptr;
    if (XR_FAILED(gipa(instance, name, &raw))) return nullptr;
    return reinterpret_cast<T>(raw);
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 3) {
        std::fprintf(stderr, "usage: xrsim_headset_probe <xrsim.dll> <case>\n");
        return 2;
    }
    const std::string mode = argv[2];
    const bool defaultCase = mode == "default";
    const bool validCase = mode == "valid";
    const bool fallbackCase = mode == "fallback";
    const bool profileCase = validCase || fallbackCase;
    const bool invalidCase = mode.rfind("invalid-", 0) == 0;
    if (!defaultCase && !profileCase && !invalidCase) return fail("unknown case");

    const std::wstring configPath = state_path(L"probe-headset.json");
    const std::wstring fallbackPath = state_path(L"headset.json");
    if (configPath.empty() || fallbackPath.empty()) return fail("XRSIM_DIR is missing");
    DeleteFileW(configPath.c_str());
    DeleteFileW(fallbackPath.c_str());
    if (defaultCase) {
        SetEnvironmentVariableW(L"XRSIM_HEADSET_CONFIG", nullptr);
    } else if (fallbackCase) {
        if (!write_text(fallbackPath, valid_profile())) return fail("write fallback profile");
        SetEnvironmentVariableW(L"XRSIM_HEADSET_CONFIG", nullptr);
    } else {
        if (mode != "invalid-missing") {
            const std::string text = validCase ? valid_profile() : invalid_profile(mode);
            if (!write_text(configPath, text.c_str())) return fail("write profile");
        }
        SetEnvironmentVariableW(L"XRSIM_HEADSET_CONFIG", configPath.c_str());
    }

    HMODULE runtime = LoadLibraryA(argv[1]);
    if (!runtime) return fail("LoadLibrary");
    const auto negotiate = reinterpret_cast<PFN_xrNegotiateLoaderRuntimeInterface>(
        GetProcAddress(runtime, "xrNegotiateLoaderRuntimeInterface"));
    if (!negotiate) return fail("negotiate export");

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

    const auto createInstance = get_proc<PFN_xrCreateInstance>(
        request.getInstanceProcAddr, XR_NULL_HANDLE, "xrCreateInstance");
    if (!createInstance) return fail("resolve xrCreateInstance");
    const char* extension = XR_MND_HEADLESS_EXTENSION_NAME;
    XrInstanceCreateInfo createInfo{XR_TYPE_INSTANCE_CREATE_INFO};
    strcpy_s(createInfo.applicationInfo.applicationName, "xrsim_headset_probe");
    strcpy_s(createInfo.applicationInfo.engineName, "direct-runtime-test");
    createInfo.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 34);
    createInfo.enabledExtensionCount = 1;
    createInfo.enabledExtensionNames = &extension;
    XrInstance instance = XR_NULL_HANDLE;
    xr = createInstance(&createInfo, &instance);

    if (invalidCase) {
        if (xr != XR_ERROR_INITIALIZATION_FAILED)
            return fail("invalid profile did not fail instance creation", xr);
        FreeLibrary(runtime);
        std::printf("PASS: %s profile rejected (%s)\n", mode.c_str(), sizeof(void*) == 8 ? "x64" : "x86");
        return 0;
    }
    if (XR_FAILED(xr)) return fail("xrCreateInstance", xr);

    const auto getSystem = get_proc<PFN_xrGetSystem>(request.getInstanceProcAddr, instance, "xrGetSystem");
    const auto getSystemProperties = get_proc<PFN_xrGetSystemProperties>(
        request.getInstanceProcAddr, instance, "xrGetSystemProperties");
    const auto enumerateViews = get_proc<PFN_xrEnumerateViewConfigurationViews>(
        request.getInstanceProcAddr, instance, "xrEnumerateViewConfigurationViews");
    if (!getSystem || !getSystemProperties || !enumerateViews) return fail("resolve system functions");

    XrSystemGetInfo systemInfo{XR_TYPE_SYSTEM_GET_INFO};
    systemInfo.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    XrSystemId system = XR_NULL_SYSTEM_ID;
    xr = getSystem(instance, &systemInfo, &system);
    if (XR_FAILED(xr)) return fail("xrGetSystem", xr);
    XrSystemProperties properties{XR_TYPE_SYSTEM_PROPERTIES};
    xr = getSystemProperties(instance, system, &properties);
    if (XR_FAILED(xr)) return fail("xrGetSystemProperties", xr);

    uint32_t count = 0;
    xr = enumerateViews(instance, system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                        0, &count, nullptr);
    if (XR_FAILED(xr) || count != 2) return fail("view count", xr);
    XrViewConfigurationView configs[2] = {
        {XR_TYPE_VIEW_CONFIGURATION_VIEW}, {XR_TYPE_VIEW_CONFIGURATION_VIEW}};
    xr = enumerateViews(instance, system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                        2, &count, configs);
    if (XR_FAILED(xr)) return fail("view configuration", xr);

    const uint32_t expectedWidth[2] = {profileCase ? 1832u : 2064u, profileCase ? 1840u : 2064u};
    const uint32_t expectedHeight[2] = {profileCase ? 1920u : 2208u, profileCase ? 1936u : 2208u};
    const uint32_t expectedMaxWidth[2] = {profileCase ? 4096u : 16384u, profileCase ? 4200u : 16384u};
    const uint32_t expectedMaxHeight[2] = {profileCase ? 4100u : 16384u, profileCase ? 4300u : 16384u};
    for (uint32_t eye = 0; eye < 2; ++eye) {
        if (configs[eye].recommendedImageRectWidth != expectedWidth[eye] ||
            configs[eye].recommendedImageRectHeight != expectedHeight[eye] ||
            configs[eye].maxImageRectWidth != expectedMaxWidth[eye] ||
            configs[eye].maxImageRectHeight != expectedMaxHeight[eye])
            return fail("configured view dimensions differ");
    }
    if (std::strcmp(properties.systemName, profileCase ? "Configured HMD" : "Meta Quest 3") != 0)
        return fail("configured systemName differs");

    const auto createSession = get_proc<PFN_xrCreateSession>(request.getInstanceProcAddr, instance, "xrCreateSession");
    const auto pollEvent = get_proc<PFN_xrPollEvent>(request.getInstanceProcAddr, instance, "xrPollEvent");
    const auto beginSession = get_proc<PFN_xrBeginSession>(request.getInstanceProcAddr, instance, "xrBeginSession");
    const auto createSpace = get_proc<PFN_xrCreateReferenceSpace>(request.getInstanceProcAddr, instance, "xrCreateReferenceSpace");
    const auto waitFrame = get_proc<PFN_xrWaitFrame>(request.getInstanceProcAddr, instance, "xrWaitFrame");
    const auto locateViews = get_proc<PFN_xrLocateViews>(request.getInstanceProcAddr, instance, "xrLocateViews");
    const auto destroySpace = get_proc<PFN_xrDestroySpace>(request.getInstanceProcAddr, instance, "xrDestroySpace");
    const auto destroySession = get_proc<PFN_xrDestroySession>(request.getInstanceProcAddr, instance, "xrDestroySession");
    const auto destroyInstance = get_proc<PFN_xrDestroyInstance>(request.getInstanceProcAddr, instance, "xrDestroyInstance");
    if (!createSession || !pollEvent || !beginSession || !createSpace || !waitFrame ||
        !locateViews || !destroySpace || !destroySession || !destroyInstance)
        return fail("resolve lifecycle functions");

    XrSessionCreateInfo sessionInfo{XR_TYPE_SESSION_CREATE_INFO};
    sessionInfo.systemId = system;
    XrSession session = XR_NULL_HANDLE;
    xr = createSession(instance, &sessionInfo, &session);
    if (XR_FAILED(xr)) return fail("xrCreateSession", xr);
    bool ready = false;
    for (int attempt = 0; attempt < 100 && !ready; ++attempt) {
        XrEventDataBuffer event{XR_TYPE_EVENT_DATA_BUFFER};
        while (pollEvent(instance, &event) == XR_SUCCESS) {
            if (event.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
                const auto* changed = reinterpret_cast<const XrEventDataSessionStateChanged*>(&event);
                ready = changed->state == XR_SESSION_STATE_READY;
            }
            event = XrEventDataBuffer{XR_TYPE_EVENT_DATA_BUFFER};
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    if (!ready) return fail("session did not reach READY");
    XrSessionBeginInfo beginInfo{XR_TYPE_SESSION_BEGIN_INFO};
    beginInfo.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    xr = beginSession(session, &beginInfo);
    if (XR_FAILED(xr)) return fail("xrBeginSession", xr);

    XrReferenceSpaceCreateInfo spaceInfo{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    spaceInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    spaceInfo.poseInReferenceSpace.orientation.w = 1.0f;
    XrSpace space = XR_NULL_HANDLE;
    xr = createSpace(session, &spaceInfo, &space);
    if (XR_FAILED(xr)) return fail("xrCreateReferenceSpace", xr);
    XrFrameWaitInfo waitInfo{XR_TYPE_FRAME_WAIT_INFO};
    XrFrameState frameState{XR_TYPE_FRAME_STATE};
    xr = waitFrame(session, &waitInfo, &frameState);
    if (XR_FAILED(xr)) return fail("xrWaitFrame", xr);

    XrViewLocateInfo locateInfo{XR_TYPE_VIEW_LOCATE_INFO};
    locateInfo.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    locateInfo.displayTime = frameState.predictedDisplayTime;
    locateInfo.space = space;
    XrViewState viewState{XR_TYPE_VIEW_STATE};
    XrView views[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
    count = 0;
    xr = locateViews(session, &locateInfo, &viewState, 2, &count, views);
    if (XR_FAILED(xr) || count != 2) return fail("xrLocateViews", xr);
    const XrFovf expectedValid[2] = {
        {-0.91f, 0.67f, 0.79f, -0.83f}, {-0.63f, 0.94f, 0.81f, -0.82f}};
    const XrFovf expectedDefault[2] = {
        {-0.9424778f, 0.7679449f, 0.9599311f, -0.9599311f},
        {-0.7679449f, 0.9424778f, 0.9599311f, -0.9599311f}};
    const XrFovf* expected = profileCase ? expectedValid : expectedDefault;
    for (uint32_t eye = 0; eye < 2; ++eye) {
        if (!floats_close(views[eye].fov.angleLeft, expected[eye].angleLeft) ||
            !floats_close(views[eye].fov.angleRight, expected[eye].angleRight) ||
            !floats_close(views[eye].fov.angleUp, expected[eye].angleUp) ||
            !floats_close(views[eye].fov.angleDown, expected[eye].angleDown))
            return fail("configured FOV differs");
    }

    destroySpace(space);
    destroySession(session);
    destroyInstance(instance);
    FreeLibrary(runtime);
    std::printf("PASS: %s headset profile (%s)\n", mode.c_str(), sizeof(void*) == 8 ? "x64" : "x86");
    return 0;
}
