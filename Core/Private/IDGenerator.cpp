#include "CoreDefine.h"
#include "IDGenerator.h"

SPACE_H(Core)

ATOMIC<SERVERID> UIDGenerator::_idIssuance{ 0 };

// 멀티스레드에서 중복되지 않는 다음 서버 ID를 원자적으로 발급한다.
SERVERID UIDGenerator::ServerNextID()
{
	SERVERID id = _idIssuance.fetch_add(1, std::memory_order_seq_cst);
	return id;
}

SPACE_T
