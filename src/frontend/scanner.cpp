#include "frontend/scanner.hpp"
#include <cstring>
#include <limits>
int yylex_init_extra(myb::ScannerContext *, yyscan_t *);
int yylex_destroy(yyscan_t);
yy_buffer_state *yy_scan_buffer(char *, std::size_t, yyscan_t);
myb::ScannerContext *yyget_extra(yyscan_t);
namespace myb {
Scanner::Scanner(SourceManager &sources, SourceId id) : context_(id, sources.failure()) {
    const auto &bytes = sources.get(id).bytes;
    // Flex 2.6.4 接口接收 size_t，内部仍将去掉两个哨兵后的长度存为 int。
    if (bytes.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()) ||
        bytes.size() > input_.max_size() - 2)
        fail({context_.failure, {}}, FailureDomain::Resource, ReasonCode::Capacity,
             "scanner input exceeds generated backend capacity");
    input_.reserve(bytes.size() + 2);
    input_.insert(input_.end(), bytes.begin(), bytes.end());
    input_.push_back(0);
    input_.push_back(0);
    try {
        if (yylex_init_extra(&context_, &handle_) != 0)
            fail({context_.failure, {}}, FailureDomain::Resource, ReasonCode::Allocation,
                 "scanner initialization failed");
        buffer_ = yy_scan_buffer(input_.data(), input_.size(), handle_);
        if (!buffer_)
            fail({context_.failure, {}}, FailureDomain::Internal, ReasonCode::Invariant,
                 "scanner rejected input buffer");
    } catch (...) {
        // 构造失败时析构函数不会执行，先销毁已发布状态，再清扫未接管的块。
        if (handle_) {
            yylex_destroy(handle_);
            handle_ = nullptr;
        }
        context_.allocations.clear();
        throw;
    }
}
Scanner::~Scanner() noexcept {
    if (handle_)
        yylex_destroy(handle_);
    context_.allocations.clear();
}
Parser::symbol_type Scanner::next() {
    return raw_lex(handle_);
}
void ScannerContext::escape(std::string_view text) {
    if (text[1] != 'u') {
        char c = text[1];
        switch (c) {
        case 'n':
            c = '\n';
            break;
        case 'r':
            c = '\r';
            break;
        case 't':
            c = '\t';
            break;
        default:
            break;
        }
        decoded_string.push_back(c);
        return;
    }
    std::uint32_t cp = 0;
    // 词法规则已确认转义形状；跳过 \u{ 前缀和末尾右花括号，只累计十六进制幅值。
    for (std::size_t i = 3; i + 1 < text.size(); ++i) {
        auto c = text[i];
        cp = (cp << 4) | static_cast<unsigned>(c <= '9'   ? c - '0'
                                               : c <= 'F' ? c - 'A' + 10
                                                          : c - 'a' + 10);
    }
    if (cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff))
        lexical_error("Unicode escape is not a scalar value");
    // 先排除非法标量，再按码点上界选择最短 UTF-8 编码宽度。
    if (cp < 0x80)
        decoded_string.push_back(static_cast<char>(cp));
    else if (cp < 0x800) {
        decoded_string.push_back(static_cast<char>(0xc0 | (cp >> 6)));
        decoded_string.push_back(static_cast<char>(0x80 | (cp & 63)));
    } else if (cp < 0x10000) {
        decoded_string.push_back(static_cast<char>(0xe0 | (cp >> 12)));
        decoded_string.push_back(static_cast<char>(0x80 | ((cp >> 6) & 63)));
        decoded_string.push_back(static_cast<char>(0x80 | (cp & 63)));
    } else {
        decoded_string.push_back(static_cast<char>(0xf0 | (cp >> 18)));
        decoded_string.push_back(static_cast<char>(0x80 | ((cp >> 12) & 63)));
        decoded_string.push_back(static_cast<char>(0x80 | ((cp >> 6) & 63)));
        decoded_string.push_back(static_cast<char>(0x80 | (cp & 63)));
    }
}
[[noreturn]] void flex_fatal(const char *text, yyscan_t scanner) {
    auto *c = yyget_extra(scanner);
    // 适配 Flex 后端的错误文本；容量关键词优先，其次分配，其余归内部状态错误。
    bool allocation = std::strstr(text, "memory") || std::strstr(text, "alloc");
    bool capacity = std::strstr(text, "overflow") || std::strstr(text, "capacity");
    fail({c->failure, {}},
         allocation || capacity ? FailureDomain::Resource : FailureDomain::Internal,
         capacity     ? ReasonCode::Capacity
         : allocation ? ReasonCode::Allocation
                      : ReasonCode::Invariant,
         text);
}
} // namespace myb
/**
 * @brief 将 Flex 分配交给当前扫描器的登记表，供异常清理追踪。
 * @pre scanner 的 yyextra 指向仍存活的 ScannerContext。
 * @return 由对应登记表拥有的 n 字节区域。
 */
void *yyalloc(std::size_t n, yyscan_t scanner) {
    return yyget_extra(scanner)->allocations.allocate(n);
}
/**
 * @brief 通过当前扫描器登记表调整区域大小，失败时保留旧块。
 * @pre p 为空或由 scanner 对应的登记表拥有。
 * @return 新区域；成功后原区域失效。
 */
void *yyrealloc(void *p, std::size_t n, yyscan_t scanner) {
    return yyget_extra(scanner)->allocations.reallocate(p, n);
}
/**
 * @brief 释放 Flex 区域并撤销当前扫描器中的登记。
 * @pre p 为空或由 scanner 对应的登记表拥有且尚未释放。
 */
void yyfree(void *p, yyscan_t scanner) {
    yyget_extra(scanner)->allocations.release(p);
}
