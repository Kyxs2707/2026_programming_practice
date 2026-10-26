#include "builtins/registry.hpp"
#include "cli/options.hpp"
#include "cli/session.hpp"
#include "platform/host_io.hpp"
#include <new>
#include <cstdio>
#include <optional>
#include <string>
#include <vector>

/** @brief 从 Windows 宽字符参数建立 UTF-8 命令会话，并处理会话建立前的失败。 */
int wmain(int argc, wchar_t **argv) {
    myb::EmergencyFailure first;
    std::optional<myb::WindowsHostIO> io;
    bool parsing = false;
    try {
        io.emplace(first);
        parsing = true;
        std::vector<std::string> owned;
        owned.reserve(static_cast<std::size_t>(argc > 0 ? argc - 1 : 0));
        for (int i = 1; i < argc; ++i)
            owned.push_back(myb::wide_to_utf8(argv[i], {first, {}}));
        // 等 owned 完成增长后再建立视图，保证 parse_options 借用的字符存储稳定。
        std::vector<std::string_view> args;
        args.reserve(owned.size());
        for (const auto &arg : owned)
            args.push_back(arg);
        auto options = myb::parse_options(args);
        parsing = false;
        myb::BuiltinRegistry builtins;
        myb::Session session(first, *io, builtins);
        return session.run(options);
    } catch (const std::bad_alloc &) {
        first.record_first({myb::FailureDomain::Resource, myb::ReasonCode::Allocation});
    } catch (const myb::Failure &failure) {
        if (parsing) {
            // 参数转换和命令行语法失败都归用法错误，此时尚无源码位置。
            try {
                io->write_stderr(
                    "myb: usage error: expected myb [--check] [--] <file>; use --help\n");
            } catch (...) {
            }
            return 2;
        }
        first.record_first(failure.header());
    } catch (...) {
        first.record_first({myb::FailureDomain::Internal, myb::ReasonCode::Invariant});
    }
    try {
        if (io)
            io->write_stderr("myb: host failure before session startup\n");
        else {
            // 平台对象尚未建立时仅输出固定 ASCII，避免再次分配会话服务。
            std::fputs("myb: host failure before session startup\n", stderr);
            std::fflush(stderr);
        }
    } catch (...) {
    }
    return myb::exit_code(first.header());
}
