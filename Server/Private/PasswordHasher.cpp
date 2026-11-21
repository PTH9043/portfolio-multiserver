#include "PasswordHasher.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <bcrypt.h>

#pragma comment(lib, "bcrypt.lib")

SPACE_H(Server)

namespace {
// SHA-256 HMAC 알고리즘 핸들을 프로세스 내에서 안전하게 공유(초기화 1회)한다.
BCRYPT_ALG_HANDLE GetHmacSha256Algorithm() noexcept
{
	struct FAlgHolder {
		BCRYPT_ALG_HANDLE Handle{ nullptr };
		FAlgHolder() noexcept
		{
			if (BCryptOpenAlgorithmProvider(&Handle, BCRYPT_SHA256_ALGORITHM, nullptr, BCRYPT_ALG_HANDLE_HMAC_FLAG) != 0)
				Handle = nullptr;
		}
		~FAlgHolder() noexcept
		{
			if (Handle != nullptr)
				BCryptCloseAlgorithmProvider(Handle, 0);
		}
	};
	static FAlgHolder holder;
	return holder.Handle;
}
}

bool GeneratePasswordSalt(FPasswordSalt& outSalt) noexcept
{
	const NTSTATUS status = BCryptGenRandom(
		nullptr,
		outSalt.data(),
		static_cast<ULONG>(outSalt.size()),
		BCRYPT_USE_SYSTEM_PREFERRED_RNG);
	return status == 0;
}

bool DerivePasswordHash(const Core::STRINGVIEW password, const FPasswordSalt& salt,
	const Core::uint32 iterations, FPasswordHash& outHash) noexcept
{
	if (password.empty() || iterations == 0)
		return false;

	BCRYPT_ALG_HANDLE alg = GetHmacSha256Algorithm();
	if (alg == nullptr)
		return false;

	// BCryptDeriveKeyPBKDF2는 비-const 포인터를 받지만 입력 버퍼를 수정하지 않는다.
	const NTSTATUS status = BCryptDeriveKeyPBKDF2(
		alg,
		reinterpret_cast<PUCHAR>(const_cast<char*>(password.data())),
		static_cast<ULONG>(password.size()),
		const_cast<PUCHAR>(salt.data()),
		static_cast<ULONG>(salt.size()),
		static_cast<ULONGLONG>(iterations),
		outHash.data(),
		static_cast<ULONG>(outHash.size()),
		0);
	return status == 0;
}

bool VerifyPasswordHash(const Core::STRINGVIEW password, const FPasswordSalt& salt,
	const FPasswordHash& expectedHash, const Core::uint32 iterations) noexcept
{
	FPasswordHash computed{};
	if (!DerivePasswordHash(password, salt, iterations, computed))
		return false;

	// 타이밍 공격을 막기 위해 모든 바이트를 끝까지 XOR 누적해 비교한다.
	Core::uint8 diff{ 0 };
	for (Core::uint64 i = 0; i < PASSWORD_HASH_BYTES; ++i)
		diff |= static_cast<Core::uint8>(computed[i] ^ expectedHash[i]);
	return diff == 0;
}

SPACE_T
