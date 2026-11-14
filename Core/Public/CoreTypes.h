#pragma once

#include <chrono>
#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 6255 6258 6387 26495)
#endif
#include <boost/asio/io_context.hpp>
#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/ip/tcp.hpp>
#ifdef _MSC_VER
#pragma warning(pop)
#endif

namespace ASIO = boost::asio;

namespace Core
{
	// char 는 부호가 컴파일러 설정에 따라 달라지므로 signed char 로 고정한다.
	using int8 = signed char;
	using uint8 = unsigned char;
	using int16 = short;
	using uint16 = unsigned short;
	using int32 = int;
	using uint32 = unsigned int;
	using int64 = long long;
	using uint64 = unsigned long long;

	template<class T>
	using ATOMIC = std::atomic<T>;

	using ATOMIC_UINT64 = ATOMIC<uint64>;

	using ATOMIC_BOOL = ATOMIC<bool>;

	// 바이트 데이터는 uint8 을 쓴다. (byte 별칭은 char 라 부호가 있고 Windows 의 byte 와 이름이 겹쳐서 없앴다)
	using WCHAR = wchar_t;

	using STRINGVIEW = std::string_view;

	// 읽기는 여럿이 동시에, 쓰기는 혼자 들어가는 잠금이다. 세 개를 한 벌로 쓴다.
	// 읽기 자리에 EXCLUSIVE 를 넣어도 컴파일되므로, 어느 쪽인지 보고 쓴다.
	//
	// 쓸 자리  : 세션/방 목록처럼 읽기가 압도적이고 쓰기가 몇 ms 늦어도 되는 곳.
	// condition_variable_any와 배타 unique_lock을 조합할 수 있다.
	// 큐 넣기와 꺼내기는 쓰기이므로 공유 잠금으로 실행하지 않는다.
	// 읽는 쪽이 쉬지 않으면 쓰기가 밀릴 수 있으며 성능은 측정이 필요하다.
	// 재귀와 승급(읽기->쓰기)이 안 된다. 올려야 하면 읽기를 놓고 다시 잡은 뒤 한 번 더 확인한다.
	// MSVC의 shared_mutex는 잠금·해제 함수가 noexcept라 예외를 던지지 않는다. 잠금만 하는 구간은 try로 감싸지 않는다.
	using SHARED_MUTEX = std::shared_mutex;
	using SHARED_LOCK_GUARD = std::shared_lock<SHARED_MUTEX>;
	using EXCLUSIVE_LOCK_GUARD = std::lock_guard<SHARED_MUTEX>;

	template<class T>
	using SHPTR = std::shared_ptr<T>;

	template<class T>
	using UQPTR = std::unique_ptr<T>;

	template<class T>
	using WKPTR = std::weak_ptr<T>;

	using EXPECTION_PTR = std::exception_ptr;
	using std::terminate;
	using ERROR_CODE = std::error_code;
	using std::generic_category;
	using std::system_category;

	// std::function 을 Func 로 정의한다.
	template<class T>
	using FUNC = std::function<T>;

	// 서버 ID 는 uint64 로 정의한다.
	using SERVERID = uint64;

	// shared_mutex와 함께 대기할 수 있는 조건 변수다.
	using COND_VARIABLE = std::condition_variable_any;

	using THREAD = std::thread;
	using THREAD_ID = THREAD::id;
	using THREADS = std::vector<std::thread>;
	using THREADFUNC = FUNC<void(void*)>;
	template<class T>
	using TIMEPOINT = std::chrono::time_point<T>;

	using STEADY_CLOCK = std::chrono::steady_clock;
	using SYSTEM_CLOCK = std::chrono::system_clock;
	using TIMEPOINT_STEADY_CLOCK = TIMEPOINT<STEADY_CLOCK>;
	using TIMEPOINT_SYSTEM_CLOCK = TIMEPOINT<SYSTEM_CLOCK>;

	using IOCONTEXT = ASIO::io_context;
	using TCPACCEPTOR = ASIO::ip::tcp::acceptor;
	using TCPSOCKET = ASIO::ip::tcp::socket;
	using TCP_ENDPOINT = ASIO::ip::tcp::endpoint;
	template<class T>
	using WORK_GUARD = ASIO::executor_work_guard<T>;

	using MILLISECONDS = std::chrono::milliseconds;
	using SECONDS = std::chrono::seconds;
	
	using BOOST_STEADY_TIMER = ASIO::steady_timer;
	using BOOST_ERROR_CODE = boost::system::error_code;
}
