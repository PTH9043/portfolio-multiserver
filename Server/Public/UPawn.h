#pragma once

#include "CoreDefine.h"
#include "UActor.h"
#include "Packet.h"

SPACE_H(Server)

// 플레이어·몬스터·통신 사물이 공통으로 상속하는 서버 도메인 엔티티 기반 클래스다.
// 컴포넌트 장착(USession 포함)은 Core::UActor가 맡고, 이 클래스는 공용 패킷 인프라
// (HandlePacket · SendPacket · RejectPacket · CS_PING → SC_PONG)를 한 곳에서 내려보낸다.
class UPawn abstract : public Core::UActor {
protected:
	explicit UPawn(Core::SHPTR<Core::UCoreInstance> coreInstance);
	virtual ~UPawn() override;

public:
	// 장착된 USession의 strand에서 호출된다. 공용 패킷(CS_PING)을 선처리하고 나머지는 파생 OnDomainPacket으로 위임한다.
	void OnReceivePacket(const Core::FPacketView& packet) final override;

protected:
	// 공용 패킷 외의 도메인 패킷을 파생 클래스(UPlayer, 향후 UMonster 등)가 처리한다.
	virtual void OnDomainPacket(const Core::FPacketView& packet) = 0;

	// 로그 식별용 파생 타입 이름이다.
	virtual Core::STRINGVIEW GetPawnTypeName() const noexcept { return "UPawn"; }

	// 프로토콜 위반으로 연결이 닫히기 직전 파생 클래스가 종료 사유 등을 기록할 수 있는 훅이다.
	virtual void OnPacketRejected() noexcept {}

	// 공용 Protobuf 역직렬화 진입점이다. 실패 시 Warning을 남기고 장착된 세션을 닫는다.
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

	// 프로토콜 위반 사유를 Warning으로 기록하고 장착된 세션을 즉시 닫는다.
	void RejectPacket(Core::uint16 packetType, Core::STRINGVIEW reason);

private:
	NO_COPY(UPawn)

	void SendRawFrame(Core::uint16 packetType, const void* data, Core::uint64 bodySize);
	void LogOversizedOutgoingPacket(Core::uint16 packetType, Core::uint64 bodySize) const;
	void LogSerializeFailure(Core::uint16 packetType) const;
};

SPACE_T
