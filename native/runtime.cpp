#include "manifest.hpp"
#include "reload-ipc.hpp"
#include <MinHook.h>
#include <memory>
#include <mutex>
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <stdexcept>

namespace {
using LoadBundle = void* (*)(void*, uint32_t, uint64_t);
using LoadAsset = void* (*)(void*, void*, void*);
LoadBundle originalSync = nullptr, originalAsync = nullptr;
LoadAsset originalAsset = nullptr, originalAssetAsync = nullptr, originalSubAssets = nullptr, originalSubAssetsAsync = nullptr;
void* (*originalGetBundle)(void*) = nullptr;
void (*originalUnload)(void*, bool) = nullptr;
const wchar_t* (*stringChars)(void*) = nullptr;
int32_t (*stringLength)(void*) = nullptr;
void* (*stringNew)(const wchar_t*, int32_t) = nullptr;
uint32_t (*handleNew)(void*, bool) = nullptr, (*handleWeak)(void*, bool) = nullptr;
void* (*handleTarget)(uint32_t) = nullptr;
void (*handleFree)(uint32_t) = nullptr;
void* (*invoke)(const void*, void*, void**, void**) = nullptr;
void* (*unbox)(void*) = nullptr;
const void *loadMethod = nullptr, *unloadMethod = nullptr, *doneMethod = nullptr;
ark::fs::path statusDirectory, executable;
std::wstring lastPatch;
std::mutex stateMutex;
thread_local bool internalLoad = false;
std::atomic_bool detaching = false;
bool reloadAvailable = false;
void* runtimeDomain = nullptr;
void* (*threadCurrent)() = nullptr;
void* (*threadAttach)(void*) = nullptr;
void (*threadDetach)(void*) = nullptr;
void* (*domainGet)() = nullptr;
HMODULE gameAssembly = nullptr;
ark::Manifest startupManifest;
std::atomic<int> initializationState = 0, initializationDepth = 0;
std::atomic<ULONGLONG> nextInitializationAttempt = 0;
struct BindingsNotReady : std::runtime_error { using std::runtime_error::runtime_error; };
int (*originalInit)(const char*) = nullptr;
int (*originalInitUtf16)(const wchar_t*) = nullptr;
void initializeManagedRuntime();
void (*managedInitializer)() = initializeManagedRuntime;

void enterManagedRuntime() {
    // domain_get may allocate its singleton. Never poll it from a native
    // bootstrap thread: wait for Unity's initialization or a registered VM call.
    if (initializationState.load() != 0 || initializationDepth.load() != 0 || GetTickCount64() < nextInitializationAttempt.load() || !threadCurrent || !threadCurrent()) return;
    int expected = 0;
    if (!initializationState.compare_exchange_strong(expected, 1)) return;
    try {
        runtimeDomain = domainGet();
        if (!runtimeDomain) { initializationState.store(0); return; }
        managedInitializer();
        initializationState.store(2);
    } catch (const BindingsNotReady& error) {
        nextInitializationAttempt.store(GetTickCount64() + 100);
        initializationState.store(0);
        ark::writeStatus(statusDirectory, "waiting-bindings", error.what());
    } catch (const std::exception& error) {
        initializationState.store(3);
        ark::writeStatus(statusDirectory, "unsupported", error.what());
    }
}
struct InitializingUnity {
    InitializingUnity() { ++initializationDepth; }
    ~InitializingUnity() { --initializationDepth; }
};
int initializeUtf8(const char* name) {
    int result;
    { InitializingUnity scope; result = originalInit(name); }
    if (result) enterManagedRuntime();
    return result;
}
int initializeUtf16(const wchar_t* name) {
    int result;
    { InitializingUnity scope; result = originalInitUtf16(name); }
    if (result) enterManagedRuntime();
    return result;
}
void* invokeManaged(const void* method, void* object, void** parameters, void** exception) {
    enterManagedRuntime();
    return invoke(method, object, parameters, exception);
}

// Native Unity workers can enter an icall without being IL2CPP threads. Any
// allocation, GC handle or managed invocation below needs a registered stack.
// Only detach threads attached by this scope; leave Unity-owned threads alone.
struct RuntimeThread {
    void* owned = nullptr;
    bool ready = false;
    RuntimeThread() {
        if (detaching.load() || !runtimeDomain || !threadCurrent || !threadAttach || !threadDetach) return;
        if (threadCurrent()) ready = true;
        else { owned = threadAttach(runtimeDomain); ready = owned != nullptr; }
    }
    ~RuntimeThread() { if (owned && !detaching.load()) threadDetach(owned); }
    RuntimeThread(const RuntimeThread&) = delete;
    RuntimeThread& operator=(const RuntimeThread&) = delete;
    explicit operator bool() const { return ready; }
};

struct Managed {
    uint32_t handle;
    Managed(void* object, bool weak = false) : handle(0) {
        RuntimeThread thread;
        if (!thread) throw std::runtime_error("Could not register an IL2CPP resource thread");
        handle = weak ? handleWeak(object, false) : handleNew(object, false);
        if (!handle) throw std::runtime_error("Could not retain a runtime resource");
    }
    ~Managed() { if (!detaching.load()) { RuntimeThread thread; if (thread) handleFree(handle); } }
    void* get() const { RuntimeThread thread; return thread ? handleTarget(handle) : nullptr; }
};
struct Target { ark::fs::path file; std::string version; std::shared_ptr<Managed> name; };
struct Loaded {
    void* identity = nullptr;
    std::wstring source;
    std::shared_ptr<Target> target;
    std::shared_ptr<Managed> object;
    std::shared_ptr<Loaded> successor;
    std::shared_ptr<Managed> successorPin;
    bool unloaded = false;
    std::mutex reloadMutex;
};
struct Pending { std::shared_ptr<Managed> object; std::wstring source; std::shared_ptr<Target> target; };
std::map<std::wstring, std::shared_ptr<Target>> targets;
std::map<void*, std::shared_ptr<Loaded>> loaded;
std::map<std::wstring, std::vector<std::weak_ptr<Loaded>>> sourceBundles;
size_t remembered = 0;
std::map<void*, Pending> creating;
std::map<std::wstring, std::vector<Pending>> assetRequests;

std::shared_ptr<Target> makeTarget(const ark::fs::path& file, const std::string& version) {
    RuntimeThread thread;
    if (!thread) throw std::runtime_error("Could not register an IL2CPP resource thread");
    auto name = file.wstring();
    auto object = stringNew(name.data(), static_cast<int32_t>(name.size()));
    if (!object) throw std::runtime_error("Could not create a resource path");
    return std::make_shared<Target>(Target{file, version, std::make_shared<Managed>(object)});
}
std::shared_ptr<Target> targetFor(const std::wstring& source) {
    std::lock_guard lock(stateMutex);
    auto found = targets.find(source);
    return found == targets.end() ? nullptr : found->second;
}
bool same(const std::shared_ptr<Target>& a, const std::shared_ptr<Target>& b) {
    return (!a && !b) || (a && b && a->version == b->version);
}
std::wstring sourceFor(void* input, uint64_t offset = 0) {
    if (!input || offset) return {};
    auto count = stringLength(input);
    if (count <= 0 || count > 32760) return {};
    auto file = ark::fs::absolute(ark::fs::path(std::wstring(stringChars(input), count))).lexically_normal();
    if (!ark::within(file, executable.parent_path() / L"Arknights_Data")) return {};
    return ark::normalized(file);
}
void remember(void* object, const std::wstring& source, const std::shared_ptr<Target>& target) {
    if (!object || source.empty()) return;
    auto entry = std::make_shared<Loaded>();
    entry->identity = object; entry->source = source; entry->target = target; entry->object = std::make_shared<Managed>(object, true);
    std::lock_guard lock(stateMutex);
    // Amortize dead-wrapper cleanup; ordinary loads never scan all live bundles.
    if (++remembered % 128 == 0) {
        for (auto it = loaded.begin(); it != loaded.end();) {
            if (it->second->object->get() != it->first) it = loaded.erase(it); else ++it;
        }
        for (auto it = sourceBundles.begin(); it != sourceBundles.end();) {
            std::erase_if(it->second, [](const auto& item) { return item.expired(); });
            if (it->second.empty()) it = sourceBundles.erase(it); else ++it;
        }
    }
    auto existing = loaded.find(object);
    if (existing != loaded.end() && existing->second->object->get() == object) return;
    if (existing != loaded.end()) loaded.erase(existing);
    auto& siblings = sourceBundles[source];
    auto pin = std::make_shared<Managed>(object);
    for (auto it = siblings.begin(); it != siblings.end();) {
        auto previous = it->lock();
        if (!previous || !previous->object->get()) { if (previous) loaded.erase(previous->identity); it = siblings.erase(it); }
        else { previous->successor = entry; previous->successorPin = pin; ++it; }
    }
    siblings.push_back(entry);
    loaded.emplace(object, entry);
}
std::shared_ptr<Loaded> known(void* object) {
    std::lock_guard lock(stateMutex);
    auto found = loaded.find(object);
    if (found == loaded.end() || found->second->object->get() != object) return nullptr;
    auto entry = found->second;
    while (entry->successor) entry = entry->successor;
    return entry;
}
bool busy(const std::wstring& source) {
    std::vector<Pending> pending;
    { std::lock_guard lock(stateMutex); auto found = assetRequests.find(source); if (found != assetRequests.end()) pending = found->second; for (const auto& [_, request] : creating) if (request.source == source) pending.push_back(request); }
    for (const auto& request : pending) {
        if (request.source != source) continue;
        auto object = request.object->get();
        if (!object) continue;
        void* exception = nullptr;
        auto result = invoke(doneMethod, object, nullptr, &exception);
        if (exception || !result || !*static_cast<bool*>(unbox(result))) return true;
    }
    return false;
}
bool retire(const std::shared_ptr<Loaded>& entry) {
    if (busy(entry->source)) return false;
    { std::lock_guard lock(stateMutex); if (entry->unloaded || !entry->object->get()) return true; }
    bool destroyObjects = false;
    void* args[] = {&destroyObjects}; void* exception = nullptr;
    // Capture Unity exceptions; never destroy objects still displayed in a scene.
    invoke(unloadMethod, entry->object->get(), args, &exception);
    return !exception;
}
void* currentBundle(const std::shared_ptr<Loaded>& entry) {
    auto desired = targetFor(entry->source);
    if (same(desired, entry->target)) return entry->object->get();
    std::unique_lock lock(entry->reloadMutex, std::try_to_lock);
    if (!lock || !retire(entry)) return entry->object->get();
    struct Guard { Guard() { internalLoad = true; } ~Guard() { internalLoad = false; } } guard;
    auto load = [&](const std::shared_ptr<Target>& target) {
        auto chosen = target ? target : makeTarget(entry->source, "");
        uint32_t crc = 0; uint64_t offset = 0; void* exception = nullptr;
        void* args[] = {chosen->name->get(), &crc, &offset};
        auto result = invoke(loadMethod, nullptr, args, &exception);
        if (exception || !result) return static_cast<void*>(nullptr);
        remember(result, entry->source, target);
        return result;
    };
    if (auto result = load(desired)) return result;
    auto result = load(entry->target);
    ark::writeStatus(statusDirectory, "reload-deferred", "Changed bundle load failed; previous resources were retained. Re-enter the character view to retry.");
    return result ? result : entry->object->get();
}
void* loadFile(LoadBundle original, void* input, uint32_t crc, uint64_t offset, bool async) {
    RuntimeThread thread;
    if (!thread) return original(input, crc, offset);
    if (internalLoad) return original(input, crc, offset);
    auto source = sourceFor(input, offset);
    if (source.empty()) return original(input, crc, offset);
    auto target = targetFor(source);
    if (!reloadAvailable) return original(target ? target->name->get() : input, target ? 0 : crc, offset);
    std::shared_ptr<Loaded> previous;
    { std::lock_guard lock(stateMutex); auto found = sourceBundles.find(source); if (found != sourceBundles.end()) for (auto it = found->second.rbegin(); it != found->second.rend(); ++it) if (auto entry = it->lock(); entry && !entry->successor && entry->object->get()) { previous = entry; break; } }
    if (previous && !same(target, previous->target)) {
        if (!async) return currentBundle(previous);
        std::unique_lock lock(previous->reloadMutex, std::try_to_lock);
        if (!lock || !retire(previous)) target = previous->target;
    }
    if (target && (!ark::fs::is_regular_file(target->file) || ark::hasLink(target->file))) target = nullptr;
    auto result = original(target ? target->name->get() : input, target ? 0 : crc, offset);
    if (!result) return result;
    if (async) {
        Pending request{std::make_shared<Managed>(result, true), source, target};
        std::lock_guard lock(stateMutex);
        for (auto it = creating.begin(); it != creating.end();) { if (it->second.object->get() != it->first) it = creating.erase(it); else ++it; }
        creating.insert_or_assign(result, std::move(request));
    } else remember(result, source, target);
    return result;
}
void* loadSync(void* f, uint32_t c, uint64_t o) { try { return loadFile(originalSync, f, c, o, false); } catch (const std::exception&) { return originalSync(f, c, o); } }
void* loadAsync(void* f, uint32_t c, uint64_t o) { try { return loadFile(originalAsync, f, c, o, true); } catch (const std::exception&) { return originalAsync(f, c, o); } }
void* getBundle(void* request) {
    RuntimeThread thread;
    if (!thread) return originalGetBundle(request);
    auto result = originalGetBundle(request);
    Pending pending;
    { std::lock_guard lock(stateMutex); auto found = creating.find(request); if (found == creating.end()) return result; pending = found->second; creating.erase(found); }
    remember(result, pending.source, pending.target);
    return result;
}
void unload(void* object, bool destroyObjects) {
    RuntimeThread thread;
    if (!thread) { originalUnload(object, destroyObjects); return; }
    auto entry = known(object);
    if (entry && !internalLoad) object = entry->object->get();
    { std::lock_guard lock(stateMutex); auto found = loaded.find(object); if (found != loaded.end() && found->second->unloaded) return; }
    originalUnload(object, destroyObjects);
    std::lock_guard lock(stateMutex);
    auto found = loaded.find(object);
    if (found != loaded.end()) found->second->unloaded = true;
}
void* asset(LoadAsset original, void* bundle, void* name, void* type, bool async) {
    RuntimeThread thread;
    if (!thread) return original(bundle, name, type);
    auto entry = known(bundle);
    if (entry) if (auto current = currentBundle(entry)) bundle = current;
    auto result = original(bundle, name, type);
    if (async && result && entry) {
        Pending request{std::make_shared<Managed>(result, true), entry->source, nullptr};
        std::lock_guard lock(stateMutex);
        auto& pending = assetRequests[entry->source];
        std::erase_if(pending, [](const Pending& item) { return !item.object->get(); });
        pending.push_back(std::move(request));
    }
    return result;
}
void* loadAsset(void* b, void* n, void* t) { return asset(originalAsset, b, n, t, false); }
void* loadAssetAsync(void* b, void* n, void* t) { return asset(originalAssetAsync, b, n, t, true); }
void* subAssets(void* b, void* n, void* t) { return asset(originalSubAssets, b, n, t, false); }
void* subAssetsAsync(void* b, void* n, void* t) { return asset(originalSubAssetsAsync, b, n, t, true); }

std::string response(size_t changed = 0) {
    std::lock_guard lock(stateMutex);
    return "{\"success\":true,\"protocol\":" + std::string(reloadAvailable ? "2" : "1") + ",\"pid\":" + std::to_string(GetCurrentProcessId()) +
        ",\"manifest\":" + ark::jsonString(ark::utf8((statusDirectory / L"manifest.tsv").wstring())) +
        ",\"lastPatch\":" + ark::jsonString(ark::utf8(lastPatch)) + ",\"changedBundles\":" + std::to_string(changed) + ",\"mappedBundles\":" + std::to_string(targets.size()) + ",\"refreshRequired\":true}";
}
void serve() {
    RuntimeThread thread;
    if (!thread) throw std::runtime_error("Could not register the IL2CPP reload thread");
    for (;;) {
        ark::WinHandle pipe(CreateNamedPipeW(ark::pipeName(GetCurrentProcessId()).c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
            PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, 65536, 65536, 0, nullptr));
        if (pipe.value == INVALID_HANDLE_VALUE) throw std::runtime_error("Cannot create the local reload endpoint");
        ark::WinHandle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
        OVERLAPPED connect{}; connect.hEvent = event.value;
        auto connected = ConnectNamedPipe(pipe.value, &connect);
        auto error = connected ? ERROR_SUCCESS : GetLastError();
        if (error != ERROR_PIPE_CONNECTED) {
            if (error != ERROR_IO_PENDING && !connected) continue;
            WaitForSingleObject(event.value, INFINITE);
        }
        std::string result;
        try {
            std::vector<wchar_t> bytes(32768);
            auto size = ark::pipeRead(pipe.value, bytes.data(), static_cast<DWORD>(bytes.size() * sizeof(wchar_t)));
            if (size < 2 || size % 2 || bytes[size / 2 - 1] != 0) throw std::runtime_error("Invalid reload message");
            std::wstring request(bytes.data(), size / 2 - 1);
            if (request.find(L'\0') != std::wstring::npos) throw std::runtime_error("Invalid reload path");
            if (request.empty()) result = response();
            else {
                if (!reloadAvailable) throw std::runtime_error("This game build does not expose the required single-Mod reload interfaces");
                if (!ark::within(request, statusDirectory / L"reloads")) throw std::runtime_error("Reload patch must be inside the active runtime's reload directory");
                auto patch = ark::readManifest(request, true);
                if (!patch.partial || ark::normalized(patch.game) != ark::normalized(executable)) throw std::runtime_error("Reload patch does not belong to this game");
                std::map<std::wstring, std::shared_ptr<Target>> changes;
                for (const auto& entry : patch.entries) changes.emplace(ark::normalized(entry.source), entry.replacement.empty() ? nullptr : makeTarget(entry.replacement, entry.replacementHash));
                {
                    std::lock_guard lock(stateMutex);
                    auto next = targets;
                    for (auto& [source, target] : changes) { if (target) next[source] = target; else next.erase(source); }
                    targets.swap(next);
                    lastPatch = patch.file.wstring();
                }
                result = response(changes.size());
            }
        } catch (const std::exception& error) { result = "{\"success\":false,\"error\":" + ark::jsonString(error.what()) + "}"; }
        try { ark::pipeWrite(pipe.value, result.data(), static_cast<DWORD>(result.size())); char received = 0; ark::pipeRead(pipe.value, &received, 1); } catch (...) {}
        DisconnectNamedPipe(pipe.value);
    }
}
template<class T> T api(HMODULE module, const char* name) {
    auto result = reinterpret_cast<T>(GetProcAddress(module, name));
    if (!result) throw std::runtime_error(std::string("Required IL2CPP export is missing: ") + name);
    return result;
}
DWORD WINAPI reloadServer(void*) {
    try { serve(); }
    catch (const std::exception& error) { ark::writeStatus(statusDirectory, "unsupported", error.what()); }
    return 0;
}
void initializeManagedRuntime() {
    ark::writeStatus(statusDirectory, "initializing-managed", "Unity initialized; configuring resource hooks on a registered runtime thread");
    RuntimeThread attached;
    if (!attached) throw std::runtime_error("IL2CPP thread registration failed");
    auto resolve = api<void* (*)(const char*)>(gameAssembly, "il2cpp_resolve_icall");
    stringChars = api<decltype(stringChars)>(gameAssembly, "il2cpp_string_chars");
    stringLength = api<decltype(stringLength)>(gameAssembly, "il2cpp_string_length");
    stringNew = api<decltype(stringNew)>(gameAssembly, "il2cpp_string_new_utf16");
    handleNew = api<decltype(handleNew)>(gameAssembly, "il2cpp_gchandle_new");
    handleWeak = api<decltype(handleWeak)>(gameAssembly, "il2cpp_gchandle_new_weakref");
    handleTarget = api<decltype(handleTarget)>(gameAssembly, "il2cpp_gchandle_get_target");
    handleFree = api<decltype(handleFree)>(gameAssembly, "il2cpp_gchandle_free");
    unbox = api<decltype(unbox)>(gameAssembly, "il2cpp_object_unbox");
    auto assemblies = api<const void** (*)(void*, size_t*)>(gameAssembly, "il2cpp_domain_get_assemblies");
    auto image = api<void* (*)(const void*)>(gameAssembly, "il2cpp_assembly_get_image");
    auto klass = api<void* (*)(void*, const char*, const char*)>(gameAssembly, "il2cpp_class_from_name");
    auto method = api<const void* (*)(void*, const char*, int)>(gameAssembly, "il2cpp_class_get_method_from_name");
    size_t count = 0; auto all = assemblies(runtimeDomain, &count);
    for (size_t i = 0; i < count; ++i) {
        if (auto type = klass(image(all[i]), "UnityEngine", "AssetBundle")) { loadMethod = method(type, "LoadFromFile_Internal", 3); unloadMethod = method(type, "Unload", 1); }
        if (auto type = klass(image(all[i]), "UnityEngine", "AsyncOperation")) doneMethod = method(type, "get_isDone", 0);
    }
    reloadAvailable = loadMethod && unloadMethod && doneMethod;
    struct Hook { const char* name; void* replacement; void** original; } hooks[] = {
        {"UnityEngine.AssetBundle::LoadFromFile_Internal(System.String,System.UInt32,System.UInt64)", reinterpret_cast<void*>(&loadSync), reinterpret_cast<void**>(&originalSync)},
        {"UnityEngine.AssetBundle::LoadFromFileAsync_Internal(System.String,System.UInt32,System.UInt64)", reinterpret_cast<void*>(&loadAsync), reinterpret_cast<void**>(&originalAsync)},
        {"UnityEngine.AssetBundleCreateRequest::get_assetBundle()", reinterpret_cast<void*>(&getBundle), reinterpret_cast<void**>(&originalGetBundle)},
        {"UnityEngine.AssetBundle::Unload(System.Boolean)", reinterpret_cast<void*>(&unload), reinterpret_cast<void**>(&originalUnload)},
        {"UnityEngine.AssetBundle::LoadAsset_Internal(System.String,System.Type)", reinterpret_cast<void*>(&loadAsset), reinterpret_cast<void**>(&originalAsset)},
        {"UnityEngine.AssetBundle::LoadAssetAsync_Internal(System.String,System.Type)", reinterpret_cast<void*>(&loadAssetAsync), reinterpret_cast<void**>(&originalAssetAsync)},
        {"UnityEngine.AssetBundle::LoadAssetWithSubAssets_Internal(System.String,System.Type)", reinterpret_cast<void*>(&subAssets), reinterpret_cast<void**>(&originalSubAssets)},
        {"UnityEngine.AssetBundle::LoadAssetWithSubAssetsAsync_Internal(System.String,System.Type)", reinterpret_cast<void*>(&subAssetsAsync), reinterpret_cast<void**>(&originalSubAssetsAsync)}
    };
    std::vector<void*> addresses;
    for (const auto& hook : hooks) addresses.push_back(resolve(hook.name));
    if (!addresses[0] || !addresses[1]) throw BindingsNotReady("Waiting for Unity AssetBundle internal-call registration");
    for (const auto& entry : startupManifest.entries) targets.emplace(ark::normalized(entry.source), makeTarget(entry.replacement, entry.replacementHash));
    for (size_t i = 2; i < addresses.size(); ++i) {
        if (!addresses[i] || std::find(addresses.begin(), addresses.begin() + i, addresses[i]) != addresses.begin() + i) reloadAvailable = false;
    }
    for (size_t i = 0; i < (reloadAvailable ? addresses.size() : 2); ++i) {
        if (!addresses[i] || MH_CreateHook(addresses[i], hooks[i].replacement, hooks[i].original) != MH_OK) {
            if (i < 2) {
                for (size_t j = 0; j < i; ++j) MH_RemoveHook(addresses[j]);
                throw std::runtime_error("AssetBundle file interfaces are unavailable; resource hooks were not activated");
            }
            for (size_t j = 2; j < i; ++j) MH_RemoveHook(addresses[j]);
            reloadAvailable = false;
            break;
        }
    }
    for (size_t i = 0; i < (reloadAvailable ? addresses.size() : 2); ++i) MH_QueueEnableHook(addresses[i]);
    if (MH_ApplyQueued() != MH_OK) throw std::runtime_error("Resource hook activation failed");
    HANDLE server = CreateThread(nullptr, 0, reloadServer, nullptr, 0, nullptr);
    if (!server) throw std::runtime_error("Could not start the reload endpoint");
    CloseHandle(server);
    ark::writeStatus(statusDirectory, "active", reloadAvailable ? "Scoped reload protocol 2 active; initialized after Unity on a registered runtime thread." : "AssetBundle redirection active; this game build does not expose scoped reload interfaces.");
}
DWORD WINAPI initialize(void*) {
    try {
        wchar_t location[32768]{}; GetModuleFileNameW(nullptr, location, 32768); executable = location;
        if (_wcsicmp(executable.filename().c_str(), L"Arknights.exe")) return 0;
        auto length = GetEnvironmentVariableW(L"QAQR_ARKNIGHTS_MANIFEST", location, 32768);
        if (!length || length >= 32768) return 0;
        startupManifest = ark::readManifest(location, true);
        if (startupManifest.partial || ark::normalized(startupManifest.game) != ark::normalized(executable)) return 0;
        statusDirectory = startupManifest.file.parent_path();
        ark::writeStatus(statusDirectory, "waiting-module", "Waiting for GameAssembly; no managed APIs have been called");
        for (int attempt = 0; attempt < 600 && !gameAssembly; ++attempt) { gameAssembly = GetModuleHandleW(L"GameAssembly.dll"); if (!gameAssembly) Sleep(100); }
        if (!gameAssembly) throw std::runtime_error("GameAssembly.dll did not load");
        // Resolving exports is native-only. All managed setup runs after init
        // returns, or at a later VM invocation if the DLL was injected late.
        domainGet = api<decltype(domainGet)>(gameAssembly, "il2cpp_domain_get");
        threadCurrent = api<decltype(threadCurrent)>(gameAssembly, "il2cpp_thread_current");
        threadAttach = api<decltype(threadAttach)>(gameAssembly, "il2cpp_thread_attach");
        threadDetach = api<decltype(threadDetach)>(gameAssembly, "il2cpp_thread_detach");
        auto call = api<decltype(invoke)>(gameAssembly, "il2cpp_runtime_invoke");
        if (MH_Initialize() != MH_OK) throw std::runtime_error("Hook initialization failed");
        if (MH_CreateHook(reinterpret_cast<void*>(call), reinterpret_cast<void*>(&invokeManaged), reinterpret_cast<void**>(&invoke)) != MH_OK ||
            MH_QueueEnableHook(reinterpret_cast<void*>(call)) != MH_OK) throw std::runtime_error("Could not observe the managed runtime entry");
        auto init = GetProcAddress(gameAssembly, "il2cpp_init");
        auto initUtf16 = GetProcAddress(gameAssembly, "il2cpp_init_utf16");
        if (init && MH_CreateHook(reinterpret_cast<void*>(init), reinterpret_cast<void*>(&initializeUtf8), reinterpret_cast<void**>(&originalInit)) == MH_OK) MH_QueueEnableHook(reinterpret_cast<void*>(init));
        if (initUtf16 && initUtf16 != init && MH_CreateHook(reinterpret_cast<void*>(initUtf16), reinterpret_cast<void*>(&initializeUtf16), reinterpret_cast<void**>(&originalInitUtf16)) == MH_OK) MH_QueueEnableHook(reinterpret_cast<void*>(initUtf16));
        ark::writeStatus(statusDirectory, "waiting-managed-init", "Waiting for Unity to initialize IL2CPP; domain_get has not been called");
        if (MH_ApplyQueued() != MH_OK) throw std::runtime_error("Runtime observer activation failed");
    } catch (const std::exception& error) { if (!statusDirectory.empty()) ark::writeStatus(statusDirectory, "unsupported", error.what()); }
    return 0;
}
}
extern "C" __declspec(dllexport) const char* QAQR_RuntimeProtocol() { return "qaqr-arknights-bundles-2"; }
BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_DETACH) detaching.store(true);
    if (reason == DLL_PROCESS_ATTACH) { DisableThreadLibraryCalls(instance); HANDLE thread = CreateThread(nullptr, 0, initialize, nullptr, 0, nullptr); if (thread) CloseHandle(thread); }
    return TRUE;
}
