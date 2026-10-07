#include <catch2/catch_test_macros.hpp>

#include <limits>

#include "Utils.h"

using ExampleMod::ClampOrDefault;
using ExampleMod::TrimWhitespace;

TEST_CASE("FormatVersion produces correct string", "[utils]")
{
	CHECK(ExampleMod::FormatVersion(1, 2, 3) == "1.2.3");
	CHECK(ExampleMod::FormatVersion(0, 0, 1) == "0.0.1");
	CHECK(ExampleMod::FormatVersion(10, 0, 0) == "10.0.0");
}

TEST_CASE("AsciiToLower lowercases A-Z only", "[utils]")
{
	using ExampleMod::AsciiToLower;

	CHECK(AsciiToLower('A') == 'a');
	CHECK(AsciiToLower('Z') == 'z');
	CHECK(AsciiToLower('M') == 'm');
	CHECK(AsciiToLower('a') == 'a');
	CHECK(AsciiToLower('z') == 'z');
	CHECK(AsciiToLower('0') == '0');
	CHECK(AsciiToLower('!') == '!');
	CHECK(AsciiToLower(static_cast<unsigned char>(0xFF)) == static_cast<unsigned char>(0xFF));
}

TEST_CASE("CaseInsensitiveEqual matches ASCII case-insensitively", "[utils]")
{
	using ExampleMod::CaseInsensitiveEqual;

	CHECK(CaseInsensitiveEqual("None", "none"));
	CHECK(CaseInsensitiveEqual("NONE", "None"));
	CHECK(CaseInsensitiveEqual("MCM", "mcm"));
	CHECK(CaseInsensitiveEqual("Map", "MAP"));
	CHECK(CaseInsensitiveEqual("", ""));
	CHECK_FALSE(CaseInsensitiveEqual("Map", "Mcm"));
	CHECK_FALSE(CaseInsensitiveEqual("None", ""));
	CHECK_FALSE(CaseInsensitiveEqual("abc", "abcd"));
}

TEST_CASE("TrimWhitespace removes leading and trailing whitespace", "[utils]")
{
	CHECK(TrimWhitespace("  hello  ") == "hello");
	CHECK(TrimWhitespace("\t hello\t") == "hello");
	CHECK(TrimWhitespace("hello") == "hello");
	CHECK(TrimWhitespace("  ") == "");
	CHECK(TrimWhitespace("") == "");
	CHECK(TrimWhitespace("  a b c  ") == "a b c");
	CHECK(TrimWhitespace("hello\r\n") == "hello");
	CHECK(TrimWhitespace("\r\n  hello  \r\n") == "hello");
}

TEST_CASE("ClampOrDefault clamps and validates values", "[utils]")
{
	constexpr float kDefault = 0.5F;
	constexpr float kMin = 0.1F;
	constexpr float kMax = 5.0F;

	// In-range values pass through unchanged
	CHECK(ClampOrDefault(1.0F, kDefault, kMin, kMax) == 1.0F);
	CHECK(ClampOrDefault(2.5F, kDefault, kMin, kMax) == 2.5F);
	CHECK(ClampOrDefault(5.0F, kDefault, kMin, kMax) == 5.0F);
	CHECK(ClampOrDefault(0.1F, kDefault, kMin, kMax) == 0.1F);

	// Below minimum (or non-finite) falls back to the default
	CHECK(ClampOrDefault(0.05F, kDefault, kMin, kMax) == kDefault);
	CHECK(ClampOrDefault(-1.0F, kDefault, kMin, kMax) == kDefault);
	CHECK(ClampOrDefault(0.0F, kDefault, kMin, kMax) == kDefault);

	// Above maximum clamps to max
	CHECK(ClampOrDefault(5.1F, kDefault, kMin, kMax) == kMax);
	CHECK(ClampOrDefault(100.0F, kDefault, kMin, kMax) == kMax);

	// Non-finite values fall back to the default
	CHECK(ClampOrDefault(std::numeric_limits<float>::quiet_NaN(), kDefault, kMin, kMax) == kDefault);
	CHECK(ClampOrDefault(std::numeric_limits<float>::infinity(), kDefault, kMin, kMax) == kDefault);
	CHECK(ClampOrDefault(-std::numeric_limits<float>::infinity(), kDefault, kMin, kMax) == kDefault);
}
