#include "runtime/decimal_codec.hpp"
#include "diagnostic/failure.hpp"
#include <cfenv>
#include <cmath>
#include <fast_float/fast_float.h>
#include <limits>
#include <ryu/ryu.h>
#include <system_error>

namespace myb {
namespace {
bool digit(char c) noexcept {
    return c >= '0' && c <= '9';
}
bool ascii_space(char c) noexcept {
    return c == ' ' || (c >= '\t' && c <= '\r');
}
std::string_view trim_ascii(std::string_view s) noexcept {
    while (!s.empty() && ascii_space(s.front()))
        s.remove_prefix(1);
    while (!s.empty() && ascii_space(s.back()))
        s.remove_suffix(1);
    return s;
}
/**
 * @brief 在调用数值库前完整检查 MYB 十进制浮点文法。
 * @param[in] s 已按模式处理首尾空白的借用文本，仅在本次调用内使用。
 * @param[in] mode 源码模式要求小数点或指数，转换模式另允许符号和整数形式。
 * @return 文本全部符合对应模式时为 true。
 */
bool float_grammar(std::string_view s, ParseMode mode) noexcept {
    std::size_t i = 0;
    if (mode == ParseMode::Conversion && !s.empty() && (s[0] == '+' || s[0] == '-'))
        ++i;
    const auto start = i;
    while (i < s.size() && digit(s[i]))
        ++i;
    if (i == start)
        return false;
    bool floating = false;
    if (i < s.size() && s[i] == '.') {
        floating = true;
        auto fraction = ++i;
        while (i < s.size() && digit(s[i]))
            ++i;
        if (i == fraction)
            return false;
    }
    if (i < s.size() && (s[i] == 'e' || s[i] == 'E')) {
        floating = true;
        ++i;
        if (i < s.size() && (s[i] == '+' || s[i] == '-'))
            ++i;
        auto exponent = i;
        while (i < s.size() && digit(s[i]))
            ++i;
        if (i == exponent)
            return false;
    }
    return i == s.size() && (floating || mode == ParseMode::Conversion);
}
[[noreturn]] void invalid_codec_value() {
    throw HostFailure({FailureDomain::Internal, ReasonCode::Invariant},
                      "invalid decimal codec state");
}
} // namespace

void DecimalCodec::initialize(EmergencyFailure &first) {
    static_assert(std::numeric_limits<double>::is_iec559 &&
                  std::numeric_limits<double>::digits == 53 &&
                  std::numeric_limits<double>::max_exponent == 1024);
    if (std::fesetround(FE_TONEAREST) != 0 || std::fegetround() != FE_TONEAREST)
        fail({first, {}}, FailureDomain::Internal, ReasonCode::Invariant,
             "binary64 round-to-nearest unavailable");
}

NumericParseResult<double> DecimalCodec::parse_float(std::string_view text, ParseMode mode) {
    if (mode == ParseMode::Conversion)
        text = trim_ascii(text);
    if (!float_grammar(text, mode))
        return {{}, NumericParseError::Content};
    if (text.front() == '+')
        text.remove_prefix(1);
    double result = 0;
    auto parsed = fast_float::from_chars(text.data(), text.data() + text.size(), result);
    if (parsed.ptr != text.data() + text.size())
        return {{}, NumericParseError::Content};
    // 库把下溢到零也报告为范围错误；只放行确实为正负零的此类结果。
    if ((parsed.ec == std::errc{} && std::isfinite(result)) ||
        (parsed.ec == std::errc::result_out_of_range && result == 0.0))
        return {result, NumericParseError::None};
    return {{}, NumericParseError::Range};
}

NumericParseResult<std::int64_t> DecimalCodec::parse_int(std::string_view text) {
    text = trim_ascii(text);
    bool negative = false;
    if (!text.empty() && (text.front() == '+' || text.front() == '-')) {
        negative = text.front() == '-';
        text.remove_prefix(1);
    }
    if (text.empty())
        return {{}, NumericParseError::Content};
    // 先完整检查文本内容，再累计幅值检查范围；超长数字前缀带非法尾字符时返回 Content。
    for (char c : text)
        if (!digit(c))
            return {{}, NumericParseError::Content};
    // 负数允许幅值达到 2^63；全程先用无符号数累积，避免对有符号最小值取负。
    const std::uint64_t limit =
        negative ? std::uint64_t{1} << 63 : std::uint64_t(std::numeric_limits<std::int64_t>::max());
    std::uint64_t magnitude = 0;
    for (char c : text) {
        auto n = static_cast<unsigned>(c - '0');
        if (magnitude > (limit - n) / 10)
            return {{}, NumericParseError::Range};
        magnitude = magnitude * 10 + n;
    }
    if (negative && magnitude == (std::uint64_t{1} << 63))
        return {std::numeric_limits<std::int64_t>::min(), NumericParseError::None};
    auto value = static_cast<std::int64_t>(magnitude);
    return {negative ? -value : value, NumericParseError::None};
}

std::string DecimalCodec::format_float(double value) {
    if (!std::isfinite(value))
        invalid_codec_value();
    if (value == 0.0)
        return std::signbit(value) ? "-0.0" : "0.0";
    char buffer[32];
    // 使用长度返回值读取栈缓冲，不依赖 NUL；Ryu 负责最短回读数字，后续只调整记法。
    const int size = d2s_buffered_n(value, buffer);
    if (size <= 0 || size > 32)
        invalid_codec_value();
    std::string_view raw(buffer, static_cast<std::size_t>(size));
    const bool negative = raw.front() == '-';
    if (negative)
        raw.remove_prefix(1);
    auto epos = raw.find('E');
    if (epos == std::string_view::npos)
        invalid_codec_value();
    std::string coefficient;
    int fractional = 0;
    bool after_dot = false;
    for (char c : raw.substr(0, epos)) {
        if (c == '.') {
            after_dot = true;
            continue;
        }
        if (!digit(c))
            invalid_codec_value();
        coefficient.push_back(c);
        if (after_dot)
            ++fractional;
    }
    auto exponent_text = raw.substr(epos + 1);
    bool minus_exponent = false;
    if (!exponent_text.empty() && (exponent_text.front() == '+' || exponent_text.front() == '-')) {
        minus_exponent = exponent_text.front() == '-';
        exponent_text.remove_prefix(1);
    }
    if (exponent_text.empty())
        invalid_codec_value();
    int exponent = 0;
    for (char c : exponent_text) {
        if (!digit(c) || exponent > 1000)
            invalid_codec_value();
        exponent = exponent * 10 + c - '0';
    }
    if (minus_exponent)
        exponent = -exponent;
    // 将最短数字写成 coefficient * 10^q，再独立选择 MYB 规定的定点或科学记法。
    int q = exponent - fractional;
    while (coefficient.size() > 1 && coefficient.back() == '0') {
        coefficient.pop_back();
        ++q;
    }
    if (coefficient.empty() || coefficient.front() == '0')
        invalid_codec_value();
    const int e = q + static_cast<int>(coefficient.size()) - 1;
    std::string output = negative ? "-" : "";
    if (e >= -4 && e < 16) {
        const int point = e + 1;
        if (point <= 0) {
            output += "0.";
            output.append(static_cast<std::size_t>(-point), '0');
            output += coefficient;
        } else if (static_cast<std::size_t>(point) >= coefficient.size()) {
            output += coefficient;
            output.append(static_cast<std::size_t>(point) - coefficient.size(), '0');
            output += ".0";
        } else {
            output.append(coefficient, 0, static_cast<std::size_t>(point));
            output += '.';
            output.append(coefficient, static_cast<std::size_t>(point));
        }
    } else {
        output += coefficient.front();
        if (coefficient.size() > 1) {
            output += '.';
            output.append(coefficient, 1);
        }
        output += 'e';
        if (e >= 0)
            output += '+';
        output += std::to_string(e);
    }
    return output;
}
} // namespace myb
