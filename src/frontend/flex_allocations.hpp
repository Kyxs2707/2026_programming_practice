#pragma once
#include "diagnostic/failure.hpp"
#include <cstddef>
namespace myb {
/**
 * @brief 登记单个扫描器的 Flex 分配，覆盖后端部分初始化和异常清理。
 *
 * 登记链接位于分配块头部，插入和撤销登记均不再分配内存。
 * 正常 yyfree 会撤销登记；生成后端尚未接管的块由 clear() 或析构释放。
 * 借用的 EmergencyFailure 须比登记表存活更久。
 */
class FlexAllocationLedger {
    /** @brief 与用户区域同次分配的双向链接头，保持最大基础对齐。 */
    struct alignas(std::max_align_t) Block {
        Block *prev;
        Block *next;
        std::size_t size;
    };
    Block *head_{};
    inline static std::size_t live_{};
    EmergencyFailure &failure_;

  public:
    explicit FlexAllocationLedger(EmergencyFailure &f) : failure_(f) {}
    ~FlexAllocationLedger() noexcept {
        clear();
    }
    FlexAllocationLedger(const FlexAllocationLedger &) = delete;
    /**
     * @brief 分配带登记头的内存并接收其所有权。
     * @param[in] n 返回区域的字节数。
     * @return 满足 max_align_t 对齐的区域，由本表 release() 或 clear() 释放。
     * @throws HostFailure 块总尺寸超限或宿主分配失败，均归 Resource。
     */
    void *allocate(std::size_t n);
    /**
     * @brief 撤销登记并释放区域，空指针为空操作。
     * @param[in] p 本表返回且尚未释放的区域，或空指针。
     * 所有指向该区域的指针在释放后失效。
     */
    void release(void *p) noexcept;
    /**
     * @brief 先分配并登记新块，再复制有效前缀并释放原块。
     * @param[in] p 本表拥有的旧区域，或空指针。
     * @param[in] n 新区域字节数。
     * @return 拥有旧区域前 min(旧尺寸, n) 字节的新区域。
     * @throws HostFailure 新块分配失败；旧区域及其登记保持有效。
     * 成功后旧区域指针失效；空指针按 allocate(n) 处理。
     */
    void *reallocate(void *p, std::size_t n);
    /** @brief 无分配地释放所有剩余登记，所指向的区域全部失效。 */
    void clear() noexcept {
        while (head_)
            release(head_ + 1);
    }
    /** @brief 返回所有登记表的存活块总数，供清理验收读取。 */
    static std::size_t live_total() noexcept {
        return live_;
    }
};
} // namespace myb
