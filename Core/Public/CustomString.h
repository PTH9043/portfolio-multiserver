#pragma once

#include <compare>
#include <concepts>
#include <cstddef>
#include <format>
#include <functional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

// CoreDefine.h 를 먼저 include 한다. (SPACE_H, Alloc::USTLAllocator 는 그쪽에서 준비된다)

SPACE_H(Core)

/*
-------------------------------
CustomString
-------------------------------
*/

// string_view 로 바꿀 수 있는 것(const char*, std::string, string_view, 풀 문자열 ...)을 받는 함수 템플릿의 조건이다.
template<class T, class CharT>
concept CustomStringLike = std::convertible_to<const T&, std::basic_string_view<CharT>>;

// 메모리 풀 할당자(USTLAllocator)를 쓰는 basic_string 을 상속한 문자열 클래스다. (GraduateProject 의 CustomString 을 이은 것)
// - std::string 의 함수를 전부 그대로 쓴다. 생성자도 물려받는다(using BASE_TYPE::BASE_TYPE).
// - 표준 할당자를 쓰는 std::string / std::wstring 과 섞어 쓸 수 있다. (생성, 대입, 비교, 연결)
//   const TChar* 가 필요하면 c_str() 을 쓴다.
template<class CharT>
class CustomString : public std::basic_string<CharT, std::char_traits<CharT>, Alloc::USTLAllocator<CharT>> {

	static_assert(std::is_same_v<CharT, char> || std::is_same_v<CharT, wchar_t>,
		"CustomString supports char and wchar_t only");

	using BASE_TYPE = std::basic_string<CharT, std::char_traits<CharT>, Alloc::USTLAllocator<CharT>>;

public:
	using STRING_VIEW_TYPE = std::basic_string_view<CharT>;
	using STD_STRING_TYPE = std::basic_string<CharT>;

	using BASE_TYPE::BASE_TYPE;
	using BASE_TYPE::operator=;

	/*
	-------------------------------
	생성 / 대입
	-------------------------------
	*/
	CustomString() = default;
	CustomString(const CustomString&) = default;
	CustomString(CustomString&&) noexcept = default;
	~CustomString() = default;
	CustomString& operator=(const CustomString&) = default;
	CustomString& operator=(CustomString&&) noexcept = default;

	// 상속한 생성자에서 기반 클래스 자신의 복사/이동 생성자는 빠지므로 직접 둔다.
	CustomString(const BASE_TYPE& text) : BASE_TYPE(text) {}
	CustomString(BASE_TYPE&& text) noexcept : BASE_TYPE(std::move(text)) {}
	// 표준 할당자를 쓰는 std::string / std::wstring 에서 복사한다. (풀 문자열과 std::string 은 서로 다른 타입이다)
	CustomString(const STD_STRING_TYPE& text) : BASE_TYPE(text.data(), text.size()) {}
	CustomString(std::nullptr_t) = delete;

	CustomString& operator=(const STD_STRING_TYPE& text) { BASE_TYPE::assign(text.data(), text.size()); return *this; }

	/*
	-------------------------------
	보기 / 덧붙이기
	-------------------------------
	*/
	// 기본 문자열의 변환 연산자로 STRINGVIEW 인자에 직접 전달할 수 있다.
	// View와 암시적 변환은 모두 원본을 소유하지 않는다.
	STRING_VIEW_TYPE View() const noexcept { return STRING_VIEW_TYPE(this->data(), this->size()); }

	// 문자열을 덧붙인다. (옛 코드의 operator<<)
	CustomString& operator<<(STRING_VIEW_TYPE text) { this->append(text); return *this; }
	CustomString& operator<<(const CharT ch) { this->push_back(ch); return *this; }
};

/*
-------------------------------
비교 / 연결 (비멤버 연산자 — friend 선언 없이 공개 인터페이스만 사용)
-------------------------------
*/
template<class CharT>
inline CustomString<CharT> ConcatCustomString(
	std::basic_string_view<CharT> left, std::basic_string_view<CharT> right)
{
	CustomString<CharT> result;
	result.reserve(left.size() + right.size());
	result.append(left.data(), left.size());
	result.append(right.data(), right.size());
	return result;
}

template<class CharT>
inline bool operator==(const CustomString<CharT>& left, const CustomString<CharT>& right) noexcept
{
	return left.View() == right.View();
}

template<class CharT>
inline auto operator<=>(const CustomString<CharT>& left, const CustomString<CharT>& right) noexcept
{
	return left.View() <=> right.View();
}

// const char*, std::string, string_view, 풀 문자열 과도 양쪽 순서로 같은 뜻으로 비교한다.
template<class CharT, class T>
	requires (!std::same_as<std::remove_cvref_t<T>, CustomString<CharT>> && CustomStringLike<T, CharT>)
inline bool operator==(const CustomString<CharT>& left, const T& right)
{
	return left.View() == std::basic_string_view<CharT>(right);
}

template<class CharT, class T>
	requires (!std::same_as<std::remove_cvref_t<T>, CustomString<CharT>> && CustomStringLike<T, CharT>)
inline auto operator<=>(const CustomString<CharT>& left, const T& right)
{
	return left.View() <=> std::basic_string_view<CharT>(right);
}

// 연결은 결과가 CustomString 이고 '\0' 이 들어 있어도 잘리지 않는다.
template<class CharT>
inline CustomString<CharT> operator+(const CustomString<CharT>& left, const CustomString<CharT>& right)
{
	return ConcatCustomString<CharT>(left.View(), right.View());
}

template<class CharT, class T>
	requires (!std::same_as<std::remove_cvref_t<T>, CustomString<CharT>> && CustomStringLike<T, CharT>)
inline CustomString<CharT> operator+(const CustomString<CharT>& left, const T& right)
{
	return ConcatCustomString<CharT>(left.View(), std::basic_string_view<CharT>(right));
}

template<class CharT, class T>
	requires (!std::same_as<std::remove_cvref_t<T>, CustomString<CharT>> && CustomStringLike<T, CharT>)
inline CustomString<CharT> operator+(const T& left, const CustomString<CharT>& right)
{
	return ConcatCustomString<CharT>(std::basic_string_view<CharT>(left), right.View());
}

template<class CharT>
inline CustomString<CharT> operator+(const CustomString<CharT>& left, const CharT right)
{
	CustomString<CharT> result{ left };
	result.push_back(right);
	return result;
}

template<class CharT>
inline CustomString<CharT> operator+(const CharT left, const CustomString<CharT>& right)
{
	CustomString<CharT> result;
	result.reserve(right.size() + 1);
	result.push_back(left);
	result.append(right);
	return result;
}

using FString = CustomString<char>;

SPACE_T

namespace std
{
	// UNORMAP<FString, ...> 처럼 키로 쓸 수 있게 한다. (string_view 로 계산하므로 새 메모리를 할당하지 않는다)
	template<class CharT>
	struct hash<Core::CustomString<CharT>>
	{
		Core::uint64 operator()(const Core::CustomString<CharT>& text) const noexcept
		{
			return static_cast<Core::uint64>(hash<basic_string_view<CharT>>{}(text.View()));
		}
	};

	// std::format("{}", text)
	template<class CharT>
	struct formatter<Core::CustomString<CharT>, CharT> : formatter<basic_string_view<CharT>, CharT>
	{
		template<class FormatContext>
		auto format(const Core::CustomString<CharT>& text, FormatContext& context) const
		{
			return formatter<basic_string_view<CharT>, CharT>::format(text.View(), context);
		}
	};
}
