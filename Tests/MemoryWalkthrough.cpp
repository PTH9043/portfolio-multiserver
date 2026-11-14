// 읽기용 실행 예제. 원본 Core를 사용하지만 서버 전체나 성능을 검사하지 않는다.
#include "CoreDefine.h"
#include "CustomMemory.h"
#include <stdexcept>

using namespace Core;
static void Check(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}

int main() {
    std::cout << "[현재 빌드의 실제 크기]\n"
        << "header=" << sizeof(UMemoryHeader)
        << " slot=" << sizeof(FPoolCacheSlot)
        << " cache_object=" << sizeof(UThreadMemoryCache) << " bytes\n";

    // 중앙 풀 두 개. 16-byte는 index 0, 80-byte는 index 4인 현재 표와 맞춘다.
    UMemoryPool smallPool{16, 0};
    UMemoryPool pool{80, 4};
    {
        // A. TLS 없이 중앙 풀만으로도 메모리를 다시 쓸 수 있다.
        auto* first = pool.Pop();
        void* payload = UMemoryHeader::AttachHeader(first, 64 + sizeof(UMemoryHeader));
        Check(UMemoryHeader::DetachHeader(payload) == first, "header round trip");
        uint64 size = 0;
        Check(first->TryRelease(size), "release header");
        pool.Push(first);
        auto* again = pool.Pop();
        Check(again == first, "central reuse");
        std::cout << "A PASS: TLS 없이 중앙 풀에 반환한 블록을 다시 사용\n";

        // B. 캐시는 중앙 풀 앞에 추가한 재사용 경로다.
        // 실제 서버의 thread_local 대신 여기서는 한 캐시 객체로 동작 순서만 보여 준다.
        UThreadMemoryCache cache;
        Check(cache.TryPush(&pool, again), "cache push");
        Check(cache.GetCachedBytes() == 80, "cache bytes after push");
        Check(cache.TryPop(&smallPool) == nullptr, "size classes must not mix");
        auto* cached = cache.TryPop(&pool);
        Check(cached == again && cache.GetCachedBytes() == 0, "cache pop");
        pool.Push(cached);
        cache.FlushToCentral(); // 빈 슬롯도 풀 연결을 정리한다.
        std::cout << "B PASS: 80-byte 칸의 주소를 16-byte 요청에 주지 않음\n";

        // C. 33개를 먼저 확보해야 32개 보관 제한을 보여 줄 수 있다.
        std::vector<UMemoryHeader*> blocks;
        blocks.reserve(33);
        for (int i = 0; i < 33; ++i) blocks.push_back(pool.Pop());
        unsigned accepted = 0, refused = 0;
        for (auto* block : blocks) {
            if (cache.TryPush(&pool, block)) ++accepted;
            else { ++refused; pool.Push(block); }
        }
        Check(accepted == 32 && refused == 1, "32-slot limit");
        Check(cache.GetCachedBytes() == 32 * 80, "cached bytes before flush");
        std::cout << "C PASS: 개인 보관 32개, 거절 1개, cached_bytes="
            << cache.GetCachedBytes() << '\n';
        cache.FlushToCentral();
        Check(cache.GetCachedBytes() == 0, "flush bytes");
        std::cout << "D PASS: flush 뒤 개인 보관량 0, 블록은 중앙 풀에서 소유\n";
    }
    // cache가 pool보다 먼저 사라진다. pool 소멸자는 중앙에 남은 블록을 해제한다.
    std::cout << "완료: 정상 경로 설명용 검사. 메모리 풀의 미해결 결함을 수정한 코드는 아님.\n";
}
