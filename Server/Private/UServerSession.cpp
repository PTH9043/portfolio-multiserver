#include "UServerSession.h"
#include "UCoreInstance.h"
#include "ULogger.h"
#include "Packet.h"

#include "packet_type.pb.h"
#include "login.pb.h"
#include "chat.pb.h"
#include "room.pb.h"

#include <cstring>
#include <string>
#include <utility>

#include "RuntimeTrace.h"
#define RUNTIME_TRACE_SOURCE "Server/Private/UServerSession.cpp"

// protobuf 생성 타입의 짧은 이름. 이 파일 안에서만 쓴다. (2026-11-19 사용자 선택)
namespace Protocol = multiplayer::protocol;

SPACE_H(Server)

UServerSession::UServerSession(Core::SHPTR<Core::UCoreInstance> coreInstance, Core::TCPSOCKET socket)
	: USession{ std::move(coreInstance), std::move(socket) }
{
	RUNTIME_TRACE_EVENT(Create, UServerSession, UServerSession, GetRuntimeTraceID());
}

UServerSession::~UServerSession()
{
	RUNTIME_TRACE_EVENT(Destroy, UServerSession, ~UServerSession, GetRuntimeTraceID());
}

void UServerSession::ProcessingData(Core::uint8* packetData, Core::uint64 packetSize)
{
	RUNTIME_TRACE_SCOPE(UServerSession, ProcessingData, GetRuntimeTraceID());
	Core::FPacketHeader header{};
	std::memcpy(&header, packetData, Core::PACKET_HEADER_SIZE);

	// USession::CombinePacket이 머리말의 본문 크기를 PACKET_MAX_BODY_SIZE 안으로 확인한 뒤 넘긴다.
	const Core::uint8* body{ packetData + Core::PACKET_HEADER_SIZE };
	const Core::uint64 bodySize{ packetSize - Core::PACKET_HEADER_SIZE };

	switch (header.type) {
	case Protocol::CS_LOGIN:
		HandlePacket<Protocol::CSLogin>(header.type, body, bodySize);
		break;
	case Protocol::CS_CHAT:
		HandlePacket<Protocol::CSChat>(header.type, body, bodySize);
		break;
	case Protocol::CS_ROOM_USERS:
		HandlePacket<Protocol::CSRoomUsers>(header.type, body, bodySize);
		break;
	default:
		RejectPacket(header.type, "undefined packet type");
		break;
	}
}

template<class TPacket>
void UServerSession::HandlePacket(Core::uint16 packetType, const Core::uint8* body, Core::uint64 bodySize)
{
	RUNTIME_TRACE_SCOPE(UServerSession, HandlePacket, GetRuntimeTraceID());
	// bodySize는 PACKET_MAX_BODY_SIZE(1020) 이하라 int로 바꿔도 값이 잘리지 않는다.
	// proto3 string이 UTF-8이 아니어도 해석에 실패한다.
	TPacket packet;
	if (!packet.ParseFromArray(body, static_cast<int>(bodySize))) {
		RejectPacket(packetType, "parse failed");
		return;
	}

	// 아직 패킷별 처리가 없어 받은 패킷의 이름과 본문 크기만 기록한다. 로그인·채팅 처리를 붙일 때 이 기록을 바꾼다.
	// protobuf의 DebugString 계열은 사람이 읽는 형식을 보장하지 않으므로(한글은 8진수 이스케이프) 쓰지 않는다.
	const auto coreInstance = GetCoreInstance();
	const auto logger = coreInstance ? coreInstance->GetLogger() : nullptr;
	if (logger) {
		Core::FString message{ "UServerSession received " };
		message << Protocol::ECSPacket_Name(static_cast<Protocol::ECSPacket>(packetType)) << " ("
			<< std::to_string(bodySize) << " bytes)";
		logger->TryLog(Core::ELogLevel::Info, message.View());
	}
}

void UServerSession::RejectPacket(Core::uint16 packetType, Core::STRINGVIEW reason)
{
	RUNTIME_TRACE_SCOPE(UServerSession, RejectPacket, GetRuntimeTraceID());
	const auto coreInstance = GetCoreInstance();
	const auto logger = coreInstance ? coreInstance->GetLogger() : nullptr;
	if (logger) {
		Core::FString message{ "UServerSession rejected packet type " };
		message << std::to_string(packetType) << " (" << reason << "); closing the connection";
		logger->TryLog(Core::ELogLevel::Warning, message.View());
	}
	Close();
}

SPACE_T
