// Control-channel regression probe: a command must survive a poll that cannot
// open command.txt, or finds it empty, just after its write time changed. Each
// case is a separate process with its own XRSIM_DIR.
//
//   exclusive-hold  the writer keeps command.txt open with no sharing for
//                   ~100 ms after writing a new command (a scanner, a backup
//                   tool), then releases it
//   truncate-write  an in-place writer truncates, pauses, then writes (what
//                   File.WriteAllText or Python "w" do, minus the pause)
//   rename-hold     a rename-over writer still holds the renamed file with
//                   DELETE access, as os.replace does for a moment
//
// The first two pin the write time with SetFileTime(-1), so it is the same
// after the hold as during it - the state in which the poller used to lose the
// command: it had marked that write time seen before reading anything.
//
// Every case first checks that a stale command.txt is still ignored at boot,
// and finishes by checking the command was applied exactly once.

#include <windows.h>

#include <openxr/openxr.h>
#include <openxr/openxr_loader_negotiation.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

namespace {

int fail(const char* what) {
    std::fprintf(stderr, "FAIL: %s\n", what);
    return 1;
}

std::wstring g_dir;

std::wstring dir_path(const wchar_t* leaf) { return g_dir + L"\\" + leaf; }

void sleep_ms(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

std::string read_text(const std::wstring& path) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return {};
    std::string text;
    char chunk[4096];
    DWORD got = 0;
    while (ReadFile(file, chunk, sizeof(chunk), &got, nullptr) && got > 0) text.append(chunk, got);
    CloseHandle(file);
    return text;
}

// The probe's own opens can meet a scanner holding the file for a moment; retry
// those for up to a second, as a well-behaved writer would.
HANDLE create_retrying(const std::wstring& path, DWORD access, DWORD share, DWORD disposition) {
    for (int attempt = 0;; ++attempt) {
        HANDLE file = CreateFileW(path.c_str(), access, share, nullptr, disposition,
                                  FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file != INVALID_HANDLE_VALUE || attempt == 100) return file;
        const DWORD error = GetLastError();
        if (error != ERROR_SHARING_VIOLATION && error != ERROR_ACCESS_DENIED) return file;
        sleep_ms(10);
    }
}

bool write_text(const std::wstring& path, const std::string& text) {
    HANDLE file = create_retrying(path, GENERIC_WRITE,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, CREATE_ALWAYS);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const BOOL ok = WriteFile(file, text.data(), static_cast<DWORD>(text.size()), &written, nullptr);
    CloseHandle(file);
    return ok && written == text.size();
}

// The comparison the runtime's poller makes. Attribute queries need no share
// access, so this works through an exclusive hold too.
ULONGLONG write_time(const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA fad{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad)) return 0;
    return (static_cast<ULONGLONG>(fad.ftLastWriteTime.dwHighDateTime) << 32) |
           fad.ftLastWriteTime.dwLowDateTime;
}

// The runtime opens xrsim.log with read sharing and rotates it at instance
// creation, so this is only the current run's log. It is written in text mode;
// CRs are dropped so a needle can end in "\n".
size_t log_count(const std::string& needle) {
    std::string log = read_text(dir_path(L"xrsim.log"));
    log.erase(std::remove(log.begin(), log.end(), '\r'), log.end());
    size_t n = 0;
    for (size_t at = log.find(needle); at != std::string::npos; at = log.find(needle, at + 1)) ++n;
    return n;
}

bool wait_log(const std::string& needle, int timeoutMs) {
    for (int waited = 0; waited <= timeoutMs; waited += 10) {
        if (log_count(needle)) return true;
        sleep_ms(10);
    }
    return false;
}

std::string applied_line(const std::string& command) {
    return "applied 1 command(s): " + command + "\n";
}

// ack.txt is rewritten in place, so a read can land between its truncation and
// its single write; retry until the line structure is whole. Text mode, like
// the log.
bool read_ack(unsigned* seq, std::string* last, std::string* error) {
    for (int attempt = 0; attempt < 50; ++attempt) {
        std::string ack = read_text(dir_path(L"ack.txt"));
        ack.erase(std::remove(ack.begin(), ack.end(), '\r'), ack.end());
        const size_t lastAt = ack.find("\nlast=");
        if (ack.size() > 1 && ack.back() == '\n' && lastAt != std::string::npos) {
            char err[256] = {};
            unsigned frame = 0;
            if (sscanf_s(ack.c_str(), "seq=%u frame=%u error=%255[^\n]", seq, &frame, err,
                         static_cast<unsigned>(sizeof(err))) == 3) {
                *error = err;
                *last = ack.substr(lastAt + 6, ack.size() - lastAt - 7);
                return true;
            }
        }
        sleep_ms(10);
    }
    return false;
}

template <typename T>
T get_proc(PFN_xrGetInstanceProcAddr gipa, XrInstance instance, const char* name) {
    PFN_xrVoidFunction raw = nullptr;
    if (XR_FAILED(gipa(instance, name, &raw))) return nullptr;
    return reinterpret_cast<T>(raw);
}

// Keeps the write time a handle's file has now: writes and closing through
// that handle no longer move it.
bool pin_write_time(HANDLE file) {
    FILETIME keep{0xFFFFFFFF, 0xFFFFFFFF};
    return SetFileTime(file, nullptr, nullptr, &keep) != FALSE;
}

// Hold for at least `minMs`, and until the runtime has logged `evidence` more
// than the `before` times it already had (5 s at most). The result says whether
// the poller demonstrably looked during this hold; without that a pass would
// test nothing.
bool hold_until_logged(const std::string& evidence, size_t before, int minMs) {
    const auto start = std::chrono::steady_clock::now();
    for (;;) {
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - start).count();
        if (ms >= minMs && log_count(evidence) > before) return true;
        if (ms > 5000) return false;
        sleep_ms(10);
    }
}

int case_exclusive_hold(const std::wstring& cmdPath, const std::string& command, bool* exercised) {
    const std::string evidence = "command.txt open failed (error 32";
    const size_t before = log_count(evidence);
    HANDLE file = create_retrying(cmdPath, GENERIC_WRITE, 0, CREATE_ALWAYS);
    if (file == INVALID_HANDLE_VALUE) return fail("exclusive open of command.txt");
    const std::string text = command + "\n";
    DWORD written = 0;
    if (!pin_write_time(file) ||
        !WriteFile(file, text.data(), static_cast<DWORD>(text.size()), &written, nullptr) ||
        written != text.size()) {
        CloseHandle(file);
        return fail("write the command under a pinned write time and the exclusive hold");
    }
    const ULONGLONG held = write_time(cmdPath);

    // Positive control: the hold really keeps a reader out.
    HANDLE reader = CreateFileW(cmdPath.c_str(), GENERIC_READ,
                                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    const DWORD readerError = GetLastError();
    if (reader != INVALID_HANDLE_VALUE || readerError != ERROR_SHARING_VIOLATION) {
        if (reader != INVALID_HANDLE_VALUE) CloseHandle(reader);
        CloseHandle(file);
        return fail("the exclusive hold did not keep a reader out with a sharing violation");
    }

    *exercised = hold_until_logged(evidence, before, 100);
    const bool early = log_count(applied_line(command)) != 0;
    CloseHandle(file);
    if (early) return fail("the command was applied while command.txt was exclusively held");
    if (write_time(cmdPath) != held) return fail("the write time moved at release; the case would test nothing");
    return 0;
}

int case_truncate_write(const std::wstring& cmdPath, const std::string& command, bool* exercised) {
    const std::string evidence = "command.txt is empty";
    const size_t before = log_count(evidence);
    HANDLE file = create_retrying(cmdPath, GENERIC_WRITE,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, CREATE_ALWAYS);
    if (file == INVALID_HANDLE_VALUE) return fail("truncate command.txt");
    // The write below keeps the truncation's write time, as it does when the
    // truncate and the write land in one file-system clock tick.
    if (!pin_write_time(file)) {
        CloseHandle(file);
        return fail("pin the write time");
    }
    const ULONGLONG truncated = write_time(cmdPath);

    *exercised = hold_until_logged(evidence, before, 100);
    const std::string text = command + "\n";
    DWORD written = 0;
    const BOOL wrote = WriteFile(file, text.data(), static_cast<DWORD>(text.size()), &written, nullptr);
    CloseHandle(file);
    if (!wrote || written != text.size()) return fail("write the command after the truncation");
    if (write_time(cmdPath) != truncated) return fail("the write time moved at the write; the case would test nothing");
    return 0;
}

int case_rename_hold(const std::wstring& cmdPath, const std::string& command) {
    const std::wstring tmpPath = cmdPath + L".tmp";
    HANDLE file = create_retrying(tmpPath, GENERIC_WRITE | DELETE,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, CREATE_ALWAYS);
    if (file == INVALID_HANDLE_VALUE) return fail("create command.txt.tmp");
    const std::string text = command + "\n";
    DWORD written = 0;
    if (!WriteFile(file, text.data(), static_cast<DWORD>(text.size()), &written, nullptr) ||
        written != text.size()) {
        CloseHandle(file);
        return fail("write command.txt.tmp");
    }

    // Rename over command.txt through this handle, as MoveFileExW does, and
    // keep the handle - and its DELETE access - open afterwards.
    std::vector<BYTE> buffer(sizeof(FILE_RENAME_INFO) + cmdPath.size() * sizeof(wchar_t));
    auto* rename = reinterpret_cast<FILE_RENAME_INFO*>(buffer.data());
    rename->ReplaceIfExists = TRUE;
    rename->FileNameLength = static_cast<DWORD>(cmdPath.size() * sizeof(wchar_t));
    std::memcpy(rename->FileName, cmdPath.c_str(), rename->FileNameLength);
    bool renamed = false;
    for (int attempt = 0; attempt < 100 && !renamed; ++attempt) {
        renamed = SetFileInformationByHandle(file, FileRenameInfo, rename,
                                             static_cast<DWORD>(buffer.size())) != FALSE;
        if (!renamed) sleep_ms(5);
    }
    if (!renamed) {
        CloseHandle(file);
        return fail("rename command.txt.tmp over command.txt");
    }

    // Positive control: the held DELETE access really keeps out a reader that
    // does not share delete access, as the poller's old _wfsopen did not.
    HANDLE oldStyle = CreateFileW(cmdPath.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                  nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    const DWORD oldStyleError = GetLastError();
    if (oldStyle != INVALID_HANDLE_VALUE || oldStyleError != ERROR_SHARING_VIOLATION) {
        if (oldStyle != INVALID_HANDLE_VALUE) CloseHandle(oldStyle);
        CloseHandle(file);
        return fail("the held DELETE access did not keep out a reader without FILE_SHARE_DELETE");
    }

    const bool appliedWhileHeld = wait_log(applied_line(command), 3000);
    CloseHandle(file);
    if (!appliedWhileHeld)
        return fail("the command was not applied while the renamer still held DELETE access");
    return 0;
}

// Everything that needs a live instance (and so a running poll thread).
int run_case(const std::string& mode, const std::wstring& cmdPath, const std::string& staleCommand) {
    // The poll thread adopts the stale file's write time before its first poll.
    // Waiting for that line also means the commands below cannot race it.
    if (!wait_log("ignoring a command.txt written before this run started", 5000))
        return fail("the stale command.txt was not reported as ignored");

    const std::string warmup = "note warmup";
    if (!write_text(cmdPath, warmup + "\n")) return fail("write the warm-up command");
    const ULONGLONG warmupTime = write_time(cmdPath);
    if (!wait_log(applied_line(warmup), 5000)) return fail("the warm-up command was not applied");
    if (log_count("applied 1 command(s): " + staleCommand)) return fail("the stale command was replayed");
    // Next write lands in a later file-system clock tick than the warm-up's.
    sleep_ms(50);

    const std::string command = "note " + mode;
    const std::string recovered = "command.txt read after";
    const size_t recoveredBefore = log_count(recovered);
    bool exercised = true;
    int result = 0;
    if (mode == "exclusive-hold") result = case_exclusive_hold(cmdPath, command, &exercised);
    else if (mode == "truncate-write") result = case_truncate_write(cmdPath, command, &exercised);
    else result = case_rename_hold(cmdPath, command);
    if (result) return result;
    if (write_time(cmdPath) == warmupTime) return fail("the case's write time equals the warm-up's");

    if (!wait_log(applied_line(command), 3000)) return fail("the command was lost");
    if (!exercised) return fail("the runtime logged no retry during the hold, so it was not exercised");
    if (mode != "rename-hold" && log_count(recovered) <= recoveredBefore)
        return fail("the runtime did not log the recovered read");
    // Twenty-odd polls later it must still have been applied exactly once.
    sleep_ms(500);
    if (log_count(applied_line(command)) != 1) return fail("the command was applied more than once");
    unsigned seq = 0;
    std::string last, error;
    if (!read_ack(&seq, &last, &error)) return fail("ack.txt never parsed");
    if (seq != 2 || last != command || error != "-")
        return fail("ack.txt does not acknowledge exactly the warm-up and the command");
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 3) {
        std::fprintf(stderr, "usage: xrsim_control_probe <xrsim.dll> <exclusive-hold|truncate-write|rename-hold>\n");
        return 2;
    }
    const std::string mode = argv[2];
    if (mode != "exclusive-hold" && mode != "truncate-write" && mode != "rename-hold")
        return fail("unknown case");

    wchar_t dir[32768] = {};
    const DWORD n = GetEnvironmentVariableW(L"XRSIM_DIR", dir, static_cast<DWORD>(std::size(dir)));
    if (n == 0 || n >= std::size(dir)) return fail("XRSIM_DIR is missing");
    g_dir = dir;
    const std::wstring cmdPath = dir_path(L"command.txt");
    DeleteFileW(dir_path(L"ack.txt").c_str());
    DeleteFileW(dir_path(L"state.json").c_str());
    DeleteFileW(dir_path(L"command.txt.tmp").c_str());

    // Last run's leftovers, an hour old: must be skipped, never replayed.
    const std::string staleCommand = "head rot 90 0 0";
    if (!write_text(cmdPath, staleCommand + "\n")) return fail("seed a stale command.txt");
    {
        HANDLE file = CreateFileW(cmdPath.c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ, nullptr,
                                  OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) return fail("open the stale command.txt");
        FILETIME now{};
        GetSystemTimeAsFileTime(&now);
        ULARGE_INTEGER t{};
        t.LowPart = now.dwLowDateTime;
        t.HighPart = now.dwHighDateTime;
        t.QuadPart -= 36000000000ULL;  // one hour of 100 ns ticks
        const FILETIME old{t.LowPart, t.HighPart};
        const BOOL aged = SetFileTime(file, nullptr, nullptr, &old);
        CloseHandle(file);
        if (!aged) return fail("age the stale command.txt");
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
    if (XR_FAILED(negotiate(&loader, &request)) || !request.getInstanceProcAddr)
        return fail("runtime negotiation");

    const auto createInstance = get_proc<PFN_xrCreateInstance>(
        request.getInstanceProcAddr, XR_NULL_HANDLE, "xrCreateInstance");
    if (!createInstance) return fail("resolve xrCreateInstance");
    const char* extension = XR_MND_HEADLESS_EXTENSION_NAME;
    XrInstanceCreateInfo createInfo{XR_TYPE_INSTANCE_CREATE_INFO};
    strcpy_s(createInfo.applicationInfo.applicationName, "xrsim_control_probe");
    strcpy_s(createInfo.applicationInfo.engineName, "direct-runtime-test");
    createInfo.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 34);
    createInfo.enabledExtensionCount = 1;
    createInfo.enabledExtensionNames = &extension;
    XrInstance instance = XR_NULL_HANDLE;
    if (XR_FAILED(createInstance(&createInfo, &instance))) return fail("xrCreateInstance");
    const auto destroyInstance = get_proc<PFN_xrDestroyInstance>(
        request.getInstanceProcAddr, instance, "xrDestroyInstance");
    if (!destroyInstance) return fail("resolve xrDestroyInstance");

    const int result = run_case(mode, cmdPath, staleCommand);
    // Pass or fail, stop the runtime's poll thread before exiting: a joinable
    // std::thread destroyed at process exit fail-fasts, and that crash code
    // would replace the FAIL exit code.
    destroyInstance(instance);
    FreeLibrary(runtime);
    if (result == 0)
        std::printf("PASS: %s command applied exactly once (%s)\n", mode.c_str(),
                    sizeof(void*) == 8 ? "x64" : "x86");
    return result;
}
