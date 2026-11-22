#include "UPawn.h"
#include "UCoreInstance.h"
#include "ULogger.h"
#include "USession.h"

#include "packet_type.pb.h"
#include "room.pb.h"

#include <string>
#include <utility>

#include "RuntimeTrace.h"
#define RUNTIME_TRACE_SOURCE "Server/Private/UPawn.cpp"

namespace Protocol = multiplayer::protocol;

SPACE_H(Server)

UPawn::UPawn(Core::SHPTR<Core::UCoreInstance> coreInstance)
	: UActor{ std::move(coreInstance) }
{
	RUNTIME_TRACE_EVENT(Create, UPawn, UPawn, GetRuntimeTraceID());
}

UPawn::~UPawn()
{
	RUNTIME_TRACE_EVENT(Destroy, UPawn, ~UPawn, GetRuntimeTraceID());
}

// 모든 통신 사물에서 공통으로 허용되는 CS_PING 하트비트를 선처리하고 나머지는 파생 OnDomainPacket으로 내려보낸다.
void UPawn::OnReceivePacket(const Core::FPacketView& packet)
{
	RUNTIME_TRACE_SCOPE(UPawn, OnReceivePacket, GetRuntimeTraceID());
	if (packet.GetType() == Protocol::CS_PING) {
		HandlePacket<Protocol::CSPing>(packet, [this](const Protocol::CSPing& ping) {
			Protocol::SCPong response;
			response.set_client_time_ms(ping.client_time_ms());
			SendPacket(Protocol::SC_PONG, response);
		});
		return;
	}

	OnDomainPacket(packet);
}

// 프로토콜 위반 사유를 Warning으로 기록하고 장착된 세션을 즉시 닫는다.
void UPawn::RejectPacket(const Core::uint16 packetType, const Core::STRINGVIEW reason)
{
	RUNTIME_TRACE_SCOPE(UPawn, RejectPacket, GetRuntimeTraceID());
	OnPacketRejected();
	const auto coreInstance = GetCoreInstance();
	const auto logger = coreInstance ? coreInstance->GetLogger() : nullptr;
	if (logger) {
		Core::FString message{ GetPawnTypeName() };
		message << " rejected packet type " << std::to_string(packetType)
				<< " (" << reason << "); closing the connection";
		logger->TryLog(Core::ELogLevel::Warning, message.View());
	}
	if (const auto session = GetSession())
		session->Close();
}

void UPawn::SendRawFrame(const Core::uint16 packetType, const void* data, const Core::uint64 bodySize)
{
	if (const auto session = GetSession())
		session->Send(data, bodySize, packetType);
}

void UPawn::LogOversizedOutgoingPacket(const Core::uint16 packetType, const Core::uint64 bodySize) const
{
	const auto coreInstance = GetCoreInstance();
	const auto logger = coreInstance ? coreInstance->GetLogger() : nullptr;
	if (!logger)
		return;
	Core::FString message{ GetPawnTypeName() };
	message << " skipped oversized packet type " << std::to_string(packetType)
			<< " (" << std::to_string(bodySize) << " bytes)";
	logger->TryLog(Core::ELogLevel::Warning, message.View());
}

void UPawn::LogSerializeFailure(const Core::uint16 packetType) const
{
	const auto coreInstance = GetCoreInstance();
	const auto logger = coreInstance ? coreInstance->GetLogger() : nullptr;
	if (!logger)
		return;
	Core::FString message{ GetPawnTypeName() };
	message << " failed to serialize packet type " << std::to_string(packetType);
	logger->TryLog(Core::ELogLevel::Warning, message.View());
}

SPACE_T
