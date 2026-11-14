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


SPACE_T
