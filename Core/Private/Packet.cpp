#include "CoreDefine.h"
#include "Packet.h"

#include <cstring>
#include <utility>

SPACE_H(Core)

// 이미 준비된 불변 버퍼로 페이로드를 생성한다.
FPacketPayload::FPacketPayload(const uint16 type, SHPTR<const BUFFER> body) noexcept
	: _Type{ type },
	  _Body{ std::move(body) }
{
}

// 원시 본문을 메모리 풀의 불변 버퍼로 복사해 스레드 간 안전한 페이로드를 만든다.
FPacketPayload FPacketPayload::CopyFrom(const uint16 type, const uint8* body, const uint64 bodySize)
{
	if (type == 0 || bodySize > PACKET_MAX_BODY_SIZE || (bodySize != 0 && body == nullptr))
		return {};

	// 빈 본문은 힙·풀 할당 없이 타입만 담아 반환한다.
	if (bodySize == 0)
		return FPacketPayload{ type, {} };

	auto copied = MakeShared<BUFFER>(bodySize);
	std::memcpy(copied->data(), body, bodySize);
	return FPacketPayload{ type, std::move(copied) };
}

// 수신 버퍼 구간을 복사 없이 가리키는 동기 뷰를 만든다(빈 본문이면 포인터·크기를 0으로 정규화).
FPacketView::FPacketView(const uint16 type, const uint8* body, const uint64 bodySize) noexcept
	: _Type{ type },
	  _Body{ (body != nullptr && bodySize != 0) ? body : nullptr },
	  _Size{ (body != nullptr && bodySize != 0) ? bodySize : 0 }
{
}

// 타 스레드·strand로 넘기기 위해 현재 뷰 본문을 불변 FPacketPayload로 복사 분리한다.
FPacketPayload FPacketView::Detach() const
{
	return FPacketPayload::CopyFrom(_Type, _Body, _Size);
}

SPACE_T