#pragma once

#include <bit>
#include <type_traits>

SPACE_H(Core)

// 머리말이 그대로 메모리 복사로 오가므로 양쪽의 바이트 순서가 같아야 한다. (x86/x64 는 리틀 엔디안)
static_assert(std::endian::native == std::endian::little,
	"the packet protocol assumes little-endian on both sides");


// 패킷의 머리
struct FPacketHeader {
	uint16 type = 0;
	uint16 size = 0;
};

// 실수로 모양이 바뀌면 컴파일이 멈추게 한다.
static_assert(4 == sizeof(FPacketHeader), "packet header size changed: the protocol breaks");
static_assert(std::is_trivially_copyable_v<FPacketHeader>, "packet header must be memcpy-able");
static_assert(std::is_standard_layout_v<FPacketHeader>, "packet header must have a plain layout");

STATIC_CONSTEXPR uint64 PACKET_HEADER_SIZE{ sizeof(FPacketHeader) };
// 패킷 하나가 가질 수 있는 최대 크기(머리말 포함)
// Size 가 uint16 이라 65535 를 넘길 수 없다.
STATIC_CONSTEXPR uint64 PACKET_BUFFER_SIZE{ 1024 };
static_assert(PACKET_HEADER_SIZE < PACKET_BUFFER_SIZE&& PACKET_BUFFER_SIZE <= 0xFFFF, "packet buffer size is out of range");
STATIC_CONSTEXPR uint64 PACKET_MAX_BODY_SIZE{ PACKET_BUFFER_SIZE - PACKET_HEADER_SIZE };

using PACKET_BUFFER = ARRAY<uint8, PACKET_BUFFER_SIZE>;

// 스레드·strand 경계를 넘어 안전하게 이동·공유할 수 있는 불변 패킷 페이로드다.
// 본문은 프로젝트 메모리 풀의 불변 버퍼(SHPTR<const BUFFER>)로 보관하므로 잠금 없이 어느 스레드에서나 읽을 수 있다.
class CORE_DLL FPacketPayload final {
public:
	FPacketPayload() noexcept = default;
	explicit FPacketPayload(uint16 type, SHPTR<const BUFFER> body) noexcept;

	// 본문 바이트를 메모리 풀 불변 버퍼로 복사해 스레드 안전한 페이로드를 만든다.
	// bodySize가 PACKET_MAX_BODY_SIZE를 넘거나 본문이 있는데 body가 비어 있으면 빈(무효) 페이로드를 돌려준다.
	static FPacketPayload CopyFrom(uint16 type, const uint8* body, uint64 bodySize);

	uint16 GetType() const noexcept { return _Type; }
	const uint8* GetData() const noexcept { return (_Body && !_Body->empty()) ? _Body->data() : nullptr; }
	uint64 GetSize() const noexcept { return _Body ? static_cast<uint64>(_Body->size()) : 0; }
	bool IsEmpty() const noexcept { return GetSize() == 0; }
	bool IsValid() const noexcept { return _Type != 0; }
	const SHPTR<const BUFFER>& GetBuffer() const noexcept { return _Body; }

private:
	uint16 _Type{ 0 };
	SHPTR<const BUFFER> _Body;
};

// USession의 수신 버퍼를 복사 없이 읽기 위한 동기 스코프 전용 뷰다.
// 복사·이동을 금지(NO_COPY)해 람다 캡처나 큐 저장으로 원시 버퍼가 타 스레드로 탈출하는 실수를 컴파일 시점에 막는다.
// 다른 스레드나 strand로 패킷을 넘겨야 할 때만 Detach()로 불변 FPacketPayload를 분리한다.
class CORE_DLL FPacketView final {
public:
	explicit FPacketView(uint16 type, const uint8* body, uint64 bodySize) noexcept;
	NO_COPY(FPacketView)

	uint16 GetType() const noexcept { return _Type; }
	const uint8* GetData() const noexcept { return _Body; }
	uint64 GetSize() const noexcept { return _Size; }
	bool IsEmpty() const noexcept { return _Size == 0; }

	// 현재 수신 버퍼 본문을 불변 FPacketPayload로 분리한다(잠금 0개, 타 스레드 전달·공유 안전).
	FPacketPayload Detach() const;

private:
	uint16 _Type{ 0 };
	const uint8* _Body{ nullptr };
	uint64 _Size{ 0 };
};


SPACE_T
