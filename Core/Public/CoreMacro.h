#pragma once 

// Ref를 통해서 함수 내부 값을 바꿀 것이라는 표시 
#define REF_OUT

// Static Constexpr를 정의
#define STATIC_CONSTEXPR static constexpr

// 조건이 맞으면 ReturnValue를 리턴하는 매크로
#define RETURN_INVALID_COND(Cond, ReturnValue) if(Cond){ return ReturnValue; }

// 조건이 맞으면 Continue를 발생시키는 매크로
#define CONTINUE_COND(Cond) if(Cond){continue;}

// Using문을 활용한 매크로
#define MAKE_USINGTYPE(FromType, ToType) using ToType = FromType;

// ThrowValue를 던지면서 조건이 맞으면 throw를 발생시키는 매크로
#define THROW_INVALID_COND(Cond, ThrowValue) if(Cond){ throw ThrowValue;}

// Space T name Space의 시작점, Space E name Space의 끝점.
#define SPACE_H(name) namespace name {

#define SPACE_T }

// 복사 생성자, 복사 대입 연산자, 이동 생성자, 이동 대입 연산자 차단.
#define NO_COPY(className) \
	className(const className&) = delete; \
	className& operator=(const className&) = delete; \
	className(className&&) = delete; \
	className& operator=(className&&) = delete;

#define DECLARE_INSTANCE(className) \
  public: \
      static className& GetInstance(); \
      static void DeleteInstance(); \
  private: \
      NO_COPY(className) \
      static std::atomic<className*> _Instance; \
      static SHARED_MUTEX _InstanceMutex;

#define IMPLEMENT_INSTANCE(className) \
  std::atomic<className*> className::_Instance{ nullptr }; \
  SHARED_MUTEX className::_InstanceMutex{}; \
  className& className::GetInstance() \
  { \
      className* instance = _Instance.load(std::memory_order_acquire); \
      if (nullptr == instance) \
      { \
          EXCLUSIVE_LOCK_GUARD lock{ _InstanceMutex }; \
          instance = _Instance.load(std::memory_order_acquire); \
          if (nullptr == instance) \
          { \
              instance = new className{}; \
              _Instance.store(instance, std::memory_order_release); \
          } \
      } \
      return *instance; \
  } \
  void className::DeleteInstance() \
  { \
      EXCLUSIVE_LOCK_GUARD lock{ _InstanceMutex }; \
      className* instance = _Instance.exchange( \
          nullptr, std::memory_order_acq_rel); \
      delete instance; \
  }


// 메모리 풀 MakeShared를 이용하는 스마트 포인터 싱글톤
#define DECLARE_SHARED_INSTANCE(className) \
  private: \
      class FCreateKey { \
          friend class className; \
          FCreateKey() = default; \
        public: \
          FCreateKey(const FCreateKey&) = default; \
      }; \
  public: \
      explicit className(FCreateKey); \
      virtual ~className(); \
      NO_COPY(className) \
      static SHPTR<className> GetInstance(); \
      static bool DeleteInstance(); \
  private: \
      static SHPTR<className> _Instance; \
      static SHARED_MUTEX _InstanceMutex;

#define IMPLEMENT_SHARED_INSTANCE(className) \
  SHPTR<className> className::_Instance{}; \
  SHARED_MUTEX className::_InstanceMutex{}; \
  className::className(FCreateKey) {} \
  className::~className() {} \
  SHPTR<className> className::GetInstance() \
  { \
      { \
          SHARED_LOCK_GUARD lock{ _InstanceMutex }; \
          if (_Instance) \
              return _Instance; \
      } \
      EXCLUSIVE_LOCK_GUARD lock{ _InstanceMutex }; \
      if (!_Instance) \
          _Instance = MakeShared<className>(FCreateKey{}); \
      return _Instance; \
  } \
  bool className::DeleteInstance() \
  { \
      SHPTR<className> instance; \
      { \
          EXCLUSIVE_LOCK_GUARD lock{ _InstanceMutex }; \
          if (!_Instance) return true; \
          if (_Instance.use_count() != 1) return false; \
          instance.swap(_Instance); \
      } \
      return true; \
  }

// 강제로 크래쉬를 일으키는 매크로
#define CRASH(cause) 	\
{											\
	int* crash = nullptr;				\
	__analysis_assume(crash != nullptr);	\
	*crash = 0xDEADBEEF;					\
}

// 만약 cond에서 확인하려는 조건이 false이면 강제로 크래쉬를 일으키는 매크로
#define ASSERT_CRASH(COND) \
{									\
	if (!(COND))					\
	{								\
		CRASH("ASSERT_CRASH");		\
		__analysis_assume(COND);	\
	}								\
}

#define ALIGNAS(size) alignas(size)
