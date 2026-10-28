#include "diagnostic/diagnostic.hpp"
#include "platform/host_io.hpp"
#include "source/source.hpp"
#include "source/utf8.hpp"
#include <charconv>
#ifdef MYB_TEST_HOOKS
#include "test_hooks.hpp"
#endif
namespace myb {
namespace {
// 诊断只保留有效的单行用户文本，避免控制字符伪造新的错误行。
std::string escaped(std::string_view text) {
    std::string out;
    for (std::size_t at = 0; at < text.size();) {
        const auto decoded = utf8::decode_one(text, at);
        const auto c = static_cast<unsigned char>(text[at]);
        const auto width = decoded ? decoded->width : std::size_t{1};
        if (c == '\n')
            out += "\\n";
        else if (c == '\r')
            out += "\\r";
        else if (c == '\t')
            out += "\\t";
        else if (!decoded || c < 32 || c == 127) {
            constexpr char hex[] = "0123456789abcdef";
            out += "\\x";
            out += hex[c >> 4];
            out += hex[c & 15];
        } else if ((decoded->value >= 0x80 && decoded->value <= 0x9f) ||
                   decoded->value == 0x2028 || decoded->value == 0x2029) {
            char digits[8];
            const auto converted = std::to_chars(digits, digits + sizeof digits, decoded->value, 16);
            out += "\\u{";
            out.append(digits, converted.ptr);
            out += '}';
        } else
            out.append(text.substr(at, width));
        at += width;
    }
    return out;
}
std::string location(const SourceManager &sources, SourceSpan span) {
    const auto &file = sources.get(span.source);
    const auto position = sources.locate(span.source, span.begin);
    return escaped(file.path_utf8) + ':' + std::to_string(position.line) + ':' +
           std::to_string(position.column);
}
} // namespace
Diagnostic make_diagnostic(const FailureHeader &header, std::string_view message,
                           const SourceManager &sources, std::span<const CallTraceEntry> trace) {
#ifdef MYB_TEST_HOOKS
    TestHooks::hit(FaultPoint::DiagnosticSnapshot);
#endif
    Diagnostic result{header, header.span ? location(sources, *header.span) + ": " : "myb: "};
    result.text += domain_name(header.domain);
    result.text += " [";
    result.text += reason_name(header.reason);
    result.text += "]: ";
    result.text += escaped(message);
    if (header.os_code)
        result.text += " (OS " + std::to_string(*header.os_code) + ')';
    result.text += '\n';
    for (const auto &entry : trace)
        result.text += "  at " + escaped(entry.name) + " (" + location(sources, entry.span) + ")\n";
    return result;
}
void report_failure(const Diagnostic &diagnostic, HostIO &io) noexcept {
    try {
        io.write_stderr(diagnostic.text);
    } catch (...) {
        // 写出可能已产生部分副作用，停止后不再输出第二条报告。
    }
}
void report_minimal_failure(HostIO &io) noexcept {
    io.write_emergency();
}
} // namespace myb
