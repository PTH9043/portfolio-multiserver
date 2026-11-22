#pragma once

#include "CoreDefine.h"
#include "ISessionHandler.h"
#include "Packet.h"

#include <string>

SPACE_H(Core)
class UCoreInstance;
class USession;
SPACE_T

SPACE_H(Server)

// 모든 서버 패킷 핸들러(UAuthHandler · UPlayer 등)가 공유하는 공용 패킷 처리 기반 클래스다.
// Protobuf 역직렬화(HandlePacket) · 직렬화 송신(SendPacket) · 프로토콜 위반 차단(RejectPacket) ·
// 공용 하트비트(CS_PING → SC_PONG)를 한 곳에서 내려보내 중복과 정책 비대칭을 없앤다.
// USession은 WKPTR로만 보관해 USession(SHPTR) -> ISessionHandler -> WKPTR<USession> 비순환 소유권을 보장한다.
class UPacketHandlerBase abstract : public Core::ISessionHandler {
protected:
	explicit UPacketHandlerBase(Core::SHPTR<Core::UCoreInstance> coreInstance,
		Core::SHPTR<Core::USession> session, Core::STRINGVIEW handlerName);
	virtual ~UPacketHandlerBase() override = default;

public:
	// 현재 연결된 USession을 잠금 없이 안전하게 조회한다(닫혔거나 소멸했으면 빈 포인터).
	Core::SHPTR<Core::USession> GetBoundSession() const noexcept { return _Session.lock(); }

	// USession의 strand에서 호출된다. 공용 패킷(CS_PING)을 선처리하고 나머지는 파생 OnDomainPacket으로 위임한다.
	void OnReceivePacket(const Core::FPacketView& packet) final override;

protected:
	// 공용 패킷 외의 도메인 패킷을 파생 핸들러(UAuthHandler · UPlayer)가 처리한다.
	virtual void OnDomainPacket(const Core::FPacketView& packet) = 0;

	// 프로토콜 위반으로 연결이 닫히기 직전 파생 핸들러가 종료 사유 등을 기록할 수 있는 훅이다.
	virtual void OnPacketRejected() noexcept {}

	// 공용 Protobuf 역직렬화 진입점이다. 실패 시 Warning을 남기고 연결을 닫는다.
	template<class TPacket, class TCallback>
	void HandlePacket(const Core::FPacketView& packetView, TCallback&& onParsed)
	{
		TPacket packet;
		if (!packet.ParseFromArray(packetView.GetData(), static_cast<int>(packetView.GetSize()))) {
			RejectPacket(packetView.GetType(), "parse failed");
			return;
		}
		onParsed(packet);
	}

	// 공용 Protobuf 직렬화 및 송신 진입점이다. 한도(1,020 B) 초과 또는 직렬화 실패 시 Warning을 남긴다.
	template<class TPacket>
	void SendPacket(const Core::uint16 packetType, const TPacket& packet)
	{
		const Core::uint64 bodySize{ static_cast<Core::uint64>(packet.ByteSizeLong()) };
		if (bodySize > Core::PACKET_MAX_BODY_SIZE) {
			LogOversizedOutgoingPacket(packetType, bodySize);
			return;
		}

		Core::ARRAY<Core::uint8, Core::PACKET_MAX_BODY_SIZE> buffer{};
		if (bodySize != 0 && !packet.SerializeToArray(buffer.data(), static_cast<int>(bodySize))) {
			LogSerializeFailure(packetType);
			return;
		}

		SendRawFrame(packetType, bodySize == 0 ? nullptr : buffer.data(), bodySize);
	}

	// 프로토콜 위반 사유를 Warning으로 기록하고 연결된 세션을 즉시 닫는다.
	void RejectPacket(Core::uint16 packetType, Core::STRINGVIEW reason);

private:
	NO_COPY(UPacketHandlerBase)

	void SendRawFrame(Core::uint16 packetType, const void* data, Core::uint64 bodySize);
	void LogOversizedOutgoingPacket(Core::uint16 packetType, Core::uint64 bodySize) const;
	void LogSerializeFailure(Core::uint16 packetType) const;

	Core::WKPTR<Core::UCoreInstance> _HandlerCoreInstance;
	Core::WKPTR<Core::USession> _Session;
	Core::STRINGVIEW _HandlerName;
};

SPACE_T
