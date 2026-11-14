#pragma once

#include "CoreDefine.h"
#include "UCoreBase.h"

SPACE_H(Core)

class CORE_DLL UJobQueue final : public UCoreBase {
public:
    UJobQueue();
    virtual ~UJobQueue() override;
    NO_COPY(UJobQueue);

private:
    QUEUE<FUNC<void()>> _Queue;
};

SPACE_T
