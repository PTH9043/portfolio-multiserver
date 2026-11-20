#pragma once

#include "CoreDefine.h"
#include "Packet.h"

SPACE_H(Server)

// 기본 채팅방 식별자·정원과 프로토콜 입력 길이 상한이다.
// 이 상수들을 바꿀 때 아래 static_assert가 PACKET_MAX_BODY_SIZE(1020 바이트) 초과를 컴파일 단계에서 막는다.
STATIC_CONSTEXPR Core::uint64 DEFAULT_GAME_ROOM_ID{ 1 };
STATIC_CONSTEXPR Core::uint32 DEFAULT_ROOM_MAX_USERS{ 1000 };
STATIC_CONSTEXPR Core::uint64 NICKNAME_MIN_CHARS{ 2 };
STATIC_CONSTEXPR Core::uint64 NICKNAME_MAX_CHARS{ 12 };
// 완성형 한글(U+AC00..U+D7A3)은 UTF-8 3바이트이므로 12자 최대 36바이트다.
STATIC_CONSTEXPR Core::uint64 NICKNAME_MAX_UTF8_BYTES{ NICKNAME_MAX_CHARS * 3 };
STATIC_CONSTEXPR Core::uint64 CHAT_MIN_CHARS{ 1 };
STATIC_CONSTEXPR Core::uint64 CHAT_MAX_CHARS{ 200 };
// 유효한 UTF-8 코드포인트는 최대 4바이트이므로 200자 최대 800바이트다.
STATIC_CONSTEXPR Core::uint64 CHAT_MAX_UTF8_BYTES{ CHAT_MAX_CHARS * 4 };
STATIC_CONSTEXPR Core::uint32 ROOM_USERS_PAGE_SIZE{ 20 };
// 동시 입장·퇴장 폭주 시 SC_ROOM_USER_COUNT 브로드캐스트를 하나로 병합하는 간격(ms)이다.
STATIC_CONSTEXPR Core::MILLISECONDS ROOM_USER_COUNT_COALESCE_INTERVAL{ 25 };

// proto3 직렬화 최악 크기 계산 (필드 번호 1..15 기준 태그 1바이트).
STATIC_CONSTEXPR Core::uint64 PROTOBUF_UINT32_FIELD_MAX_BYTES{ 1 + 5 };
STATIC_CONSTEXPR Core::uint64 PROTOBUF_ENUM_FIELD_MAX_BYTES{ 1 + 10 };

constexpr Core::uint64 ProtobufStringFieldMaxBytes(Core::uint64 maxPayloadBytes) noexcept
{
	const Core::uint64 lengthPrefixBytes{ maxPayloadBytes < 128 ? 1ull : 2ull };
	return 1 + lengthPrefixBytes + maxPayloadBytes;
}

// 패킷별 직렬화 상한이 헤더를 제외한 최대 본문 크기(PACKET_MAX_BODY_SIZE) 안에 들어오는지 컴파일 시점에 검증한다.
STATIC_CONSTEXPR Core::uint64 CS_LOGIN_MAX_BYTES{ ProtobufStringFieldMaxBytes(NICKNAME_MAX_UTF8_BYTES) };
STATIC_CONSTEXPR Core::uint64 CS_CHAT_MAX_BYTES{ ProtobufStringFieldMaxBytes(CHAT_MAX_UTF8_BYTES) };
STATIC_CONSTEXPR Core::uint64 CS_ROOM_USERS_MAX_BYTES{ PROTOBUF_UINT32_FIELD_MAX_BYTES };
STATIC_CONSTEXPR Core::uint64 SC_LOGIN_RESULT_MAX_BYTES{
	PROTOBUF_ENUM_FIELD_MAX_BYTES + PROTOBUF_UINT32_FIELD_MAX_BYTES
};
STATIC_CONSTEXPR Core::uint64 SC_ROOM_USER_COUNT_MAX_BYTES{ PROTOBUF_UINT32_FIELD_MAX_BYTES };
STATIC_CONSTEXPR Core::uint64 SC_CHAT_MAX_BYTES{
	ProtobufStringFieldMaxBytes(NICKNAME_MAX_UTF8_BYTES) + ProtobufStringFieldMaxBytes(CHAT_MAX_UTF8_BYTES)
};
STATIC_CONSTEXPR Core::uint64 SC_ROOM_USERS_MAX_BYTES{
	(PROTOBUF_UINT32_FIELD_MAX_BYTES * 2)
	+ (static_cast<Core::uint64>(ROOM_USERS_PAGE_SIZE) * ProtobufStringFieldMaxBytes(NICKNAME_MAX_UTF8_BYTES))
};

static_assert(CS_LOGIN_MAX_BYTES <= Core::PACKET_MAX_BODY_SIZE, "CSLogin exceeds PACKET_MAX_BODY_SIZE");
static_assert(CS_CHAT_MAX_BYTES <= Core::PACKET_MAX_BODY_SIZE, "CSChat exceeds PACKET_MAX_BODY_SIZE");
static_assert(CS_ROOM_USERS_MAX_BYTES <= Core::PACKET_MAX_BODY_SIZE, "CSRoomUsers exceeds PACKET_MAX_BODY_SIZE");
static_assert(SC_LOGIN_RESULT_MAX_BYTES <= Core::PACKET_MAX_BODY_SIZE, "SCLoginResult exceeds PACKET_MAX_BODY_SIZE");
static_assert(SC_ROOM_USER_COUNT_MAX_BYTES <= Core::PACKET_MAX_BODY_SIZE, "SCRoomUserCount exceeds PACKET_MAX_BODY_SIZE");
static_assert(SC_CHAT_MAX_BYTES <= Core::PACKET_MAX_BODY_SIZE, "SCChat exceeds PACKET_MAX_BODY_SIZE");
static_assert(SC_ROOM_USERS_MAX_BYTES <= Core::PACKET_MAX_BODY_SIZE, "SCRoomUsers exceeds PACKET_MAX_BODY_SIZE");

// text[index..]에서 UTF-8 코드포인트 하나를 엄격하게 해독한다(과장 인코딩·서로게이트 거부).
// 성공하면 index를 다음 코드포인트 위치로 옮기고 true를 돌려준다.
inline bool DecodeUtf8CodePoint(Core::STRINGVIEW text, Core::uint64& index, Core::uint32& outCodePoint) noexcept
{
	const Core::uint64 length{ text.size() };
	if (index >= length)
		return false;

	const Core::uint8 b0{ static_cast<Core::uint8>(text[index]) };
	if (b0 <= 0x7Fu) {
		outCodePoint = b0;
		index += 1;
		return true;
	}

	if (b0 >= 0xC2u && b0 <= 0xDFu) {
		if (index + 2 > length)
			return false;
		const Core::uint8 b1{ static_cast<Core::uint8>(text[index + 1]) };
		if ((b1 & 0xC0u) != 0x80u)
			return false;
		outCodePoint = (static_cast<Core::uint32>(b0 & 0x1Fu) << 6) | static_cast<Core::uint32>(b1 & 0x3Fu);
		index += 2;
		return true;
	}

	if (b0 >= 0xE0u && b0 <= 0xEFu) {
		if (index + 3 > length)
			return false;
		const Core::uint8 b1{ static_cast<Core::uint8>(text[index + 1]) };
		const Core::uint8 b2{ static_cast<Core::uint8>(text[index + 2]) };
		if ((b1 & 0xC0u) != 0x80u || (b2 & 0xC0u) != 0x80u)
			return false;
		if (b0 == 0xE0u && b1 < 0xA0u)
			return false;
		if (b0 == 0xEDu && b1 >= 0xA0u)
			return false;
		outCodePoint = (static_cast<Core::uint32>(b0 & 0x0Fu) << 12)
			| (static_cast<Core::uint32>(b1 & 0x3Fu) << 6)
			| static_cast<Core::uint32>(b2 & 0x3Fu);
		index += 3;
		return true;
	}

	if (b0 >= 0xF0u && b0 <= 0xF4u) {
		if (index + 4 > length)
			return false;
		const Core::uint8 b1{ static_cast<Core::uint8>(text[index + 1]) };
		const Core::uint8 b2{ static_cast<Core::uint8>(text[index + 2]) };
		const Core::uint8 b3{ static_cast<Core::uint8>(text[index + 3]) };
		if ((b1 & 0xC0u) != 0x80u || (b2 & 0xC0u) != 0x80u || (b3 & 0xC0u) != 0x80u)
			return false;
		if (b0 == 0xF0u && b1 < 0x90u)
			return false;
		if (b0 == 0xF4u && b1 > 0x8Fu)
			return false;
		outCodePoint = (static_cast<Core::uint32>(b0 & 0x07u) << 18)
			| (static_cast<Core::uint32>(b1 & 0x3Fu) << 12)
			| (static_cast<Core::uint32>(b2 & 0x3Fu) << 6)
			| static_cast<Core::uint32>(b3 & 0x3Fu);
		index += 4;
		return true;
	}

	return false;
}

// 완성형 한글(U+AC00..U+D7A3), ASCII 영문 대소문자, ASCII 숫자만 허용한다.
inline bool IsAllowedNicknameCodePoint(Core::uint32 codePoint) noexcept
{
	if (codePoint >= 0xAC00u && codePoint <= 0xD7A3u)
		return true;
	if (codePoint >= static_cast<Core::uint32>('A') && codePoint <= static_cast<Core::uint32>('Z'))
		return true;
	if (codePoint >= static_cast<Core::uint32>('a') && codePoint <= static_cast<Core::uint32>('z'))
		return true;
	if (codePoint >= static_cast<Core::uint32>('0') && codePoint <= static_cast<Core::uint32>('9'))
		return true;
	return false;
}

// 닉네임 규칙: UTF-8 2~12자, 완성형 한글·영문·숫자만 허용(공백·특수문자 불가).
inline bool IsValidNickname(Core::STRINGVIEW nickname) noexcept
{
	if (nickname.empty() || nickname.size() > NICKNAME_MAX_UTF8_BYTES)
		return false;

	Core::uint64 index{ 0 };
	Core::uint64 charCount{ 0 };
	while (index < nickname.size()) {
		Core::uint32 codePoint{ 0 };
		if (!DecodeUtf8CodePoint(nickname, index, codePoint))
			return false;
		if (!IsAllowedNicknameCodePoint(codePoint))
			return false;
		++charCount;
		if (charCount > NICKNAME_MAX_CHARS)
			return false;
	}

	return charCount >= NICKNAME_MIN_CHARS && charCount <= NICKNAME_MAX_CHARS;
}

// 채팅 본문 규칙: 유효한 UTF-8 1~200자.
inline bool IsValidChatText(Core::STRINGVIEW text) noexcept
{
	if (text.empty() || text.size() > CHAT_MAX_UTF8_BYTES)
		return false;

	Core::uint64 index{ 0 };
	Core::uint64 charCount{ 0 };
	while (index < text.size()) {
		Core::uint32 codePoint{ 0 };
		if (!DecodeUtf8CodePoint(text, index, codePoint))
			return false;
		++charCount;
		if (charCount > CHAT_MAX_CHARS)
			return false;
	}

	return charCount >= CHAT_MIN_CHARS && charCount <= CHAT_MAX_CHARS;
}

SPACE_T
