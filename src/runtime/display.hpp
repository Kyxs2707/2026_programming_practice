#pragma once
#include "diagnostic/failure.hpp"
#include "runtime/value.hpp"
namespace myb {
class TextBuilder;
class Heap;
/**
 * @brief 按统一显示规则把任意 MYB 值追加到 Builder，以当前路径识别循环。
 * 根字符串按原字节输出，嵌套字符串转义；浮点委托 DecimalCodec。
 * @pre 输入对象属于输出 Builder 的会话，并在整个调用期间已有根保护。
 * Builder 扩容可触发 GC，传入的 Value 副本本身不构成根。
 * 图遍历使用显式宿主栈，不调用 MYB；输入根保持整个可达图有效。
 * @throws HostFailure 内部格式化故障、容量或宿主分配失败。
 */
void display(Value, TextBuilder &, ErrorSite);
/**
 * @brief 使用同一显示器把已保活的值格式化到所属会话的 Builder。
 * @pre 输入对象与 Builder 属于 heap，整个调用期间已有根；增长可触发 GC。
 * @throws HostFailure 内部格式化故障、容量或宿主分配失败。
 */
void format_value(Heap &, Value, TextBuilder &);
} // namespace myb
