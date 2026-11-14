#pragma once

#include "CoreTypes.h"
#include "CoreMacro.h"
#include <concepts>
#include <type_traits>

SPACE_H(Core)

// 같은 값은 같은 해시를 반환해야 하며, 집합에 있는 동안 해시와 비교 기준을 바꾸지 않는다.
// 동등 비교는 예외를 던지지 않아야 한다.
template<class T>
class IHashable {
public:
    using HASHABLE_TYPE = T;

    virtual uint64 GetHash() const = 0;
    virtual bool IsEqual(const T& other) const noexcept = 0;

protected:
    virtual ~IHashable() = default;
};

template<class T>
concept Hashable = requires { typename std::remove_cv_t<T>::HASHABLE_TYPE; } &&
    std::derived_from<std::remove_cv_t<T>, IHashable<typename std::remove_cv_t<T>::HASHABLE_TYPE>> &&
    std::derived_from<std::remove_cv_t<T>, typename std::remove_cv_t<T>::HASHABLE_TYPE>;

template<class T>
struct FHashableHash {
    uint64 operator()(const T& value) const {
        static_assert(Hashable<T>, "A set key must publicly inherit IHashable<T>");
        using BASE_TYPE = typename std::remove_cv_t<T>::HASHABLE_TYPE;
        return static_cast<const IHashable<BASE_TYPE>&>(value).GetHash();
    }
};

// 대상이 있는 WKPTR은 집합에서 제거할 때까지 외부에서 SHPTR을 유지해야 한다.
template<class T>
struct FHashableHash<WKPTR<T>> {
    uint64 operator()(const WKPTR<T>& value) const {
        static_assert(Hashable<T>, "A weak set key must publicly inherit IHashable<T>");
        const auto object = value.lock();
        if (!object)
            return static_cast<uint64>(std::hash<T*>{}(nullptr));
        using BASE_TYPE = typename std::remove_cv_t<T>::HASHABLE_TYPE;
        return static_cast<const IHashable<BASE_TYPE>&>(*object).GetHash();
    }
};

template<class T>
struct FHashableEqual {
    bool operator()(const T& left, const T& right) const noexcept {
        static_assert(Hashable<T>, "A set key must publicly inherit IHashable<T>");
        using BASE_TYPE = typename std::remove_cv_t<T>::HASHABLE_TYPE;
        return static_cast<const IHashable<BASE_TYPE>&>(left).IsEqual(right);
    }
};

template<class T>
struct FHashableEqual<WKPTR<T>> {
    bool operator()(const WKPTR<T>& left, const WKPTR<T>& right) const noexcept {
        static_assert(Hashable<T>, "A weak set key must publicly inherit IHashable<T>");
        const auto leftObject = left.lock();
        const auto rightObject = right.lock();
        if (!leftObject || !rightObject)
            return leftObject == rightObject;
        using BASE_TYPE = typename std::remove_cv_t<T>::HASHABLE_TYPE;
        return static_cast<const IHashable<BASE_TYPE>&>(*leftObject).IsEqual(*rightObject);
    }
};

SPACE_T
