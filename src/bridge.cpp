#include "bridge.h"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <objbase.h>
#include <vector>

// The worker uses the same acquire/release operations on aligned mapping fields.
extern "C" __declspec(dllexport) long long IwiLoad(volatile LONG64* p) {
    return InterlockedCompareExchange64(p, 0, 0);
}
extern "C" __declspec(dllexport) void IwiStore(volatile LONG64* p, LONG64 v) {
    InterlockedExchange64(p, v);
}
// Audio workers on the same GPU can publish their timestamps in a different order.
extern "C" __declspec(dllexport) void IwiStoreMax(volatile LONG64* p, LONG64 value) {
    auto previous = IwiLoad(p);
    while (previous < value) {
        const auto observed = InterlockedCompareExchange64(p, value, previous);
        if (observed == previous) return;
        previous = observed;
    }
}
namespace isolator {
namespace fs = std::filesystem;
static fs::path modulePath() {
    HMODULE module{};
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&IwiLoad), &module);
    std::wstring path(32768, L'\0');
    path.resize(GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size())));
    return fs::path(path);
}
static void dispose(HANDLE& h) { if (h) { CloseHandle(h); h = nullptr; } }

bool Bridge::start(double sampleRate) {
    close();
    try {
        const auto binary = modulePath();
        const auto resources = binary.parent_path().parent_path() / L"Resources";
#ifdef VOICE_ISOLATOR_RTX
        std::wstring installed(32768, L'\0');
        DWORD bytes = static_cast<DWORD>(installed.size() * sizeof(wchar_t));
        if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Local Audio Lab\\Voice Isolator RTX",
                         L"EngineRoot", RRF_RT_REG_SZ | RRF_SUBKEY_WOW6464KEY,
                         nullptr, installed.data(), &bytes) != ERROR_SUCCESS) return false;
        const auto root = fs::path(installed.c_str());
        if (!root.is_absolute()) return false;
        const auto python = root / L"python/python.exe";
#else
        std::ifstream config(resources / L"engine-root.txt");
        std::string line;
        if (!std::getline(config, line)) return false;
        const auto root = fs::u8path(line);
        const auto python = root / L".venv/Scripts/python.exe";
#endif
        const auto script = resources / L"worker.py";
        if (!fs::is_regular_file(python) || !fs::is_regular_file(script)) return false;
        GUID id{};
        if (FAILED(CoCreateGuid(&id))) return false;
        wchar_t guid[48]{};
        StringFromGUID2(id, guid, 48);
        const std::wstring name = std::wstring(L"Local\\InworldIsolator-") + guid;
        mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(Shared), name.c_str());
        if (!mapping || GetLastError() == ERROR_ALREADY_EXISTS) { close(); return false; }
        shared = static_cast<Shared*>(MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Shared)));
        inputEvent = CreateEventW(nullptr, FALSE, FALSE, (name + L"-input").c_str());
        outputEvent = CreateEventW(nullptr, FALSE, FALSE, (name + L"-output").c_str());
        if (!shared || !inputEvent || !outputEvent) { close(); return false; }
        store(Magic, protocol);
        job = CreateJobObjectW(nullptr, nullptr);
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!job || !SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits))) {
            close(); return false;
        }
        auto quote = [](const std::wstring& value) { return L"\"" + value + L"\""; };
        auto cmd = quote(python.wstring()) + L" -I -B -u " + quote(script.wstring()) + L" " +
                   quote(root.wstring()) + L" " + quote(binary.wstring()) + L" " + quote(name) + L" " +
                   std::to_wstring(static_cast<int>(sampleRate));
#ifdef VOICE_ISOLATOR_RTX
        cmd += L" --rtx";
#endif
        STARTUPINFOW startup{}; startup.cb = sizeof(startup);
        PROCESS_INFORMATION child{};
        if (!CreateProcessW(python.c_str(), cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW | CREATE_SUSPENDED,
                            nullptr, root.c_str(), &startup, &child)) { close(); return false; }
        process = child.hProcess;
        if (!AssignProcessToJobObject(job, process) || ResumeThread(child.hThread) == static_cast<DWORD>(-1)) {
            TerminateProcess(process, 1); CloseHandle(child.hThread); close(); return false;
        }
        CloseHandle(child.hThread);
        return true;
    } catch (...) { close(); return false; }
}
void Bridge::close() {
    if (shared) store(Stop, 1);
    if (inputEvent) SetEvent(inputEvent);
    if (process) WaitForSingleObject(process, 200);
    dispose(job); // also reaps a Python venv launcher's child on host shutdown
    dispose(process); dispose(inputEvent); dispose(outputEvent);
    if (shared) { UnmapViewOfFile(shared); shared = nullptr; }
    dispose(mapping);
    epoch = 0; origin = 0;
}
int Bridge::state() const {
    if (!shared || !process || WaitForSingleObject(process, 0) == WAIT_OBJECT_0) return -1;
    return static_cast<int>(read(State));
}
void Bridge::reset(int64_t cursor, int64_t advanceMode, int channels) {
    if (!shared) return;
    epoch += 2;
    store(Epoch, epoch - 1);
    store(AdvanceMode, advanceMode);
    store(Channels, channels);
    origin = cursor; store(Origin, cursor); store(InputEnd, cursor);
    store(Epoch, epoch);
    SetEvent(inputEvent);
}
void Bridge::publish(int64_t first, int count, const float* audio, const float* second) {
    if (!shared) return;
    if (first + count - std::max<int64_t>(origin, read(Consumed)) > capacity - 2048)
        reset(first, read(AdvanceMode), static_cast<int>(read(Channels)));
    for (int i = 0; i < count; ++i) {
        const auto slot = (first + i) & (capacity - 1);
        shared->input[0][slot] = audio[i];
        if (second) shared->input[1][slot] = second[i];
    }
    store(InputEnd, first + count);
    SetEvent(inputEvent);
}
int64_t Bridge::wetEnd() const {
    return shared && epoch && read(WetEpoch) == epoch ? read(WetEnd) : origin;
}
bool Bridge::waitReady() {
    if (!shared) return false;
    const auto deadline = GetTickCount64() + 90000;
    while (state() == 0 && GetTickCount64() < deadline) WaitForSingleObject(outputEvent, 30);
    return state() == 1;
}
void Bridge::waitWet(int64_t target) {
    if (target <= origin) return;
    const auto deadline = GetTickCount64() + 15000;
    while (wetEnd() < target && state() == 1 && GetTickCount64() < deadline)
        WaitForSingleObject(outputEvent, 20);
}
}
