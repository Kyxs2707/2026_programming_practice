#include "source/source.hpp"
#include "source/utf8.hpp"
#include <algorithm>
#include <limits>

namespace myb {
SourceId SourceManager::add(std::string path, std::string bytes) {
    if (files_.size() >= std::numeric_limits<SourceId>::max() || files_.size() == files_.max_size())
        fail({failure_, {}}, FailureDomain::Resource, ReasonCode::Capacity,
             "source count exceeds capacity");
    const auto id = static_cast<SourceId>(files_.size());
    files_.push_back({std::move(path), std::move(bytes), {}, 0});
    auto &source = files_.back();
    source.bom_size = source.bytes.starts_with("\xEF\xBB\xBF") ? 3 : 0;
    auto add_line = [&](std::size_t offset) {
        if (source.line_starts.size() == source.line_starts.max_size())
            fail({failure_, {}}, FailureDomain::Resource, ReasonCode::Capacity,
                 "source line index exceeds capacity");
        source.line_starts.push_back(offset);
    };
    add_line(0);
    for (std::size_t offset = 0; offset < source.bytes.size();) {
        const auto decoded = utf8::decode_one(source.bytes, offset);
        if (!decoded)
            fail({failure_, SourceSpan{id, offset, offset + 1}}, FailureDomain::Frontend,
                 ReasonCode::Encoding, "source contains invalid UTF-8");
        if (decoded->value == '\r') {
            ++offset;
            if (offset < source.bytes.size() && source.bytes[offset] == '\n')
                ++offset;
            add_line(offset);
        } else {
            offset += decoded->width;
            if (decoded->value == '\n')
                add_line(offset);
        }
    }
    return id;
}
const SourceFile &SourceManager::get(SourceId id) const {
    if (id >= files_.size())
        fail({failure_, {}}, FailureDomain::Internal, ReasonCode::Invariant,
             "invalid source identity");
    return files_[id];
}
SourcePosition SourceManager::locate(SourceId id, std::uint64_t offset) const {
    const auto &source = get(id);
    if (offset > source.bytes.size())
        fail({failure_, {}}, FailureDomain::Internal, ReasonCode::Invariant,
             "source offset out of bounds");
    const auto upper = std::upper_bound(source.line_starts.begin(), source.line_starts.end(),
                                        static_cast<std::size_t>(offset));
    const auto line = static_cast<std::size_t>(upper - source.line_starts.begin() - 1);
    auto current = line == 0 ? source.bom_size : source.line_starts[line];
    std::uint64_t column = 1;
    // 编码失败后保留的源码也用于诊断；只累计目标偏移之前完整合法的码点。
    // 遇非法序列或偏移落在码点内部时保留当前列，供首错定位使用。
    while (current < offset) {
        const auto decoded = utf8::decode_one(source.bytes, current);
        if (!decoded || current + decoded->width > offset)
            break;
        // CRLF 内部偏移仍归前一行行尾，避免把同一次换行计入列数。
        if (decoded->value == '\r' || decoded->value == '\n')
            break;
        ++column;
        current += decoded->width;
    }
    return {line + 1, column};
}
std::string_view SourceManager::slice(SourceSpan span) const {
    const auto &source = get(span.source);
    if (span.begin > span.end || span.end > source.bytes.size())
        fail({failure_, {}}, FailureDomain::Internal, ReasonCode::Invariant,
             "invalid source range");
    return std::string_view(source.bytes)
        .substr(static_cast<std::size_t>(span.begin),
                static_cast<std::size_t>(span.end - span.begin));
}
SourceSpan SourceManager::merge(SourceSpan first, SourceSpan last) const {
    slice(first);
    slice(last);
    if (first.source != last.source || first.begin > last.end)
        fail({failure_, {}}, FailureDomain::Internal, ReasonCode::Invariant,
             "cannot merge source ranges");
    return {first.source, first.begin, last.end};
}
} // namespace myb
