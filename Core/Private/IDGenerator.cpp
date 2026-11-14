#include "CoreDefine.h"
#include "IDGenerator.h"

SPACE_H(Core)

ATOMIC<SERVERID> UIDGenerator::_idIssuance{ 0 };

SERVERID UIDGenerator::ServerNextID()
{
	SERVERID id = _idIssuance.fetch_add(1, std::memory_order_seq_cst);
	return id;
}

SPACE_T
