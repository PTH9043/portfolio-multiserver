#pragma once

#include "CoreDefine.h"
#include "UCharacter.h"

SPACE_H(Server)

// 플레이어의 공용 식별·프로필 정보 구조체다.
// 닉네임에 한정하지 않고 추후 계정 ID·등급·상태 등 공용 속성을 확장해 함께 전달·보관한다.
struct FPlayerInfo {
	Core::FString Nickname;
};

// 플레이어가 조종하는 사물이다. 통신이 필요한 액터로서 생성 시 확정되는 불변 FPlayerInfo와
// 불변 약한 세션 참조(WKPTR<Core::USession>)를 보관해 잠금 없이 안전하게 조회할 수 있다.
class UPlayer final : public Core::UCharacter {
public:
	explicit UPlayer(Core::SHPTR<Core::UCoreInstance> coreInstance, FPlayerInfo info, Core::WKPTR<Core::USession> session);
	virtual ~UPlayer() override;
	NO_COPY(UPlayer)

	// 생성 시 확정된 불변 플레이어 정보 참조다. 반환한 참조는 이 UPlayer가 살아 있는 동안 유효하다.
	const FPlayerInfo& GetInfo() const noexcept { return _Info; }

	// 연결된 세션을 돌려준다. 생성 시 확정된 불변 WKPTR이라 잠금 없이 호출할 수 있다.
	// 세션이 이미 종료·해제됐으면 빈 포인터다.
	Core::SHPTR<Core::USession> GetSession() const noexcept override;

private:
	FPlayerInfo _Info;
	Core::WKPTR<Core::USession> _Session;
};

SPACE_T
