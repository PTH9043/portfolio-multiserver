#include "UPacketHandlerBase.h"
#include "UCoreInstance.h"
#include "ULogger.h"
#include "USession.h"

#include "packet_type.pb.h"
#include "room.pb.h"

#include <string>
#include <utility>

#include "RuntimeTrace.h"
#define RUNTIME_TRACE_SOURCE "Server/Private/UPacketHandlerBase.cpp"

namespace Protocol = multiplayer::protocol;

SPACE_H(Server)

UPacketHandlerBase::UPacketHandlerBase(Core::SHPTR<Core::UCoreInstance> coreInstance,
	Core::SHPTR<Core::USession> session, const Core::STRINGVIEW handlerName)
	: _HandlerCoreInstance{ std::move(coreInstance) },
	  _Session{ std::move(session) },
	  _HandlerName{ handlerName }
{
}

// 모든 연결 상태(로그인 전·후)에서 공통으로 허용되는 CS_PING 하트비트를 선처리하고 나머지는 파생에 위임한다.
void UPacketHandlerBase::OnReceivePacket(const Core::FPacketView& packet)
{
	RUNTIME_TRACE_SCOPE(UPacketHandlerBase, OnReceivePacket, 0);
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

// 프로토콜 위반 사유를 Warning으로 기록하고 연결된 세션을 즉시 닫는다.
void UPacketHandlerBase::RejectPacket(const Core::uint16 packetType, const Core::STRINGVIEW reason)
{
	RUNTIME_TRACE_SCOPE(UPacketHandlerBase, RejectPacket, 0);
	OnPacketRejected();
	const auto coreInstance = _HandlerCoreInstance.lock();
	const auto logger = coreInstance ? coreInstance->GetLogger() : nullptr;
	if (logger) {
		Core::FString message{ _HandlerName };
		message << " rejected packet type " << std::to_string(packetType)
				<< " (" << reason << "); closing the connection";
		logger->TryLog(Core::ELogLevel::Warning, message.View());
	}
	if (const auto session = _Session.lock())
		session->Close();
}

void UPacketHandlerBase::SendRawFrame(const Core::uint16 packetType, const void* data, const Core::uint64 bodySize)
{
	if (const auto session = _Session.lock())
		session->Send(data, bodySize, packetType);
}

void UPacketHandlerBase::LogOversizedOutgoingPacket(const Core::uint16 packetType, const Core::uint64 bodySize) const
{
	const auto coreInstance = _HandlerCoreInstance.lock();
	const auto logger = coreInstance ? coreInstance->GetLogger() : nullptr;
	if (!logger)
		return;
	Core::FString message{ _HandlerName };
	message << " skipped oversized packet type " << std::to_string(packetType)
			<< " (" << std::to_string(bodySize) << " bytes)";
	logger->TryLog(Core::ELogLevel::Warning, message.View());
}

void UPacketHandlerBase::LogSerializeFailure(const Core::uint16 packetType) const
{
	const auto coreInstance = _HandlerCoreInstance.lock();
	const auto logger = coreInstance ? coreInstance->GetLogger() : nullptr;
	if (!logger)
		return;
	Core::FString message{ _HandlerName };
	message << " failed to serialize packet type " << std::to_string(packetType);
	logger->TryLog(Core::ELogLevel::Warning, message.View());
}

SPACE_T
