#pragma once

#include <array>
#include <deque>
#include <list>
#include <map>
#include <queue>
#include <set>
#include <stack>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <concurrent_priority_queue.h>
#include <concurrent_queue.h>
#include <concurrent_unordered_map.h>
#include <concurrent_unordered_set.h>
#include <concurrent_vector.h>

// 메모리 풀 할당자(USTLAllocator)가 기본으로 들어간 STL / concurrent 별칭이다.
// CoreDefine.h 가 CoreTypes.h, CoreMacro.h 를 먼저 include 한 뒤에 이 파일을 include 한다.
#include "CustomAlloc.h"
#include "IHashable.h"

// 주의: 메모리 풀 내부(CustomMemory.h 의 중앙 큐 등)에서는 이 별칭을 쓰지 않는다.
// 풀이 자기 자신에게서 메모리를 받으면 재귀 호출이 되기 때문이다. 풀 내부는 std::/concurrency:: 를 직접 쓴다.

SPACE_H(Core)

/*
-------------------------------
STL : 순차 컨테이너
-------------------------------
*/

// std::array 는 할당자를 쓰지 않는다. 이름만 맞춘다.
template<class T, uint64 Size>
using ARRAY = std::array<T, Size>;

template<class T, class Allocator = Alloc::USTLAllocator<T>>
using VECTOR = std::vector<T, Allocator>;

template<class T, class Allocator = Alloc::USTLAllocator<T>>
using DEQUE = std::deque<T, Allocator>;

template<class T, class Allocator = Alloc::USTLAllocator<T>>
using LIST = std::list<T, Allocator>;

// 바이트 버퍼는 VECTOR<uint8> 로 정의한다.
using BUFFER = VECTOR<uint8>;

/*
-------------------------------
STL : 연관 컨테이너
-------------------------------
*/

// 맵의 할당자는 원소(pair<const K, V>)를 기준으로 만든다.
template<class K, class V, class Compare = std::less<K>,
	class Allocator = Alloc::USTLAllocator<std::pair<const K, V>>>
using MAP = std::map<K, V, Compare, Allocator>;

template<class K, class Compare = std::less<K>, class Allocator = Alloc::USTLAllocator<K>>
using SET = std::set<K, Compare, Allocator>;

template<class K, class V, class Hash = std::hash<K>, class KeyEqual = std::equal_to<K>,
	class Allocator = Alloc::USTLAllocator<std::pair<const K, V>>>
using UNORMAP = std::unordered_map<K, V, Hash, KeyEqual, Allocator>;

template<class K, class Hash = std::hash<K>, class KeyEqual = std::equal_to<K>,
	class Allocator = Alloc::USTLAllocator<K>>
using UNORSET = std::unordered_set<K, Hash, KeyEqual, Allocator>;

/*
-------------------------------
STL : 어댑터(할당자가 아니라 안쪽 컨테이너를 바꾼다)
-------------------------------
*/

template<class T, class Container = DEQUE<T>>
using QUEUE = std::queue<T, Container>;

template<class T, class Container = DEQUE<T>>
using STACK = std::stack<T, Container>;

template<class T, class Compare = std::less<T>, class Container = VECTOR<T>>
using PRIQUEUE = std::priority_queue<T, Container, Compare>;

/*
-------------------------------
concurrent (PPL) : 여러 스레드가 동시에 넣고 뺄 수 있는 컨테이너
-------------------------------
*/

template<class T, class Allocator = Alloc::USTLAllocator<T>>
using CONQUEUE = concurrency::concurrent_queue<T, Allocator>;

template<class T, class Allocator = Alloc::USTLAllocator<T>>
using CONVECTOR = concurrency::concurrent_vector<T, Allocator>;

template<class T, class Compare = std::less<T>, class Allocator = Alloc::USTLAllocator<T>>
using CONPRIQUEUE = concurrency::concurrent_priority_queue<T, Compare, Allocator>;

template<class K, class V, class Hash = std::hash<K>, class KeyEqual = std::equal_to<K>,
	class Allocator = Alloc::USTLAllocator<std::pair<const K, V>>>
using CONUNORMAP = concurrency::concurrent_unordered_map<K, V, Hash, KeyEqual, Allocator>;

// 일반 타입은 기존 표준 해시를 쓰고, 인터페이스를 상속한 타입은 공통 해시·비교를 사용한다.
template<class T>
struct FConcurrentSetTraits {
	using HASH = std::hash<T>;
	using KEY_EQUAL = std::equal_to<T>;
};

template<Hashable T>
struct FConcurrentSetTraits<T> {
	using HASH = FHashableHash<T>;
	using KEY_EQUAL = FHashableEqual<T>;
};

// WKPTR의 대상 타입은 컨테이너가 해시·비교를 인스턴스화할 때 완전한 정의가 필요하다.
template<class T>
struct FConcurrentSetTraits<WKPTR<T>> {
	using HASH = FHashableHash<WKPTR<T>>;
	using KEY_EQUAL = FHashableEqual<WKPTR<T>>;
};

template<class K, class Hash = typename FConcurrentSetTraits<K>::HASH,
	class KeyEqual = typename FConcurrentSetTraits<K>::KEY_EQUAL,
	class Allocator = Alloc::USTLAllocator<K>>
using CONUNORSET = concurrency::concurrent_unordered_set<K, Hash, KeyEqual, Allocator>;


SPACE_T
