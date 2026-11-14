#pragma once

#include <algorithm>
#include <compare>
#include <concepts>
#include <cstddef>
#include <filesystem>
#include <format>
#include <functional>
#include <ostream>
#include <stdexcept>
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
	using size_type = typename BASE_TYPE::size_type;
	using STRING_VIEW_TYPE = std::basic_string_view<CharT>;
	using STD_STRING_TYPE = std::basic_string<CharT>;
	using RAW_TYPE = BASE_TYPE;

	using BASE_TYPE::BASE_TYPE;
	using BASE_TYPE::operator=;
	using BASE_TYPE::replace;	// 아래에서 다시 선언한 것 외의 replace(반복자 버전 등)도 그대로 쓴다.

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
	CustomString(const std::filesystem::path& path) : BASE_TYPE(FromPath(path)) {}
	CustomString(std::nullptr_t) = delete;

	CustomString& operator=(const STD_STRING_TYPE& text) { BASE_TYPE::assign(text.data(), text.size()); return *this; }
	CustomString& operator=(const std::filesystem::path& path) { BASE_TYPE::operator=(FromPath(path)); return *this; }

	/*
	-------------------------------
	변환
	-------------------------------
	*/
	// 기본 문자열의 변환 연산자로 STRINGVIEW 인자에 직접 전달할 수 있다.
	// View와 암시적 변환은 모두 원본을 소유하지 않는다.
	STRING_VIEW_TYPE View() const noexcept { return STRING_VIEW_TYPE(this->data(), this->size()); }

	// 표준 할당자 std::string 으로 복사한다. (외부 라이브러리에 std::string 을 넘길 때)
	STD_STRING_TYPE ToStd() const { return STD_STRING_TYPE(this->data(), this->size()); }

	// char 문자열은 UTF-8, wchar_t 문자열은 UTF-16(Windows)으로 본다.
	std::filesystem::path ToPath() const
	{
		if constexpr (std::is_same_v<CharT, wchar_t>)
		{
			return std::filesystem::path(std::wstring_view(this->data(), this->size()));
		}
		else
		{
			return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(this->data()), this->size()));
		}
	}
	operator std::filesystem::path() const { return ToPath(); }

	// char <-> wchar_t 변환. char 문자열이면 wchar_t 문자열을, wchar_t 문자열이면 UTF-8 char 문자열을 돌려준다.
	auto convertstr() const
	{
		if constexpr (std::is_same_v<CharT, char>)
		{
			const std::u8string utf8(reinterpret_cast<const char8_t*>(this->data()), this->size());
			const std::wstring wide{ std::filesystem::path(utf8).wstring() };
			return CustomString<wchar_t>(wide);
		}
		else
		{
			const std::u8string utf8{ std::filesystem::path(std::wstring_view(this->data(), this->size())).u8string() };
			return CustomString<char>(reinterpret_cast<const char*>(utf8.data()), utf8.size());
		}
	}

	// 숫자를 문자열로 바꾼다. (옛 코드의 std::_Integral_to_string 은 MSVC 내부 함수라 std::to_string 으로 바꿨다)
	template<class T> requires std::is_arithmetic_v<T>
	static CustomString to_string(const T value)
	{
		if constexpr (std::is_same_v<CharT, char>) { return CustomString(std::to_string(value)); }
		else { return CustomString(std::to_wstring(value)); }
	}

	// substr 이 풀 문자열(기반 클래스)이 아니라 CustomString 을 돌려준다.
	CustomString substr(const size_type pos = 0, const size_type count = BASE_TYPE::npos) const
	{
		return CustomString(BASE_TYPE::substr(pos, count));
	}

	// 문자열을 덧붙인다. (옛 코드의 operator<<)
	CustomString& operator<<(STRING_VIEW_TYPE text) { this->append(text); return *this; }
	CustomString& operator<<(const CharT ch) { this->push_back(ch); return *this; }

	/*
	-------------------------------
	replace : 자기 버퍼의 일부로 교체해도 안전하게
	-------------------------------
	*/
	// MSVC(14.51)의 std::string::replace 는 소스가 자기 버퍼의 일부이고 SSO 범위 안에서 길이가 늘어나면 죽는다. (실측: Debug/Release 모두)
	// 그래서 소스를 받는 replace 는 자기 버퍼를 가리키면 먼저 복사해 둔다. 반복자 버전은 기반 클래스의 것을 그대로 쓴다.
	CustomString& replace(const size_type pos, const size_type count, const CharT* pText)
	{
		return ReplaceImpl(pos, count, pText, BASE_TYPE::traits_type::length(pText));
	}
	CustomString& replace(const size_type pos, const size_type count, const CharT* pText, const size_type length)
	{
		return ReplaceImpl(pos, count, pText, length);
	}
	CustomString& replace(const size_type pos, const size_type count, const BASE_TYPE& text)
	{
		return ReplaceImpl(pos, count, text.data(), text.size());
	}
	CustomString& replace(const size_type pos, const size_type count, const CustomString& text)
	{
		return ReplaceImpl(pos, count, text.data(), text.size());
	}
	CustomString& replace(const size_type pos, const size_type count, const BASE_TYPE& text, const size_type textPos, const size_type textCount = BASE_TYPE::npos)
	{
		return ReplaceImpl(pos, count, text.data(), SubLength(text.size(), textPos, textCount), textPos);
	}
	CustomString& replace(const size_type pos, const size_type count, const CustomString& text, const size_type textPos, const size_type textCount = BASE_TYPE::npos)
	{
		return ReplaceImpl(pos, count, text.data(), SubLength(text.size(), textPos, textCount), textPos);
	}
	CustomString& replace(const size_type pos, const size_type count, STRING_VIEW_TYPE text)
	{
		return ReplaceImpl(pos, count, text.data(), text.size());
	}
	CustomString& replace(const size_type pos, const size_type count, STRING_VIEW_TYPE text, const size_type textPos, const size_type textCount = BASE_TYPE::npos)
	{
		return ReplaceImpl(pos, count, text.data(), SubLength(text.size(), textPos, textCount), textPos);
	}

	/*
	-------------------------------
	비교 / 연결 (옛 코드의 버그를 고친 것)
	-------------------------------
	*/
	// 같은 타입끼리는 문자열 전체가 같은가를 본다. (옛 코드는 왼쪽이 const char* 이면 "포함하는가"를 봤다)
	friend bool operator==(const CustomString& left, const CustomString& right) noexcept { return left.View() == right.View(); }
	friend auto operator<=>(const CustomString& left, const CustomString& right) noexcept { return left.View() <=> right.View(); }

	// const char*, std::string, string_view, 풀 문자열 과도 양쪽 순서로 같은 뜻으로 비교한다.
	template<class T> requires (!std::same_as<std::remove_cvref_t<T>, CustomString> && CustomStringLike<T, CharT>)
	friend bool operator==(const CustomString& left, const T& right) { return left.View() == STRING_VIEW_TYPE(right); }
	template<class T> requires (!std::same_as<std::remove_cvref_t<T>, CustomString> && CustomStringLike<T, CharT>)
	friend auto operator<=>(const CustomString& left, const T& right) { return left.View() <=> STRING_VIEW_TYPE(right); }

	// 연결은 결과가 CustomString 이고 '\0' 이 들어 있어도 잘리지 않는다.
	friend CustomString operator+(const CustomString& left, const CustomString& right) { return Concat(left.View(), right.View()); }
	template<class T> requires (!std::same_as<std::remove_cvref_t<T>, CustomString> && CustomStringLike<T, CharT>)
	friend CustomString operator+(const CustomString& left, const T& right) { return Concat(left.View(), STRING_VIEW_TYPE(right)); }
	template<class T> requires (!std::same_as<std::remove_cvref_t<T>, CustomString> && CustomStringLike<T, CharT>)
	friend CustomString operator+(const T& left, const CustomString& right) { return Concat(STRING_VIEW_TYPE(left), right.View()); }
	friend CustomString operator+(const CustomString& left, const CharT right)
	{
		CustomString result{ left };
		result.push_back(right);
		return result;
	}
	friend CustomString operator+(const CharT left, const CustomString& right)
	{
		CustomString result;
		result.reserve(right.size() + 1);
		result.push_back(left);
		result.append(right);
		return result;
	}

private:
	static BASE_TYPE FromPath(const std::filesystem::path& path)
	{
		if constexpr (std::is_same_v<CharT, wchar_t>)
		{
			const std::wstring wide{ path.wstring() };
			return BASE_TYPE(wide.data(), wide.size());
		}
		else
		{
			const std::u8string utf8{ path.u8string() };
			return BASE_TYPE(reinterpret_cast<const char*>(utf8.data()), utf8.size());
		}
	}

	static CustomString Concat(STRING_VIEW_TYPE left, STRING_VIEW_TYPE right)
	{
		CustomString result;
		result.reserve(left.size() + right.size());
		result.append(left.data(), left.size());
		result.append(right.data(), right.size());
		return result;
	}

	// text[textPos, textPos + textCount) 의 길이. textPos 가 범위를 벗어나면 std::string 처럼 out_of_range 를 던진다.
	static size_type SubLength(const size_type textSize, const size_type textPos, const size_type textCount)
	{
		if (textPos > textSize) { throw std::out_of_range("invalid string position"); }
		return (std::min)(textCount, textSize - textPos);
	}

	// pText 가 이 문자열 자신의 버퍼 안을 가리키는가
	bool IsInsideOwnBuffer(const CharT* pText, const size_type length) const noexcept
	{
		if (0 == length) { return false; }
		const CharT* pBegin{ this->data() };
		const CharT* pEnd{ pBegin + this->size() };
		return std::less_equal<const CharT*>{}(pBegin, pText) && std::less<const CharT*>{}(pText, pEnd);
	}

	CustomString& ReplaceImpl(const size_type pos, const size_type count, const CharT* pText, const size_type length, const size_type skip = 0)
	{
		const CharT* pSource{ pText + skip };
		if (true == IsInsideOwnBuffer(pSource, length))
		{
			const BASE_TYPE copy(pSource, length);
			BASE_TYPE::replace(pos, count, copy);
		}
		else
		{
			BASE_TYPE::replace(pos, count, pSource, length);
		}
		return *this;
	}
};

using FString = CustomString<char>;
using FWString = CustomString<wchar_t>;

SPACE_T

namespace std
{
	// UNORMAP<STRING, ...> 처럼 키로 쓸 수 있게 한다. (string_view 로 계산하므로 새 메모리를 할당하지 않는다)
	template<class CharT>
	struct hash<Core::CustomString<CharT>>
	{
		size_t operator()(const Core::CustomString<CharT>& text) const noexcept
		{
			return hash<basic_string_view<CharT>>{}(text.View());
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
