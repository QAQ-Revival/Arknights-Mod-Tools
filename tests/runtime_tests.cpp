// Exercise the production routing and real local pipe using a small fake Unity
// boundary. This is deliberately not evidence of actual game compatibility.
#include "../native/runtime.cpp"
#include <fstream>
#include <iostream>
#include <thread>

namespace fixture {
thread_local bool registered = false;
std::atomic<int> attaches = 0, detaches = 0, unknownCalls = 0;
int domainQueries = 0, setups = 0;
bool bindingsReady = false;
void requireThread() { if (!registered) { ++unknownCalls; throw std::runtime_error("Collecting from unknown thread"); } }
void* current() { return registered ? reinterpret_cast<void*>(1) : nullptr; }
void* attach(void*) { ++attaches; registered = true; return reinterpret_cast<void*>(1); }
void detach(void*) { requireThread(); ++detaches; registered = false; }
void* domain() { requireThread(); ++domainQueries; return reinterpret_cast<void*>(1); }
void setup() { requireThread(); ++setups; if (!bindingsReady) throw BindingsNotReady("Bindings not registered yet"); enterManagedRuntime(); }
struct Bundle { ark::fs::path file; bool unloaded = false; bool destroyed = false; };
struct Request { bool done = false; Bundle* bundle = nullptr; };
std::map<uint32_t, void*> handles;
std::map<uint32_t, bool> weakHandles;
std::mutex handleMutex;
uint32_t nextHandle = 1;
std::map<std::wstring, int> reads;
uint32_t retain(void* object, bool) { requireThread(); std::lock_guard lock(handleMutex); auto id = nextHandle++; handles[id] = object; return id; }
void* target(uint32_t id) { requireThread(); std::lock_guard lock(handleMutex); return handles.contains(id) ? handles[id] : nullptr; }
void release(uint32_t id) { requireThread(); std::lock_guard lock(handleMutex); handles.erase(id); }
uint32_t weak(void* object, bool flag) { auto id = retain(object, flag); std::lock_guard lock(handleMutex); weakHandles[id] = true; return id; }
void collect(void* object) {
    std::lock_guard lock(handleMutex);
    for (auto [id, value] : handles) if (value == object && !weakHandles.contains(id)) return;
    for (auto& [_, value] : handles) if (value == object) value = nullptr;
}
void* load(void* name, uint32_t, uint64_t) {
    auto file = ark::fs::path(*static_cast<std::wstring*>(name));
    if (!ark::fs::exists(file)) return nullptr;
    reads[ark::normalized(file)]++;
    return new Bundle{file};
}
void* loadAsync(void* name, uint32_t crc, uint64_t offset) { return new Request{false, static_cast<Bundle*>(load(name, crc, offset))}; }
void* get(void* request) { auto value = static_cast<Request*>(request); value->done = true; return value->bundle; }
void unload(void* object, bool destroy) { auto bundle = static_cast<Bundle*>(object); bundle->unloaded = true; bundle->destroyed = destroy; }
void* asset(void* object, void*, void*) { if (static_cast<Bundle*>(object)->unloaded) throw std::runtime_error("A stale cached wrapper reached Unity"); return object; }
void* assetAsync(void* object, void*, void*) { return new Request{false, static_cast<Bundle*>(asset(object, nullptr, nullptr))}; }
void* call(const void* method, void* object, void** args, void** exception) {
    requireThread();
    *exception = nullptr;
    if (method == loadMethod) return ::loadSync(args[0], *static_cast<uint32_t*>(args[1]), *static_cast<uint64_t*>(args[2]));
    if (method == unloadMethod) { ::unload(object, *static_cast<bool*>(args[0])); return nullptr; }
    if (method == doneMethod) return &static_cast<Request*>(object)->done;
    *exception = reinterpret_cast<void*>(1); return nullptr;
}
void check(bool condition, const char* text) { if (!condition) throw std::runtime_error(text); }
}
int wmain(int argc, wchar_t** argv) {
    using fixture::check;
    try {
        check(argc == 2, "Supply a new isolated test directory");
        auto root = ark::fs::absolute(argv[1]); check(!ark::fs::exists(root), "Test directory already exists");
        executable = root / L"game/Arknights.exe"; statusDirectory = root / L"output";
        ark::fs::create_directories(executable.parent_path() / L"Arknights_Data");
        ark::fs::create_directories(statusDirectory / L"reloads");
        std::ofstream(executable) << "synthetic fixture, never executed";
        auto original = [&](const wchar_t* name) { auto file = executable.parent_path() / L"Arknights_Data" / name; std::ofstream(file) << "original"; return file; };
        auto a = original(L"a.ab"), b = original(L"b.ab"), c = original(L"c.ab");
        runtimeDomain = reinterpret_cast<void*>(1);
        threadCurrent = fixture::current; threadAttach = fixture::attach; threadDetach = fixture::detach;
        fixture::registered = true;
        domainGet = fixture::domain;
        auto realInitializer = managedInitializer;
        managedInitializer = fixture::setup;
        runtimeDomain = nullptr;
        std::thread bootstrap([] { enterManagedRuntime(); });
        bootstrap.join();
        check(fixture::domainQueries == 0 && fixture::setups == 0, "Native startup queried the allocating domain API");
        ++initializationDepth;
        enterManagedRuntime();
        --initializationDepth;
        check(fixture::domainQueries == 0, "Resource setup ran before Unity initialization returned");
        enterManagedRuntime();
        check(initializationState.load() == 0 && fixture::setups == 1, "Early Unity bindings were treated as a permanent failure");
        fixture::bindingsReady = true;
        nextInitializationAttempt.store(0);
        enterManagedRuntime();
        enterManagedRuntime();
        check(fixture::domainQueries == 2 && fixture::setups == 2 && initializationState.load() == 2, "Managed startup was repeated or re-entered");
        managedInitializer = realInitializer;
        stringChars = [](void* s) { fixture::requireThread(); return static_cast<std::wstring*>(s)->c_str(); };
        stringLength = [](void* s) { fixture::requireThread(); return static_cast<int32_t>(static_cast<std::wstring*>(s)->size()); };
        stringNew = [](const wchar_t* s, int32_t n) -> void* { fixture::requireThread(); return new std::wstring(s, n); };
        handleNew = fixture::retain; handleWeak = fixture::weak; handleTarget = fixture::target; handleFree = fixture::release;
        reloadAvailable = true;
        invoke = fixture::call; unbox = [](void* object) { fixture::requireThread(); return object; };
        loadMethod = reinterpret_cast<void*>(1); unloadMethod = reinterpret_cast<void*>(2); doneMethod = reinterpret_cast<void*>(3);
        originalSync = fixture::load; originalAsync = fixture::loadAsync; originalGetBundle = fixture::get; originalUnload = fixture::unload;
        originalAsset = fixture::asset; originalAssetAsync = fixture::assetAsync;
        auto name = [&](const ark::fs::path& file) { auto value = file.wstring(); return stringNew(value.data(), static_cast<int32_t>(value.size())); };
        auto oldA = static_cast<fixture::Bundle*>(::loadSync(name(ark::fs::relative(a)), 0, 0));
        auto untouched = ::loadSync(name(b), 0, 0);
        auto pendingC = ::loadAsync(name(c), 0, 0);
        auto baselineHash = ark::sha256(a);
        auto patch = [&](const std::wstring& id, const ark::fs::path& source, bool remove = false, bool corrupt = false) {
            auto directory = statusDirectory / L"reloads" / id; ark::fs::create_directories(directory);
            auto replacement = directory / L"changed.ab"; std::ofstream(replacement) << ark::utf8(id);
            auto file = directory / L"manifest.tsv";
            std::ofstream stream(file, std::ios::binary);
            stream << "QAQR-ARKNIGHTS-PATCH\t1\nGAME\t" << ark::utf8(executable.wstring()) << '\n';
            if (remove) stream << "REMOVE\tArknights_Data/" << ark::utf8(source.filename().wstring()) << '\t' << ark::sha256(source) << '\n';
            else stream << "BUNDLE\tArknights_Data/" << ark::utf8(source.filename().wstring()) << "\tchanged.ab\t" << ark::sha256(source) << '\t' << (corrupt ? std::string(64, '0') : ark::sha256(replacement)) << '\n';
            stream.close(); return file;
        };
        std::thread([] { try { serve(); } catch (...) {} }).detach();
        for (int i = 0; i < 100 && !WaitNamedPipeW(ark::pipeName(GetCurrentProcessId()).c_str(), 20); ++i) Sleep(10);
        auto send = [&](const ark::fs::path& file) { return ark::runtimeRequest(GetCurrentProcessId(), file.wstring()); };
        auto first = patch(L"first", a);
        check(send(first).find("\"success\":true") != std::string::npos, "Scoped patch rejected");
        check(fixture::attaches.load() == fixture::detaches.load(), "Idle reload server kept a thread registered and would block Unity shutdown");
        auto newA = static_cast<fixture::Bundle*>(::loadAsset(oldA, nullptr, nullptr));
        check(newA->file == first.parent_path() / L"changed.ab", "Cached bundle did not follow its replacement");
        check(oldA->unloaded && !oldA->destroyed, "Displayed objects were destroyed");
        std::exception_ptr workerError;
        std::thread worker([&] {
            try {
                check(!fixture::registered, "Foreign worker unexpectedly started registered");
                check(::loadAsset(oldA, nullptr, nullptr) == newA, "Foreign-thread asset read lost its replacement");
                check(!fixture::registered, "Foreign worker registration was not released");
                { Managed retained(newA); check(retained.get() == newA, "Foreign-thread handle access failed"); }
                check(!fixture::registered, "Foreign-thread handle cleanup leaked registration");
            } catch (...) { workerError = std::current_exception(); }
        });
        worker.join();
        if (workerError) std::rethrow_exception(workerError);
        check(fixture::unknownCalls.load() == 0 && fixture::attaches.load() > 0 && fixture::detaches.load() > 0, "An unregistered thread reached the GC boundary");
        check(fixture::registered, "A Unity-owned thread was detached");
        fixture::collect(newA);
        check(::loadAsset(oldA, nullptr, nullptr) == newA, "Forwarded replacement was collected while its cached wrapper was still in use");
        check(::loadAsset(untouched, nullptr, nullptr) == untouched && fixture::reads[ark::normalized(b)] == 1, "Unrelated bundle reloaded");
        auto reading = static_cast<fixture::Request*>(::loadAssetAsync(oldA, nullptr, nullptr));
        auto second = patch(L"second", a); send(second);
        check(::loadAsset(oldA, nullptr, nullptr) == newA && !newA->unloaded, "Pending asynchronous read was invalidated");
        reading->done = true;
        check(static_cast<fixture::Bundle*>(::loadAsset(oldA, nullptr, nullptr))->file == second.parent_path() / L"changed.ab", "Completed async read blocked reload");
        auto third = patch(L"third", c); send(third);
        auto oldC = ::getBundle(pendingC);
        check(static_cast<fixture::Bundle*>(::loadAsset(oldC, nullptr, nullptr))->file == third.parent_path() / L"changed.ab", "Async bundle request was not tracked");
        check(send(patch(L"invalid", a, false, true)).find("\"success\":false") != std::string::npos, "Corrupt patch accepted");
        send(patch(L"remove", a, true));
        check(ark::normalized(static_cast<fixture::Bundle*>(::loadAsset(oldA, nullptr, nullptr))->file) == ark::normalized(a), "Disabling did not restore the original bundle");
        check(ark::sha256(a) == baselineHash && fixture::reads[ark::normalized(b)] == 1, "Original or unrelated resources changed");
        check(send(statusDirectory / L"outside.tsv").find("\"success\":false") != std::string::npos, "Out-of-scope patch accepted");
        originalShutdown = [] { fixture::registered = false; };
        shutdownRuntime();
        check(detaching.load() && !fixture::registered, "Runtime shutdown did not precede domain teardown");
        { RuntimeThread stopped; check(!stopped, "Resource hook re-entered the stopped VM"); }
        check(fixture::attaches.load() == fixture::detaches.load(), "Reload thread registration leaked at shutdown");
        std::cout << "Runtime: deferred Unity startup, scoped GC registration, idle server detachment, shutdown, scoped reload and async reads verified; no game executed.\n" << std::flush;
        ExitProcess(0);
    } catch (const std::exception& error) { std::cerr << error.what() << '\n' << std::flush; ExitProcess(1); }
}
