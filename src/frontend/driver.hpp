#pragma once
#include "ast/factory.hpp"
#include "source/source.hpp"
#include <memory>
#include <string>
namespace myb {
/** @brief 拥有整数的十进制幅值词素，保留 Int64 最小值的直接负号判定。 */
struct IntLexeme {
    std::string digits;
};
/** @brief 拥有尚未转换为 Float64 的源码浮点词素。 */
struct FloatLexeme {
    std::string text;
};
class Scanner;
/**
 * @brief 协调一次完整扫描解析，独占扫描器和待交付的 AST。
 *
 * SourceManager 及其 EmergencyFailure 由调用方保活，须覆盖 Driver 的寿命。
 * factory() 和 failure() 返回借用引用；源码编号按值返回。
 * 扫描器字符缓冲不会进入交付的树，树自行拥有名称、词素和字符串。
 */
class Driver {
  public:
    /**
     * @brief 为已登记的源码建立解析驱动。
     * @pre SourceId 对应 sources 中已完成 UTF-8 验证的源码。
     */
    Driver(SourceManager &, SourceId);
    ~Driver() noexcept;
    /**
     * @brief 从源码起点解析完整文件，成功后转移整棵树的所有权。
     * @return 语言文法生成的完整程序，空文件也返回有效程序节点。
     * @throws FrontendFailure 发生词法或语法错误；在首错处终止解析。
     * @throws HostFailure 扫描后端容量、分配或内部约束失败。
     * @throws std::bad_alloc 宿主容器或 AST 分配失败。
     * 异常传播前释放扫描器及待交付程序；调用方不会收到部分树。
     */
    std::unique_ptr<AstProgram> parse();
    /**
     * @brief 按需建立并借用本次解析的扫描器。
     * @return 由 Driver 独占的扫描器，引用在下一次 parse() 或 Driver 析构时失效。
     */
    Scanner &scanner();
    AstFactory &factory() noexcept {
        return factory_;
    }
    SourceId source_id() const noexcept {
        return source_;
    }
    EmergencyFailure &failure() const noexcept {
        return sources_.failure();
    }
    /**
     * @brief 接收起始产生式归约成功后的程序所有权，等待 parse() 交付。
     * @param[in] program 由当前驱动的工厂构造的完整程序。
     */
    void accept(std::unique_ptr<AstProgram> program) {
        program_ = std::move(program);
    }

  private:
    SourceManager &sources_;
    SourceId source_;
    AstFactory factory_;
    std::unique_ptr<Scanner> scanner_;
    std::unique_ptr<AstProgram> program_;
};
} // namespace myb
