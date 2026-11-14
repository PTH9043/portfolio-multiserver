#include "CoreDefine.h"
#include "CustomAlloc.h"
#include <cstring>
#include <limits>
#include <stdexcept>
#include <cstdint>
#include <mutex>
#include <set>

using namespace Core;
void Require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
struct alignas(std::max_align_t) AlignedValue { unsigned char bytes[32]; };

int main(int argc, char** argv)
{
    // A stack header reproduces the old guard failure without leaking a pool allocation.
    if (argc == 2 && std::string_view(argv[1]) == "--probe") {
        UMemoryHeader header{};
        const bool ok = UMemoryHeader::RecordHeader(&header, MEMORYHEADER_SIZE + 32) != nullptr;
        std::cout << (ok ? "PASS" : "FAIL") << " valid header must return a payload\n";
        return ok ? 0 : 1;
    }
    int passed = 0;
    UMemoryAdiminstor memory;
    try {
        Require(UMemoryHeader::RecordHeader(nullptr, 32) == nullptr, "null header guard");
        memory.Release(nullptr);
        std::cout << "PASS null contracts\n"; ++passed;

        for (uint64 size : {0ull,1ull,15ull,16ull,17ull,495ull,496ull,497ull,511ull,512ull,513ull,1520ull,1536ull,2544ull,2560ull,4592ull,4593ull,4608ull,8192ull}) {
            void* p = memory.Allocate(size);
            Require(p != nullptr, "allocation returned null");
            Require(UMemoryHeader::DetachHeader(p)->GetAllocSize() == size + MEMORYHEADER_SIZE, "header size");
            Require(reinterpret_cast<std::uintptr_t>(p) % alignof(std::max_align_t) == 0, "fundamental alignment");
            std::memset(p, 0xA5, static_cast<size_t>(size));
            memory.Release(p);
        }
        GThreadMemoryCache.FlushToCentral();
        std::cout << "PASS 19 boundary sizes, payload writes and alignment\n"; ++passed;

        auto* first = memory.Allocate(32); memory.Release(first);
        auto* second = memory.Allocate(32);
        Require(first == second, "TLS reuse");
        memory.Release(second); GThreadMemoryCache.FlushToCentral();
        std::cout << "PASS release, reuse and flush\n"; ++passed;

        bool overflow = false;
        try { memory.Allocate(std::numeric_limits<uint64>::max()); }
        catch (const std::bad_array_new_length&) { overflow = true; }
        Require(overflow, "header addition overflow");
        std::cout << "PASS header overflow rejection\n"; ++passed;

        std::atomic<bool> workersOk{true};
        SHARED_MUTEX activeMutex;
        std::set<void*> activeBlocks;
        std::vector<std::jthread> workers;
        for (int w=0; w<4; ++w) workers.emplace_back([&, w] {
            try {
                for (int cycle=0; cycle<100; ++cycle) {
                    std::vector<void*> blocks;
                    for (int i=0; i<40; ++i) {
                        void* p = memory.Allocate(64);
                        bool unique;
                        { EXCLUSIVE_LOCK_GUARD lock(activeMutex); unique = activeBlocks.insert(p).second; }
                        if (!unique) { workersOk=false; continue; }
                        std::memset(p, w + 1, 64);
                        blocks.push_back(p);
                    }
                    for (void* p : blocks) {
                        auto* bytes = static_cast<unsigned char*>(p);
                        for (int i=0; i<64; ++i) if (bytes[i] != w + 1) workersOk=false;
                        { EXCLUSIVE_LOCK_GUARD lock(activeMutex); activeBlocks.erase(p); }
                        memory.Release(p);
                    }
                }
                GThreadMemoryCache.FlushToCentral();
            } catch (...) { workersOk=false; }
        });
        for (auto& worker : workers) worker.join();
        Require(workersOk && activeBlocks.empty(), "worker allocations, live block uniqueness, content or flush");
        std::cout << "PASS 4 workers, 16000 allocations\n"; ++passed;

        void* transferred=memory.Allocate(64);
        std::thread releaser([&]{ memory.Release(transferred); GThreadMemoryCache.FlushToCentral(); });
        releaser.join();
        std::cout << "PASS cross-thread release before manager destruction\n"; ++passed;

        // Allocator helpers use the existing singleton. Destroy containers before flushing/deleting it.
        {
            std::vector<AlignedValue, Alloc::USTLAllocator<AlignedValue>> values(10);
            Require(reinterpret_cast<std::uintptr_t>(values.data()) % alignof(AlignedValue)==0, "STL alignment");
            values.resize(50);
            auto value=MakeShared<int>(42);
            Require(*value==42, "allocate_shared");
            Alloc::USTLAllocator<AlignedValue> allocator;
            bool rejected=false;
            try { allocator.allocate(std::numeric_limits<uint64>::max()/sizeof(AlignedValue)+1); }
            catch(const std::bad_array_new_length&) { rejected=true; }
            Require(rejected,"allocator multiplication overflow");
        }
        GThreadMemoryCache.FlushToCentral();
        UCoreGlobal::DeleteInstance();
        std::cout << "PASS STL vector, allocate_shared and count overflow\n"; ++passed;
        std::cout << "RESULT " << passed << "/7 groups passed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL " << e.what() << '\n';
        GThreadMemoryCache.FlushToCentral();
        return 1;
    }
}
