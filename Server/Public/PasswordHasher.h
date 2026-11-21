#pragma once

#include "CoreDefine.h"

SPACE_H(Server)

// Windows CNG(bcrypt.lib) 기반 PBKDF2-HMAC-SHA256 비밀번호 해시 유틸리티다.
// 외부 암호화 라이브러리 의존성 없이 계정별 16바이트 솔트 생성과 상수시간 검증을 제공한다.
STATIC_CONSTEXPR Core::uint64 PASSWORD_SALT_BYTES{ 16 };
STATIC_CONSTEXPR Core::uint64 PASSWORD_HASH_BYTES{ 32 };
STATIC_CONSTEXPR Core::uint32 PASSWORD_PBKDF2_DEFAULT_ITERATIONS{ 60000 };

using FPasswordSalt = Core::ARRAY<Core::uint8, PASSWORD_SALT_BYTES>;
using FPasswordHash = Core::ARRAY<Core::uint8, PASSWORD_HASH_BYTES>;

// OS 암호학적 난수 생성기(BCryptGenRandom)로 16바이트 솔트를 생성한다.
bool GeneratePasswordSalt(FPasswordSalt& outSalt) noexcept;

// PBKDF2-HMAC-SHA256으로 32바이트 비밀번호 해시를 유도한다.
bool DerivePasswordHash(Core::STRINGVIEW password, const FPasswordSalt& salt,
	Core::uint32 iterations, FPasswordHash& outHash) noexcept;

// 저장된 솔트·해시와 입력 비밀번호를 상수시간(Constant-Time) 비교로 검증한다.
bool VerifyPasswordHash(Core::STRINGVIEW password, const FPasswordSalt& salt,
	const FPasswordHash& expectedHash, Core::uint32 iterations) noexcept;

SPACE_T
