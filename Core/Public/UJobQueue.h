#pragma once

#include "CoreDefine.h"
#include "UCoreBase.h"

SPACE_H(Core)

// 순차 실행할 작업 콜백(FUNC)을 보관하는 기본 작업 큐다.
class CORE_DLL UJobQueue final : public UCoreBase {
public:
    UJobQueue();
    virtual ~UJobQueue() override;
    NO_COPY(UJobQueue);

private:
    QUEUE<FUNC<void()>> _Queue;
};

SPACE_T
