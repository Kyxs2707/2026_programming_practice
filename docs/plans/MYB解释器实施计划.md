# MYB 解释器实施计划（Implementation Plan）

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking. 执行方式由用户在审阅本计划后选择，不因本文件创建而启动实现。

**Goal:** 实现 Windows 首版 MYB 解释器、21 个已确认内置及一键黑盒测试，并接入 GitHub PR 自动构建、回归与合并检查，使完整语言规范有可重复执行的验证证据。

**Architecture:** Flex/Bison 构建独占所有权 AST，完成全文件结构检查后，由独立模块生成固定字段栈式字节码。VM 使用统一 Value 数组和显式帧；Heap 通过精确根进行非移动标记—清除，平台 I/O 通过 HostIO 隔离。

**Tech Stack:** C++20、UCRT64 GCC 15.2.0、Flex 2.6.4、Bison 3.8.2、CMake 3.24+、Ninja 1.11+、Python 3.10+；fast_float v8.3.2、Ryu `59661c3` 的 C99 依赖闭包。

**Spec:** [MYB 解释器设计](../specs/MYB解释器设计.md)；语言行为以 [MYB 语言规范](../specs/MYB语言规范.md) v0.2（含 2026-10-22 D63 澄清）为准；完整课程交付顺序见 [项目实施步骤](../superpowers/plans/2026-10-11-myb-implementation.md)。

状态：用户于 2026-10-22 明确要求“好，你先完成M1吧”，已授权任务 01–06。**M1 已于 2026-10-23 完成实现与验收；用户同日授权完成 M2，任务 07–11 已通过本地 Debug/Release 验收与独立审查，见 [M2 测试报告](../reports/M2测试报告.md)。M3–M4 未启动。** 2026-10-23 按用户要求在 M1 与 M2 之间新增 CI-01/CI-02 两项工程任务，用户已授权实施；CI-01 的本地脚本、远端两 job 正常→失败→恢复和独立仅文档 PR 触发均已验收；用户随后将仓库改为 public 并授权完成 CI-02，强制合并条件已配置并完成失败/恢复验收；整个 CI 工程阶段完成。 独立构建、源码前端与堆模块并行实施，由根代理集成并安排一次独立整体审查。实测命令、范围与调整见 [构建与测试](../guides/构建与测试.md) 和 [测试报告](../reports/测试报告_v0.1.md)。

## Global Constraints（全局约束）

- 所有改动位于 dev 或从 dev 创建的派生分支；开始执行时再建立隔离分支/工作区，默认分支名为 `codex/myb-interpreter`；main 只能 PR 合并。
- Windows 首版；核心与平台分离。核心/生成代码为 C++20，Ryu 独立 C99；不更换已确认的工具链。
- 固定版本源码先核验完整提交号、逐文件 SHA-256、文件清单、许可与契约；configure 不联网下载、不安装工具。版本未核实时保留待核验状态，不改用 master。
- 用户直接执行源文件；必须完成全文件解析、结构检查、全部字节码生成/核验后才运行。未声明、未初始化、动态类型、调用数量和构造字段集合等保持既定运行时检查时序。
- 变量/参数动态改绑，数组混合类型；对象 Value 复制共享，普通切片浅拷贝，deep_copy 保留图中共享/循环；字段按声明类型/可空规则检查。
- Flex reentrant C 后端按 C++20 编译，Bison `lalr1.cc`＋variant＋完整 symbol；只读 Visitor、unique_ptr 树形所有权；AST 高度最多 256，超限受控资源错误 3。
- SourceSpan 使用 SourceId:uint32、原 UTF-8 字节半开范围:uint64；1 基码点行列；LF/CRLF/单 CR，CRLF 一次；BOM 不占列，Tab 一列。U+2028/U+2029 不作源码换行。
- 指令为 Op:uint16＋a/b:uint32，单元内编号独立；初始化状态为 Dormant/Uninitialized/Initialized。VM 值区按需至少 64 个 Value，帧/遍历按需至少 16 项；不足时取 required 与旧容量加一半的较大值。
- 从首次受管分配开始接入根和可执行 GC；输出先由稳定根接管。GC 初始/最小 1 MiB、存活容量倍数 2、不重入；bad_alloc 不自动重试。标记失败不 sweep；不追踪预留空位。
- 用户函数由 VM 循环推进；目标/数量预检先于实参，全部实参从左到右；子帧独立参数区，准备成功才进入。native 输出根 → 清旧参数/收 sp → 写结果并增 sp → 撤根，期间不分配/GC。
- CLI 为文件入口、--check、--help、--version；支持 -- 终止选项，无 REPL/脚本 argv/stdin 源码。退出 0 成功、1 MYB 错误、2 用法/入口读取、3 受控宿主资源/内部故障。
- 控制台宽字符桥接，文件/管道 UTF-8 字节；平台不解释 Value/根。Q02 仅控制台单 Ctrl+Z 整行作为 EOF；read_line/split_lines 的单 CR 文本规则保持不变。
- 仅已有 21 个内置；不引入模块、闭包、用户异常捕获、游戏终端 API、自适应指令、磁盘字节码、C++ 第三方测试框架或 CTest。
- Debug/Release 分目录；生成物和 build 不入 Git；Release 禁用 fast-math，使用 `-ffp-contract=off`。不设未确认的总内存、MYB 递归深度或执行时限。
- 黑盒使用 MYB 脚本、独立进程/工作目录、字节级输出和文件检查；Python 驱动默认超时 5 秒、单例可增加；不自动更新 golden、不归一化换行或替换坏编码。

## Review Focus（容易遗漏的五类输入）

1. 文件后部/未执行路径的结构错误也阻止先前输出；直接负 MIN 与括号内超范围正数不能混淆。归任务 07/08。
2. 调用预检失败不求实参；实参或帧准备失败不把尚未进入的函数放进调用链。归任务 06/11。
3. GC 发生在 native、编译常量增长或半成品发布时，对象仍存活；工作表内存失败不能清除漏标对象。归任务 04/08/14/17。
4. 合法首行后预读到坏 UTF-8、管道正常关闭、Unicode 路径和部分写入/关闭失败均按真实边界处理。归任务 02/16。
5. VM 第一个向量已增长、第二个增长失败或诊断再次缺内存时，旧状态可清理、首错保留、遍历计数归零。归任务 10/11/13/17。

## 文件职责与统一接口

所有下列路径相对仓库根；任务 01–06 的 M1 文件已实现，其余文件按所属任务逐步建立。每任务列出的测试源需加入任务 01 的内部目标，blackbox 分组同时滚动聚合进默认 manifest：直接汇总cases并按默认manifest目录换算fixture路径，不增加include/import清单语法。中间版本只验收已实现子集，不把未实现分支的临时行为当作最终 MYB 规则。

每项红测试附最小代码锚点和精确预期；片段中的first、span、ops、unit等是该测试所需的最小测试数据或测试辅助对象，按本任务接口完成构造，不新增产品辅助API。中文C++源按UTF-8编译，测试期望按显式字节核对，不能依赖编辑器本机编码。

| 文件/组 | 单一职责与初次落地任务 |
|---|---|
| CMakeLists.txt、cmake/FindMybTools.cmake、third_party/manifest.json | 工具发现、固定版本源码与目标/生成依赖；01/05 |
| src/source/span.hpp、utf8.hpp/.cpp、source.hpp/.cpp | 编码、原字节与位置；02 |
| src/diagnostic/failure.hpp/.cpp、diagnostic.hpp/.cpp | 无分配首错、异常分类；02；完整报告17 |
| src/platform/utf8_sink.hpp、host_io.hpp、windows_io.cpp | 纯字节/文本/路径 I/O；02；完整读写16 |
| src/runtime/decimal_codec.hpp/.cpp | 严格数字文本和 D72 浮点排版；03 |
| src/runtime/value.hpp、schema.hpp、object.hpp、roots.hpp/.cpp、heap.hpp/.cpp、text_builder.hpp/.cpp | 值、对象、根、受控分配/增长；04 |
| src/ast/node.hpp、nodes.hpp/.cpp、visitor.hpp、factory.hpp | 全部节点、只读分派、构造高度；05 |
| src/frontend/lexer.l、parser.y、scanner.hpp/.cpp、flex_allocations.hpp、driver.hpp/.cpp | 扫描/解析与清理；05；完整文法07 |
| src/semantic/checked_ast.hpp、checker.hpp/.cpp | 声明索引、字面量侧表和结构检查；05/07 |
| src/bytecode/instruction.hpp、program.hpp/.cpp、compiler.hpp/.cpp、binding_layout.hpp/.cpp、verify.hpp/.cpp | 编译产物、生成、修补与容量核验；06/08 |
| src/vm/vm.hpp/.cpp、frame.hpp、value_store.hpp/.cpp、globals.hpp/.cpp、call.hpp/.cpp、iteration.hpp/.cpp | 调度/执行状态；06/10/11/13 |
| src/runtime/operators.hpp、value_ops.hpp/.cpp、sequence_ops.hpp/.cpp、graph_copy.hpp/.cpp、display.hpp/.cpp | 共享操作符枚举、动态语义、图与显示；06/09/12/14 |
| src/builtins/registry.hpp/.cpp、array_builtins.cpp、scalar_builtins.cpp、text_builtins.cpp、io_builtins.cpp | 描述表与分组回调；06/12/14/15/16 |
| src/main.cpp、src/cli/options.hpp/.cpp、session.hpp/.cpp | Unicode 入口、选项、寿命与阶段调度；06 |
| tests/internal/test_support.hpp、main.cpp、test_hooks.hpp/.cpp、fixtures.hpp/.cpp | 最小注册/断言、内部故障接缝、MemoryHostIO/会话辅助；01/06 |
| tools/run_blackbox.py、tests/driver/test_run_blackbox.py、tests/blackbox/manifest.json | 字节级外部驱动与其自身测试；01 |
| .github/workflows/ci.yml、tools/ci.ps1、docs/CI与合并规则.md | PR/push 自动构建、回归与合并检查；CI-01/CI-02 |
| docs/构建与测试.md、需求覆盖表.md、测试报告.md、README.md | 可复现证据与入口；01/18 |

统一命名，后续任务不另建同义接口：

- `FailureHeader{domain, reason, optional<SourceSpan> span, optional<uint32_t> os_code}`；异常提供 `header() const noexcept`。`EmergencyFailure::record_first(FailureHeader) noexcept`、`is_set() const noexcept`、`header() const noexcept` 在构造详细诊断前保存首错；`ErrorSite{EmergencyFailure&, optional<SourceSpan>}` 供语义辅助函数使用。
- `NumericParseResult<T>{optional<T> value, NumericParseError error}`，error=None/Content/Range；`ParseMode{SourceLiteral,Conversion}`。`DecimalCodec::initialize(EmergencyFailure&)` 设置并核验数值环境；`parse_float(string_view,ParseMode)`、`parse_int(string_view)` 返回相应结果，`format_float(double)->string`。正常解析失败不登记首错。
- `RootedValue` 不可移动，`get()->const Value&`、`set(Value) noexcept`；`Heap(RootRegistry&,EmergencyFailure&)` 的 make_array/make_struct/make_string 写入 `RootedValue&`；NativeRootSet 枚举集合而非登记元素地址。
- `Driver(SourceManager&,SourceId)::parse()->unique_ptr<AstProgram>`；`Checker::check(const AstProgram&)->CheckedAst`；`BytecodeCompiler::compile(const CheckedAst&,CompiledProgram&)`；`VM::execute(const CompiledProgram&)`。编译器/VM 构造时借用同一 Session 的 Heap、根、首错和内置表。
- `NativeFn=void(*)(NativeContext&,span<const Value>,RootedValue&)`；NativeContext 借用 Heap、RootRegistry、HostIO、EmergencyFailure 和当前 SourceSpan。`BuiltinDescriptor{name:string_view,arity:uint8,invoke:NativeFn}`；表在执行期间不可变。
- `Utf8Sink::append(span<const char>)`；HostIO 提供 `read_entry(string_view)->EntryBytes{path_utf8,bytes}`、`read_line(Utf8Sink&)->LineStatus{Line,End}`、`read_text(string_view,Utf8Sink&)`、`write_text(string_view,string_view)`、`write_stdout(string_view,bool append_lf)`、`write_stderr(string_view)`，不接收 Value。WindowsHostIO 为正式后端，MemoryHostIO 只在测试中模拟同一边界。
- `VmFixture::compile(string_view)->const CompiledProgram&` 与 `run(string_view)->TestRun`；TestRun 拥有 exit_code、stdout_bytes、stderr_bytes、可选 FailureHeader、调用名列表；fixture 内部管理 AST/Program/Heap 寿命，不与产品公开 API 混用。
- 内部程序支持 `--case <name>`，无参数运行所有已登记 case。使用最小 `MYB_TEST(name)`/`REQUIRE(condition)`，断言失败退出非零且 Release 不消失；不引入测试框架。TestHooks 仅测试构建可用：`force_gc(bool)`、`fail_next(FaultPoint)`、`clear_fault()`、`heap_counts(const Heap&)->HeapCounts{live,marked,collecting,active_arrays}`、`active_iteration_count(const VM&)->size_t`，后一个方法在13接入。
- FaultPoint 按真实失败位置区分 HeapObject、HeapRegistry、Payload、MarkWorklist、ProgramConstants、NativeWorkspace、VmValues、VmStates、VmFrames、IterationRecords、FlexAllocation、DiagnosticSnapshot、OsRead、OsWrite、OsClose；各模块在所属任务接入，绝不暴露为 MYB 内置/CLI/环境选项。

## 里程碑与执行命令

| 里程碑 | 任务 | 可验收结果 |
|---|---|---|
| M1 最小链路 | 01–06 | print(1+2) 得到 3；前端/入口/运行失败分开；外部驱动可判成败 |
| CI 工程阶段 | CI-01、CI-02 | Windows PR 自动构建/回归成功；失败的必需检查实际阻止合并 |
| M2 前端与函数 | 07–11 | 完整文法/检查/字节码，变量/短路/while、递归与受保护调用 |
| M3 完整运行时 | 12–16 | 数组/结构/for、深复制与显示、21 内置、Windows 文本 I/O |
| M4 验收 | 17–18 | 故障/资源/诊断、全部黑盒、干净/并行/Release 构建与复现文档 |

任务按依赖推进，成功用例和故障协议均需验收。M1 的独立模块并行实施，最终统一提交一个可构建的里程碑，避免中间 CMake 引用尚未提交的文件；后续任务仍按任务组织提交。安装/定位工具已在任务 01 完成；缺工具不是语义红测试证据。运行命令前使用 UCRT64 环境或配置当前 PowerShell 进程 PATH，项目路径不硬编码到共享 CMake。

下列命令为构建复现示例。CMake 将可执行文件输出到对应构建目录根，以下全使用 Windows PowerShell：

```powershell
cmake -S . -B build/debug -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_COMPILER=D:/msys64/ucrt64/bin/g++.exe -DCMAKE_C_COMPILER=D:/msys64/ucrt64/bin/gcc.exe -DFLEX_EXECUTABLE=D:/msys64/usr/bin/flex.exe -DBISON_EXECUTABLE=D:/msys64/usr/bin/bison.exe -DMYB_BUILD_INTERNAL_TESTS=ON
cmake --build build/debug --parallel
& .\build\debug\myb_internal_tests.exe --case source_positions
python tools/run_blackbox.py --interpreter build/debug/myb.exe --manifest tests/blackbox/smoke/manifest.json
```

以下每项的“构建并运行”均先重建相关目标，再运行明确命令。红阶段记录新增断言/缺少实现的真实失败；通过后跑本组与已有默认回归，只有新改动/新风险才增加重复测试。第 01 项前不存在任何构建入口。

## M1：最小执行链

### 任务 01：可复现工具链、依赖与测试入口

**Files:** Create `CMakeLists.txt`、`cmake/FindMybTools.cmake`、`.gitignore`、`third_party/manifest.json`、`src/runtime/toolchain_contract.cpp`、`tests/internal/test_support.hpp`、`tests/internal/main.cpp`、`tests/internal/test_hooks.hpp`、`tests/internal/test_hooks.cpp`、`tests/internal/toolchain_cases.cpp`、`tools/run_blackbox.py`、`tests/driver/test_run_blackbox.py`、`tests/driver/child_fixture.py`、`docs/构建与测试.md`；导入已核验的固定版本数值依赖源码及原许可。

**Interfaces:** Produces myb_core、myb_decimal_ryu、可选 myb_internal_tests 目标及上述测试入口；驱动 CLI/manifest 严格采用设计格式，返回 0 全通过/1 失败或超时/2 参数或 manifest 错误。此时尚无 myb 执行目标，不伪造源文件检查成功。

- [x] **1. 核验未来执行环境。** 读取 GCC/Flex/Bison/CMake/Ninja/Python 版本，定位或补齐缺少的原生工具；核验 fast_float v8.3.2 和 Ryu 59661c3 的完整提交号、逐文件 SHA-256 和许可，并在 manifest 记录。固定版本源码无法满足契约时记录证据、修订对应依赖条目，不放宽 D72。
- [x] **2. 写红测试。** toolchain case 要求 `__cplusplus==202002L`、double 为 binary64、span 可用；原始依赖把 `1.25` 解析为 1.25、Ryu 输出可提取同值。Python unittest 用独立 child_fixture 固定输出 `b"a\r\nb\n"`、stderr 和退出 7，证明正确预期通过、错误预期/超时失败、下一例仍运行、`../` 副作用路径被拒绝。

```cpp
MYB_TEST(toolchain) {
    REQUIRE(__cplusplus == 202002L);
    REQUIRE(std::numeric_limits<double>::is_iec559);
    REQUIRE(std::numeric_limits<double>::digits == 53);
}
```
- [x] **3. 确认红。** 在工具可用后尝试配置/目标编译及 `python -m unittest discover -s tests/driver -v`，记录实现缺失导致的失败，明确不是安装环境缺失。
- [x] **4. 实现。** 建立目标、最小断言/故障接缝和驱动。生成物/build 忽略，产物按 Debug/Release 隔离；驱动二进制捕获、隔离 cwd、严格 UTF-8 regex 与 `$SCRIPT` 转义，不 shell 启动。
- [x] **5. 验证绿。** `cmake --build build/debug --target myb_internal_tests --parallel`；`& .\build\debug\myb_internal_tests.exe --case toolchain`；重跑上述 unittest，均返回 0。实际坏 golden/死循环由自测 fixture 检出，不把故意失败例纳入正式全绿集。
- [x] **6. 提交。** 按本计划开头的并行实施调整，随 M1 可构建里程碑统一提交源码、固定依赖、测试和文档；不纳入 build、工具安装或生成源码。

### 任务 02：源码位置、基础首错和宿主入口字节通路

**Files:** Create `src/source/span.hpp`、`src/source/utf8.hpp`、`src/source/utf8.cpp`、`src/source/source.hpp`、`src/source/source.cpp`、`src/diagnostic/failure.hpp`、`src/diagnostic/failure.cpp`、`src/platform/utf8_sink.hpp`、`src/platform/host_io.hpp`、`src/platform/windows_io.cpp`、`tests/internal/source_cases.cpp`、`tests/internal/failure_cases.cpp`；Modify CMake 源清单。

**Interfaces:** Produces `SourceManager(EmergencyFailure&)` 的 add/get/locate/slice、FailureHeader/EmergencyFailure/三类异常 `.header()`、Utf8Sink、HostIO；本项实现 read_entry、标准流识别/宽字符输出，逐行/程序文件接口在16实现。`utf8::decode_one(string_view,size_t) noexcept->optional<DecodedCodePoint{value,width}>` 不自行决定错误阶段；SourceManager 赋予源码归属。

- [x] **1. 写红测试。** source_positions 对 BOM＋`中\t😀\r\nx\ry\n`（17字节）断言 offset7=1:3、13=2:1、15=3:1、17=4:1，slice[3,11)为8字节；`ok\n\xFF` 错误偏移3。failure_first 先登记 Runtime/Type，再模拟 bad_alloc，首错类别保持 1。

```cpp
EmergencyFailure first;
SourceManager sources(first);
auto id = sources.add("p.myb", "\xEF\xBB\xBF" "中\t😀\r\nx\ry\n");
REQUIRE(sources.get(id).bytes.size() == 17);
REQUIRE(sources.locate(id, 7).column == 3);
REQUIRE(sources.locate(id, 17).line == 4);
```
- [x] **2. 确认红。** 重建并运行 `--case source_positions`、`--case failure_first`，预期缺少接口/位置或首错断言失败；宿主入口读文件测试使用临时中文/含空格路径，不运行 MYB。
- [x] **3. 实现。** 原字节/路径独立持有；先保存记录、严格验证再建立行索引；BOM/CRLF/CR/Tab/EOF 按全局约束。所有容量和偏移检查在增长前。失败先记录固定头再构造详情；read_entry 保留原字节、宽路径、真实错误码，未使用的无效标准流不提前失败。
- [x] **4. 验证绿。** 重跑两 case，增加 `--case entry_io`，全为0；空文件 EOF=1:1、非法编码不替换继续、无位置不伪造、越界/跨源 span 为 Internal、U+2028 不增行；输出逐字节含 NUL，入口 I/O 故障可由调用者映射2。
- [x] **5. 提交。** 按本计划开头的并行实施调整，随 M1 可构建里程碑统一提交源码、固定依赖、测试和文档；不纳入 build、工具安装或生成源码。

### 任务 03：严格十进制解析与 D72 浮点排版

**Files:** Create `src/runtime/decimal_codec.hpp`、`src/runtime/decimal_codec.cpp`、`tests/internal/decimal_cases.cpp`；Modify CMake 源清单。

**Interfaces:** Consumes 01 固定版本源码、02 宿主故障；Produces统一 NumericParseResult/ParseMode、DecimalCodec 三方法。方法只处理标量/宿主文本，不创建受管对象，不把正常 Content/Range 失败登记为首错。

- [x] **1. 写红测试。** decimal_codec 要求源码 `.5/1./1e/NaN` Content，转换 `" +01e2 "` 为100.0、`"-1e-9999"` 为 signbit=true 的0、`"1e9999"` Range；int `"-9223372036854775808"` 为MIN、正幅值2^63为Range。format 精确断言 `1.0`、`1000.0`、`1e+16`、`0.0001`、`1e-5`、`-0.0`。

```cpp
EmergencyFailure first;
DecimalCodec::initialize(first);
auto p = DecimalCodec::parse_float(
    "1.00000000000000011102230246251565404236316680908203125",
    ParseMode::Conversion);
REQUIRE(p.value && *p.value == 1.0); // 精确二进制中点，取偶
REQUIRE(DecimalCodec::format_float(-0.0) == "-0.0");
```
- [x] **2. 确认红。** 重建并运行 `& .\build\debug\myb_internal_tests.exe --case decimal_codec`，记录解析/排版未实现的失败。
- [x] **3. 实现。** 预校验 MYB 文法、转换仅裁 ASCII 空白/处理正号；fast_float 必须完整消费，只接受成功有限值或范围错误且结果为带符号零。Ryu _n 用32字节缓冲和返回长度，提取并规范化 c×10^q 后按最终 e 排版；设置/验证 FE_TONEAREST，不用 locale/默认 ostream。
- [x] **4. 验证绿。** 同 case 为0；独立位模式断言最大有限/最小次正规/中点取偶、负零、2^53 两侧与指数阈值邻居；固定随机种子补充回读位相同，独立 golden 验证 shortest/nearest/even，不能只做回读。try 解析失败不污染 EmergencyFailure，宿主故障不转 Content/Range。
- [x] **5. 提交。** 按本计划开头的并行实施调整，随 M1 可构建里程碑统一提交源码、固定依赖、测试和文档；不纳入 build、工具安装或生成源码。

### 任务 04：从首次分配接入 Value、根、Heap 与受控增长

**Files:** Create `src/runtime/value.hpp`、`src/runtime/schema.hpp`、`src/runtime/object.hpp`、`src/runtime/roots.hpp`、`src/runtime/roots.cpp`、`src/runtime/heap.hpp`、`src/runtime/heap.cpp`、`src/runtime/text_builder.hpp`、`src/runtime/text_builder.cpp`、`tests/internal/heap_cases.cpp`；Modify CMake 源清单、TestHooks。

**Interfaces:** Consumes 02 的首错/Utf8Sink/UTF-8；Produces七类 Value、独立 BindingState、稳定 RootRegistry/RootedValue/NativeRootSet、Marker::mark(Value)、Heap::collect()/clear()、make_array(size_t,out)、make_struct(const StructSchema&,out)、make_string(TextBuilder&&,out)、reserve_array(ArrayRef,size_t)。StructSchema持有id/name和有序FieldSchema，FieldType保存TypeKind、可选SchemaId及nullable；FieldConstraint为FieldType别名，语义检查与运行时共用类型编号。TextBuilder(Heap&) 实现 Utf8Sink 的 append、reserve、bytes，不可复制；make_string接管缓冲时转账且不重复释放，不要求移动根提供者。

- [x] **1. 写红测试。** heap_roots 建自含数组/共享别名，仅一个根时 collect 后live=1，撤完根后live=0；可增长 NativeRootSet 模拟未启动 VM 的常量集合，内部 vector 扩容后仍保活。heap_growth 在逐允许点 GC 下验证数组/子对象身份不变、`北邮😀\0` 字节和码点计数、旧输出根失败时仍保护。

```cpp
EmergencyFailure first;
RootRegistry roots;
Heap heap(roots, first);
{ RootedValue a(roots); heap.make_array(0, a); heap.collect();
  REQUIRE(TestHooks::heap_counts(heap).live == 1); }
heap.collect();
REQUIRE(TestHooks::heap_counts(heap).live == 0);
```
- [x] **2. 确认红。** 重建并运行 `--case heap_roots`、`--case heap_growth`，期望根/分配/追踪或记账断言失败，而非可执行文件不存在。
- [x] **3. 实现。** Schema/字段按设计稳定、具名类型身份明确；Heap vector<unique_ptr> 拥有不移动对象。根侵入式无分配注册、集合回调枚举；迭代 mark，完整成功才 sweep。构造→登记→输出根发布段不 GC，析构不跟随子边；Builder/数组容量实际记账、1MiB/倍2与大请求阈值，bad_alloc 不自动重试。
- [x] **4. 验证绿。** 两 case 为0；Object/Registry/Payload 故障无悬空登记/重复记账，MarkWorklist 故障不 sweep、marked=0、collecting=false、live不变；clear 后live=0。Schema 比Heap对象活得久；单值根不登记容器槽地址，Builder在Heap前销毁。
- [x] **5. 提交。** 按本计划开头的并行实施调整，随 M1 可构建里程碑统一提交源码、固定依赖、测试和文档；不纳入 build、工具安装或生成源码。

### 任务 05：AST、Flex/Bison 与最小检查链

**Files:** Create `src/ast/node.hpp`、`src/ast/nodes.hpp`、`src/ast/nodes.cpp`、`src/ast/visitor.hpp`、`src/ast/factory.hpp`、`src/frontend/lexer.l`、`src/frontend/parser.y`、`src/frontend/scanner.hpp`、`src/frontend/scanner.cpp`、`src/frontend/flex_allocations.hpp`、`src/frontend/driver.hpp`、`src/frontend/driver.cpp`、`src/semantic/checked_ast.hpp`、`src/semantic/checker.hpp`、`src/semantic/checker.cpp`、`tests/internal/frontend_cases.cpp`；Modify CMake 生成依赖/源清单。

**Interfaces:** Consumes 02/03/04；Produces全部设计节点/纯虚只读 AstVisitor、`AstFactory(EmergencyFailure&)`、`AstFactory::make<T>(SourceSpan,Args&&...)->unique_ptr<T>`、Node::height()/accept()、Driver/Checker/CheckedAst。Create `src/runtime/operators.hpp` 共用UnaryOp{Plus,Minus,Not}、BinaryOp{Add,Subtract,Multiply,Divide,FloorDivide,Modulo}、CompareOp{Equal,NotEqual,Less,LessEqual,Greater,GreaterEqual}，不让 AST/VM 各定义一套；LiteralTable 保存宿主字面量，不含受管 Value。先实现表达式/命名调用/简单语句的最小可执行语法，完整文法在07扩展。

- [x] **1. 写红测试。** ast_contract 验证叶高1、Group链256后再接父抛 Resource/Capacity、程序根也计层、经基类删除完整、accept只访问当前节点；遗漏任何具体visit的Visitor仍abstract。frontend_smoke 对 `print(1 + 2);` 断言一顶层ExprStmt→Call(print)→Binary(Add)，并检查各token真实字节范围；非法字符不输出ECHO，空文件返回空根。

```cpp
static_assert(std::has_virtual_destructor_v<Node>);
static_assert(std::is_abstract_v<AstVisitor>);
AstFactory factory(first);
std::unique_ptr<Expr> e = factory.make<IntExpr>(span, std::string("1"));
for (int i = 1; i < 256; ++i) e = factory.make<GroupExpr>(span, std::move(e));
REQUIRE(e->height() == 256);
```
- [x] **2. 确认红。** 构建内部目标并运行 `--case ast_contract`、`--case frontend_smoke`，记录所有权/扫描解析/侧表未实现导致的失败。
- [x] **3. 实现。** 节点含虚 noexcept 析构、Group/省略端点/原字段源序与深只读访问；按 max(child.height)+1 在连接前限高。前端采用已定完整symbol接口、yyextra、可写副本双NUL、消费字节定位、%initial-action/YYLLOC_DEFAULT；EOF显式symbol，语法/词法抛1、fatal资源/内部抛3，分配登记兜底部分初始化。CMake 明确 generated/parser.hpp 包含依赖。核验 Flex 2.6.4 生成后端仍使用 int 保存缓冲长度，复制前拒绝超过 INT_MAX 字节的源码，按 Resource/Capacity 退出 3，避免 size_t 到 int 窄化。
- [x] **4. 验证绿。** 两case为0；正常→失败→正常连续解析相互独立，注入FlexAllocation失败后活跃扫描分配归零；内部NUL不截断、开头BOM/CRLF/CR与完整非法码点位置正确。Checker至少建立整数/浮点字面量侧表并保留直接负MIN例外，不执行动态运算。
- [x] **5. 提交。** 按本计划开头的并行实施调整，随 M1 可构建里程碑统一提交源码、固定依赖、测试和文档；不纳入 build、工具安装或生成源码。

### 任务 06：标量字节码、CLI 与第一组 MYB 黑盒

**Files:** Create `src/bytecode/instruction.hpp`、`src/bytecode/program.hpp`、`src/bytecode/program.cpp`、`src/bytecode/compiler.hpp`、`src/bytecode/compiler.cpp`、`src/bytecode/verify.hpp`、`src/bytecode/verify.cpp`、`src/vm/frame.hpp`、`src/vm/vm.hpp`、`src/vm/vm.cpp`、`src/runtime/value_ops.hpp`、`src/runtime/value_ops.cpp`、`src/runtime/display.hpp`、`src/runtime/display.cpp`、`src/builtins/registry.hpp`、`src/builtins/registry.cpp`、`src/builtins/io_builtins.cpp`、`src/main.cpp`、`src/cli/options.hpp`、`src/cli/options.cpp`、`src/cli/session.hpp`、`src/cli/session.cpp`、`tests/internal/fixtures.hpp`、`tests/internal/fixtures.cpp`、`tests/internal/smoke_cases.cpp`、`tests/blackbox/smoke/manifest.json` 和该组 cases/expected；Modify CMake 增加 myb 目标、默认manifest。

**Interfaces:** Consumes 01–05；Produces固定Instruction/CompiledProgram/CodeUnit、compile/execute、NativeFn/NativeContext、Session寿命和 VmFixture/TestRun。`parse_options(span<const string_view>)->Options{mode,file}` 只解析已确认 CLI；wmain 先严格转换宽参数。此阶段生成/执行 CONST、BINARY(Add/Div)、PRECALL/CALL、DROP、HALT，native提供print/write及标量显示；其余能力在所属任务实现，不伪造检查成功或悄悄跳过节点。

- [x] **1. 写红测试。** smoke/three.myb=`print(1 + 2);`，stdout=`b"3\n"`、stderr空、exit0；`print(1/0);` 为Arithmetic/1；缺少源文件为2且无虚构行列；`print(1 + );` 前端1。`--check` 对正常脚本为0且两流空、不读取stdin。`print(write("X"),1);` 在数量预检处1，stdout空。

```myb
print(1 + 2);
```
- [x] **2. 确认红。** 按全局Debug配置构建，运行 `python tools/run_blackbox.py --interpreter build/debug/myb.exe --manifest tests/blackbox/smoke/manifest.json`，新MYB用例应因缺少执行链而失败；把配置/链接问题与语义红测试分开记录。
- [x] **3. 实现。** Session 先调用DecimalCodec::initialize，再严格组织 Source→Driver→Checker→compile/verify→VM，--check只到生成结束；Program地址稳定，常量和VM/全局提供者先登记，统一输出根接管。加入 myb 入口 -municode、帮助/版本、--、错误分类和基本报告；运行时仍做命名目标/数量预检。print/write在实参全求完后回调，结果不裸交接。
- [x] **4. 验证绿。** 同黑盒命令和 `--case smoke_pipeline` 均为0；内部MemoryHostIO计数确认--check的stdin读取/内置回调次数为0，另以非法UTF-8 stdin的检查用例证明不执行read_line。--help/--version为0、版本精确 `myb 0.1.0 (language 0.2)\n`；多文件/未知选项/无文件为2；中文和带空格源路径可运行、--后连字符路径可读、不强制.myb后缀。Resource注入经统一边界为3，不冒充null。
- [x] **5. 提交。** 按本计划开头的并行实施调整，随 M1 可构建里程碑统一提交源码、固定依赖、测试和文档；不纳入 build、工具安装或生成源码。

## CI：M1 后、M2 前的自动构建与合并检查

2026-10-23 用户要求将 CI 加入计划。新增 CI-01、CI-02 两项工程任务，保持原任务 01–18 编号不变；合计 18 项解释器任务＋2 项 CI 任务。本节已获用户授权实施，CI-01 本地与远端红绿回归、普通代码/独立仅文档 PR 触发及 dev push 均已验收；用户随后将仓库改为 public 并完成 CI-02：远端规则已生效，失败时阻止合并、恢复后满足必需状态检查条件的验证通过；整个 CI 阶段完成。证据见 [CI 与合并规则](../rules/CI与合并规则.md)。CI-01 自动执行构建/测试，CI-02 负责平台强制合并条件，两者分别验收。

### 任务 CI-01：GitHub Actions Windows 构建与自动回归

**依赖与位置：**任务 01–06 已验收之后、任务 07 之前。后续 M2–M4 每次新增正式测试都必须同步默认测试入口，使同一 CI 持续覆盖增长中的解释器。

**Files:** Create `.github/workflows/ci.yml`、`tools/ci.ps1`、`docs/CI与合并规则.md`；Modify `tools/run_blackbox.py`、`tests/driver/test_run_blackbox.py`、`docs/构建与测试.md`、`README.md`。本任务执行时才创建/修改这些实现文件。

**Interfaces:** 工作流名 `MYB CI`，两个固定 job ID 与显示名均为 `windows-debug`、`windows-release`；不用动态矩阵后缀改变必需检查名。触发 `pull_request`（目标 `dev`、`main`）、`push`（仅 `dev`）和 `workflow_dispatch`；派生分支通过面向 dev/main 的 PR 验证，不因分支前缀遗漏检查。首版不设置 paths/paths-ignore、跳过文档变更或条件跳过整个必需 job；不启用自动合并、发布、部署或 Linux 矩阵。

采用 GitHub 托管 `windows-2025` runner，通过 `msys2/setup-msys2` 配置 `UCRT64`，每 job 设置 `timeout-minutes: 30`。安装 `mingw-w64-ucrt-x86_64-gcc`、`mingw-w64-ucrt-x86_64-cmake`、`mingw-w64-ucrt-x86_64-ninja`、`mingw-w64-ucrt-x86_64-python` 以及 MSYS 的 `flex`、`bison`；记录实际版本，满足现有 CMake/语言版本要求。runner 标签固定并不固定其镜像或滚动软件包版本，日志须保留实际环境。checkout、MSYS2 setup、日志上传 action 在实施时核验并固定完整提交 SHA，登记来源版本；首版不缓存 build 或生成物。Windows runner 的用途与 MSYS2 UCRT64 接口见 [runner 官方清单](https://github.com/actions/runner-images)、[MSYS2 CI 文档](https://www.msys2.org/docs/ci/)和 [setup-msys2](https://github.com/msys2/setup-msys2)。

`tools/ci.ps1 -Configuration <Debug|Release> -Msys2Root <实际安装根目录>` 从脚本位置确定项目根，解析原生 CMake/Ninja、UCRT64 gcc/g++/Python 和 usr/bin 下 Flex/Bison；为整个构建/测试进程设置以 UCRT64 bin 优先、usr/bin 随后的 PATH，使运行库和生成器子工具可发现，不硬编码本机 D:/msys64。配置时显式传入同一 Python 的 `Python3_EXECUTABLE`。脚本成功退出 0，工具缺失、配置/构建/测试失败、零用例或超时均退出非零，且保留首个失败退出码；PowerShell 每条原生命令后立即检查 `$LASTEXITCODE`，不能只依赖 ErrorActionPreference，也不能让后续成功覆盖前次失败。

| job | 全新检出后的配置 | 必须执行的验证 |
|---|---|---|
| windows-debug | build/debug，Debug，MYB_BUILD_INTERNAL_TESTS=ON | 全内部测试、driver unittest、指向 Debug 的 CLI unittest、默认 MYB 黑盒 |
| windows-release | build/release，Release，MYB_BUILD_INTERNAL_TESTS=OFF | 指向 Release 的 CLI unittest、默认 MYB 黑盒；确认无测试钩子/测试目录编译依赖 |

两 job 独立检出、生成并构建，不能借用另一个 job 或本机的产物。configure 离线核验仓库内固定版本数值依赖源码的逐文件 SHA-256；工具安装联网与 configure 不联网分开处理。仅权限 `contents: read`，不使用 pull_request_target 来运行 PR 代码；失败后仍上传已有日志，不能以 continue-on-error 把失败改为成功。

- [x] **1. 写防假绿的红测试。** 在 driver 自测加入 `test_rejects_empty_case_list`，提交 schema_version=1、cases=[] 的合法 JSON，断言驱动退出 2、未打印 PASS；运行 `python -m unittest discover -s tests/driver -v`，当前空清单会返回成功，应真实观测到断言失败。保留原有坏 golden、超时后继续、严格 UTF-8 与隔离用例。

```python
def test_rejects_empty_case_list(self):
    result = self.run_cases([])
    self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
    self.assertNotIn(b"PASS", result.stdout)
```

- [x] **2. 实现并验证测试入口。** 驱动将空 cases 作为 manifest 错误返回 2；CI 包装器对 unittest 用同一 discover 得到非空 suite 后实际运行，内部程序无匹配本已返回非零；逐条保留失败状态。新增的空 manifest 测试和现有 driver 全套均为绿。数量来自实际运行，不用固定的 M1 25/26/8/7 代替测试发现，也不用当前未接入的 ctest 代替内部测试。
- [x] **3. 实现工作流与可本机复现的脚本。** 配置/构建命令沿用任务 01/18，分别选择 Debug/ON 和 Release/OFF，两个 job 都从空构建目录并行构建。Debug 跑 `build/debug/myb_internal_tests.exe`；两种配置均显式设置 MYB_INTERPRETER 后运行 CLI unittest，再运行默认 `python tools/run_blackbox.py --interpreter <本配置myb.exe>`；driver 自测归 Debug。不固定为 smoke 子 manifest，不依赖 examples/hello.myb 固定输出，保留字节 fixtures 的 .gitattributes。

```powershell
& ./tools/ci.ps1 -Configuration Debug -Msys2Root <实际MSYS2根目录>
& ./tools/ci.ps1 -Configuration Release -Msys2Root <实际MSYS2根目录>
```

- [x] **4. 验证真正检出失败。** 先在本机分别运行脚本，对正确源码均退出 0；再在 dev 派生的临时验证分支，按当次授权范围推送并创建目标 dev 的验证 PR。保存正常 → 临时写错 three.stdout → 恢复预期三个提交各自的两 job 状态、运行链接与 SHA；错误预期必须使黑盒出现 mismatch 并令两 job 非零，恢复后两 job 全绿。不能以本机成功或仅 YAML 文件存在标记 CI 已运行；故意失败的文件改动最终必须恢复，不绕过 PR 写 main。
- [x] **5. 记录验收证据并提交。** 在 docs/CI与合并规则.md 记录环境、动作 SHA、触发分支、固定检查名、命令、动态测试计数、三个远端运行及日志入口；README/构建文档给出本地同等命令。日志包含 configure/build/测试输出及提交 SHA，失败时用 always 上传已有日志；不把管道测试计作真实控制台验证。新开普通代码 PR 及仅文档变更 PR 都应出现两项检查，未来加合并队列时再补 merge_group。提交 `ci: add Windows build and regression checks`；远端验收尚未执行时本任务保持未完成。

触发语义及跳过检查的影响以 [GitHub workflow 语法](https://docs.github.com/en/actions/reference/workflows-and-actions/workflow-syntax)和[必需检查排障](https://docs.github.com/en/pull-requests/how-tos/merge-and-close-pull-requests/troubleshooting-required-status-checks)为依据；当前方案不通过路径筛选省略必需检查。

### 任务 CI-02：验证并配置 PR 强制合并条件

**依赖：**CI-01 已有实际成功运行，两个固定检查名已在 GitHub 出现。本任务已在用户将仓库改为 public 后，按最新明确授权实施并读回核验。原验收未改动 main 的既有其他保护；用户随后取消了 `Restrict updates`，PR、线性历史及代码质量/CodeQL 要求仍保留。当前配置与原验收历史分别见 [CI 与合并规则](../rules/CI与合并规则.md)，CI 绿色不代表其他合并条件自动满足。

**Files/配置:** Modify `docs/CI与合并规则.md`；读取并增补 GitHub 的 dev/main 现有 ruleset 或 branch protection，不将文档中的规则当成平台配置，不覆盖已有更严格的要求。

**Interfaces:** main 必须通过 PR 更新并要求 `windows-debug`、`windows-release` 两项最新有效检查通过，保留禁止直接写 main 的约束；dev 要求同两项状态检查，本任务不额外要求所有 dev 改动必须走 PR。检查来源使用 GitHub Actions；平台设置采用实际运行显示的精确名称并要求与当前目标分支保持最新。任何失败、缺失、待运行或取消的必需检查都不能作为可合并成功证据。

- [x] **1. 核验现状与能力。** 读取仓库默认分支、现有保护规则、两项实际检查名及管理权限；按 GitHub 当前仓库类型/可用能力确认可设置强制合并条件。无法设置时记录具体权限或能力原因与手工配置步骤，本项保持待完成，CI-01 构建测试成功须与平台已强制执行合并条件分别记录。
- [x] **2. 准备并应用配置。** 保存原有配置及精确增补内容：main 的 require PR、两项 required status checks 和最新分支要求；dev 的两项 required status checks。保留已有保护，不加入 CI 机器人推送/自动合并权限，不为试跑直接更新 main。按照当次执行授权通过 GitHub 界面或 API 配置并读回核对。
- [x] **3. 验证失败被拦截。** 在派生验证分支复用错误 golden 的失败版本，分别面向 dev/main 检查 PR 合并状态，证明所需检查失败确实阻止合并；面向 main 的验证 PR 不执行实际合并。保存设置读回、检查结果与阻止原因，不仅截取 Actions 页红色。
- [x] **4. 恢复并确认成功条件。** 恢复预期，确保针对最新 PR 提交的两项检查成功且必需状态检查条件已满足；如还有冲突或其他原有审批要求，分别记录，不为得到“可合并”而降低保护。恢复故意失败差异，不通过管理员绕过检查；保存最终 SHA 和运行链接。
- [x] **5. 文档与提交。** 在 docs/CI与合并规则.md 分别记录 CI 自动运行、远端规则生效和红绿合并状态证据；提交 `docs: record required CI merge checks`。只有 CI-01 与 CI-02 各自验收通过，才把本 CI 阶段标为完成。

GitHub 的 required checks 与跳过/取消状态行为见 [状态检查](https://docs.github.com/en/pull-requests/reference/status-checks)和[必需检查排障](https://docs.github.com/en/pull-requests/how-tos/merge-and-close-pull-requests/troubleshooting-required-status-checks)。

## M2：完整前端、变量控制流与函数

### 任务 07：完整文法与全文件结构检查

**Files:** Modify lexer.l、parser.y、scanner/driver、AST与Checker；Create `tests/fixtures/frontend/all_syntax.myb`、`tests/internal/checker_cases.cpp`、`tests/blackbox/frontend/manifest.json` 和 cases/expected；Modify CMake、默认manifest。

**Interfaces:** Consumes05/03；Completes全部29个具体节点和EBNF，`CheckedAst{borrowed AstProgram,owned DeclarationIndex,owned LiteralTable}`；Checker借静态21内置名称，不调用回调。DeclarationIndex保存函数/结构编号、参数数和字段类型/顺序，不建立运行时全局变量。

- [x] **1. 写红测试。** checker_contract：`if(false){missing(1,2);} len(); let x=x+1;` 检查通过；`-(9223372036854775808)` 前端1，`(-9223372036854775808)` 通过，`-(-9223372036854775808)`不折叠外层。黑盒 `print("before"); if(false){break;}` 应exit1且stdout空，覆盖 Review Focus 1。

```myb
print("before");
if (false) { break; }
```
- [x] **2. 确认红。** 重建，运行 `--case checker_contract`、`--case frontend_parse`，此时完整语法/结构断言应失败。尚未完整生成的新语法先用内部检查器验收，07的执行前错误黑盒待08接入完整生成后转绿。
- [x] **3. 实现。** 全部运算优先级、赋值目标、切片/构造、控制语句和顶层限制；不支持形式明确拒绝。三遍完成顶层冲突、字段类型、全部子树结构检查；参数/函数外层体同层，for source先于体绑定。只有直接UnaryMinus(IntToken幅值2^63)规范化；赋值受保护名不提前拒绝RHS，构造字段集合留运行时。
- [x] **4. 验证绿。** 两内部case为0；全语法fixture包含5顶层项/2参数函数，Group、短路、Unary→Index及省略切片形状正确。补同层重名/受保护声明/前向互递归结构/未知字段类型、字符串转义、坏数字/尾逗号/空语句、空块/函数/结构；Bison无未解释冲突，生成头依赖正确。
- [x] **5. 提交。** 提交内部验收已通过的前端/检查器和预期文件，消息 `feat: complete MYB grammar and structural checking`；未接入生成的黑盒放分组manifest，不提前加入默认全绿集，08完成后立即聚合。

### 任务 08：完整编译产物、声明点布局与字节码核验

**Files:** Create `src/bytecode/binding_layout.hpp`、`src/bytecode/binding_layout.cpp`、`tests/internal/bytecode_cases.cpp`；Modify program/compiler/verify、fixtures、CMake和frontend/default manifests。

**Interfaces:** Produces `BindingLayout::build(const CheckedAst&)->BindingLayout`；编译器补齐标签与指令生成；`verify(CodeUnit&)` 写回 local_count/max_operand，错误为HostFailure/Internal。副表CallSite/ConstructionPlan/IterationSite、函数单元、稳定Schema和程序名称池按设计实现。

- [x] **1. 写红测试。** bytecode_layout 编译 `let x=3; fn f(){let before=x;let x=x+1;if(false){let y=unknown;}}`，要求before读全局，自初始化先DECL_LOCAL再LOAD_LOCAL，未知名保留；参数从槽0起、局部不复用、子树区间连续。bytecode_verify 手工构造非法跳转/栈下溢/不等汇合，均Internal/3；合法动态错误不得执行。

```cpp
// unit 的合法路径为 CONST(1), CONST(2), BINARY(Add), DROP, HALT。
verify(unit);
REQUIRE(unit.max_operand == 2);
```
- [x] **2. 确认红。** 重建并运行 `--case bytecode_layout`、`--case bytecode_verify`、`--case bytecode_constant_roots`，记录缺少完整单元/布局/核验的失败。
- [x] **3. 实现。** 全操作码/栈效应/源码条目、绝对指令号标签回填、CALL按k→1分析、死代码结构核验；函数默认null+RETURN，顶层HALT。先编号再按声明点解析，不提前激活let；构造原始字段副表保留重复项。编译期ProgramConstants根先登记，各单元String常量经临时根交入表，生成数据不借AST。
- [x] **4. 验证绿。** 三内部case为0；强制每允许点GC后，释放AST再收集，未调用函数的String常量仍可读，ProgramConstants追加/登记故障无悬空根。07前端黑盒通过；额外 --check 对未知名称、错误参数数/构造字段集合、自初始化均成功无输出；将frontend组聚合默认manifest。
- [x] **5. 提交。** 内部及已接入黑盒全绿后提交 `feat: generate verified units and declaration-point bindings`。

### 任务 09：完整动态运算与数值边界

**Files:** Modify `src/runtime/value_ops.hpp`、`src/runtime/value_ops.cpp`、operators.hpp；Create `tests/internal/value_ops_cases.cpp`；Modify CMake。

**Interfaces:** ValueOps借Heap/根/首错上下文；`unary(UnaryOp,Value,ErrorSite)->Value`、`compare(CompareOp,Value,Value,ErrorSite)->Value` 仅返回标量；`binary(BinaryOp,Value,Value,ErrorSite,RootedValue&)` 统一输出分配结果。结构字段检查与序列方法在12接入。

- [x] **1. 写红测试。** value_ops 精确断言 -7//3=-3、-7%3=2、7%-3=-2、MIN%-1=0、MIN//-1为Arithmetic；MAX+1、MIN取负、除正负零均失败。`9007199254740993 == 9007199254740992.0` 为false，MAX < 2^63为true，Bool不充当数值。

```cpp
auto equal = ops.compare(CompareOp::Equal,
    Value{std::int64_t{9007199254740993}}, Value{9007199254740992.0}, site);
REQUIRE(!std::get<bool>(equal.data));
```
- [x] **2. 确认红。** 重建运行 `& .\build\debug\myb_internal_tests.exe --case value_ops`，未实现运算或边界断言应失败。
- [x] **3. 实现。** GCC checked溢出包装、整除/余数特判后floor修正；浮点finite与下溢规则、混合比较先范围分界再安全截断；严格Bool、String码点比较/拼接、数组/结构身份相等与跨类别规则。拼接走受控Builder，输入Value在may-GC前有根。
- [x] **4. 验证绿。** 同case为0；补跨数值有序/相等、可空值与不可排序类别、String无规范化、Int64两端转换；核对错误位置为运算符，右操作数求值错误不能被左类型错误抢先报告。
- [x] **5. 提交。** 检查宿主signed UB/有损比较/根空窗后提交 `feat: complete checked dynamic value operations`。

### 任务 10：变量、短路、块、if/while 与值区增长

**Files:** Create `src/vm/value_store.hpp`、`src/vm/value_store.cpp`、`src/vm/globals.hpp`、`src/vm/globals.cpp`、`tests/internal/vm_scalar_cases.cpp`、`tests/blackbox/scalar/manifest.json` 和 cases/expected；Modify vm/compiler、CMake、默认manifest。

**Interfaces:** Completes帧base/operand_base/region_end/sp与三态、局部/全局表、CLEAR_LOCALS与所有标量/分支指令。全局查找不插入默认条目，读/写位置由本单元编号定位；跨容器增长只保存索引。

- [x] **1. 写红测试。** scalar/bindings.myb：`let x=3;{print(x);let x;x=7;print(x);} if(false){print(missing);} print(false&&missing);` 输出 `3\n7\nfalse\n`、exit0。`let x=1;{let x=x+1;}` Uninitialized/1；`print(x);let x=1;` Name/1；`let x;x=null;print(x);` 输出null。`print(true+missing);` 先Name，`print(1&&true);` Type。

```myb
let x = 3;
{ print(x); let x; x = 7; print(x); }
if (false) { print(missing); }
print(false && missing);
```
- [x] **2. 确认红。** 重建运行 `--case vm_scalar` 与 `python tools/run_blackbox.py --interpreter build/debug/myb.exe --manifest tests/blackbox/scalar/manifest.json`，新变量/跳转或输出断言应失败。
- [x] **3. 实现。** 声明先未初始化、运行后才全局入表；按既定映射查局部/外层。短路左右分别严格检查Bool，汇合留下一个Bool；普通语句落出栈深不变。扩容遵循64/16与1.5倍策略，Value/状态准备全部成功再提交范围，GC只枚举初始化局部和有效操作数。
- [x] **4. 验证绿。** 两组返回0；`let x=1;x="a";print(x);` 输出a，证明可改绑不同类型；while零/多轮、break/continue最内层、块遮蔽与退出根清除均覆盖。vm_growth_failure在VmValues成功后VmStates失败、各reserve失败时，断言Resource/3、旧sp/帧可扫描、无未提交根/二次错误；满足 Review Focus 5。已有smoke/frontend回归仍绿。
- [x] **5. 提交。** 聚合scalar组后提交 `feat: execute dynamic bindings and scalar control flow`。

### 任务 11：用户调用、native 交接与真实调用链

**Files:** Create `src/vm/call.hpp`、`src/vm/call.cpp`、`tests/internal/vm_call_cases.cpp`、`tests/blackbox/calls/manifest.json` 和 cases/expected；Modify compiler、vm/frame/value_store、registry、fixtures、CMake、默认manifest。

**Interfaces:** Produces `VM::prepare_call(CallSiteId)`、`return_to_caller(RootedValue&)`；补齐FunctionInfo/不可变命名目标表、帧executing_ip/next_ip与caller_arg_base/caller_call_ip/iteration_base。静态BuiltinDescriptor的名称/固定数量可提前用于结构保护；回调随所属任务补齐，最终16要求21项均为实际实现，不引入运行时动态注册框架。

- [x] **1. 写红测试。** calls/nested.myb：`fn add(a,b){return a+b;} print(100+add(3,4));` 输出107；`fn tag(){return "北"+"邮";} print(tag());` 输出北邮。`fn f(x){} f(write("X"),write("Y"));` stdout空/数量错误1；`fn f(a,b){} f(write("A"),missing);` 仅A/Name1，调用链不含f。未知目标同样不执行实参，覆盖 Review Focus 2。

```myb
fn add(a, b) { return a + b; }
print(100 + add(3, 4));
```
- [x] **2. 确认红。** 重建运行 `--case vm_calls` 与 `python tools/run_blackbox.py --interpreter build/debug/myb.exe --manifest tests/blackbox/calls/manifest.json`，新用户帧/返回断言应失败。
- [x] **3. 实现。** PRECALL预检→全部实参→准备父完整保留区之后的子区域；参数复制/初始化并完成可扫描状态后无分配提交活动帧，才撤父实参。普通MYB调用仅推进同一个VM循环；RETURN先根→统一清理→恢复父预留结果槽。native期间禁VM扩容/重入，先清旧args再写重叠结果，out根持续保护。
- [x] **4. 验证绿。** 两组为0；默认null/零实参、前向互递归、词法全局可见性均正确：全局x=1、caller局部x=2而被调f读x时应得1；f在全局let执行前读取x应为Name/1。4096层计数递归输出4096、不借宿主递归；强制GC下tag返回新字符串仍存活。vm_call_failures逐个准备点失败为3、子帧不入链、旧帧根完整；native无伪帧、调用位置用executing_ip，清理不覆盖首错。
- [x] **5. 提交。** 聚合calls组后提交 `feat: add explicit function frames and protected handoffs`。

## M3：完整对象、遍历、内置与 Windows I/O

### 任务 12：数组/结构构造、访问、写入及数组内置

**Files:** Create `src/runtime/sequence_ops.hpp`、`src/runtime/sequence_ops.cpp`、`src/builtins/array_builtins.cpp`、`tests/internal/vm_object_cases.cpp`、`tests/blackbox/objects/manifest.json` 和 cases/expected；Modify ValueOps、compiler、vm、registry、CMake、默认manifest。

**Interfaces:** Produces `SequenceOps::get_index(Value,Value,ErrorSite,RootedValue&)`、`get_slice(Value,optional<Value>,optional<Value>,ErrorSite,RootedValue&)`、`set_index(Value,Value,Value,ErrorSite)`；`ValueOps::check_field_value(const FieldConstraint&,Value,ErrorSite)->Value`。CHECK_STRUCT/CHECK_FIELD/BUILD_STRUCT按本单元原始方案执行；新增len/append/insert/pop/remove_at回调，活动计数检查从此存在。

- [ ] **1. 写红测试。** `struct P{a:Int64;b:Int64;} let x=P{b:write("B"),a:write("A")};` 仅输出B后Type1；改为 `P{bad:write("X")}` 不输出X，集合预检1，--check仍成功。`let a=[[1]];let b=a[:];append(b[0],2);print(a[0][1]);print(a==b);` 输出 `2\nfalse\n`；`print("北邮😀"[-1]);` 输出emoji。

```myb
struct P { a: Int64; b: Int64; }
let x = P { b: write("B"), a: write("A") };
```
- [ ] **2. 确认红。** 重建，运行 `--case vm_objects` 和 `python tools/run_blackbox.py --interpreter build/debug/myb.exe --manifest tests/blackbox/objects/manifest.json`，新对象/访问/字段断言应失败。
- [ ] **3. 实现。** 元素LtoR；构造集合先检、每字段即时检查和Int64→Float64提升，最终按声明顺序存储。索引/显式端点全求完后按原接收对象当前长度检查；省略不同于null。完整RHS先于最终LHS位置，最终目标不先LOAD；中间访问仍逐层执行。所有结果走受保护Heap，pop/remove_at先把移出值交out根再删槽。
- [ ] **4. 验证绿。** 两组为0；`1[write("I")]=write("R");` 先RI后Type，`print(1[write("S"):write("E")]);` 先SE后Type；端点改变长度后取当前长度。补负/越界/空切片、String不可写、显式null端点、insert的0..n/-n..-1、空pop、零字段独立身份、nullable/具名结构约束/数组字段混合元素。`fn make(){return [];}print(make()==make());` 必须false，不能把可变字面量缓存为常量。vm_objects_gc强制GC，嵌套/切片/移出值均保活，失败不发布半成品。
- [ ] **5. 提交。** 聚合objects组，复验参数改绑隔离/数组修改共享后提交 `feat: implement object operations and array builtins`。

### 任务 13：for、活动遍历与所有出口清理

**Files:** Create `src/vm/iteration.hpp`、`src/vm/iteration.cpp`、`tests/internal/vm_iteration_cases.cpp`、`tests/blackbox/iteration/manifest.json` 和 cases/expected；Modify compiler、frame/vm/call、TestHooks、CMake、默认manifest。

**Interfaces:** Produces `VM::begin_iteration(IterationSiteId)`、`next_iteration(IterationSiteId)->bool`、`end_iteration(IterationSiteId) noexcept`、`unwind_all() noexcept`；活动记录持来源Value、帧/站点、游标和登记状态，参与根；Array计数而非布尔锁。内部fixture提供清理后记录/活动计数检查，不增加用户接口。

- [ ] **1. 写红测试。** `fn pick(a){for(let x in a){let r=[x];if(x==1){continue;}return r;}return [];} let a=[1,2];let r=pick(a);append(a,3);print(r[0]);print(len(a));` 输出 `2\n3\n`。`let a=[1];for(let x in a){for(let y in a){break;}append(a,2);}` IterationMutation/1，证明内层退出不解除外层。

```myb
let a = [1];
for (let x in a) {
    for (let y in a) { break; }
    append(a, 2);
}
```
- [ ] **2. 确认红。** 重建运行 `--case vm_iteration` 与 `python tools/run_blackbox.py --interpreter build/debug/myb.exe --manifest tests/blackbox/iteration/manifest.json`，应在遍历或出口协议处失败。
- [ ] **3. 实现。** source在旧环境求一次；先准备记录容量，再无分配提交来源根/checked计数。每轮新绑定，与本轮体同层；String字节游标产生单码点String。正常/continue清本轮局部，continue保留目标for；break/耗尽恰好ITER_END，return仅统一收尾。编译器安排词法清理，普通块不建立运行时作用域记录。
- [ ] **4. 验证绿。** 两组为0；已有元素替换后续轮可见（1、9），重绑来源变量不换遍历对象；别名/字段/参数增删都先拒绝。覆盖内层while出口、同数组嵌套、Unicode、每轮未初始化、source同名外层、return结果。vm_iteration_failures对记录分配、码点分配和循环体错误断言：诊断先保存、记录/全部数组计数归0、首错不变，满足 Review Focus 5。
- [ ] **5. 提交。** 聚合iteration组后提交 `feat: enforce active iteration and lexical exit cleanup`。

### 任务 14：深拷贝与完整统一显示

**Files:** Create `src/runtime/graph_copy.hpp`、`src/runtime/graph_copy.cpp`、`src/builtins/scalar_builtins.cpp`、`tests/internal/graph_display_cases.cpp`、`tests/blackbox/display/manifest.json` 和 cases/expected；Modify display、registry、CMake、默认manifest。

**Interfaces:** Produces `deep_copy(Heap&,RootRegistry&,Value,RootedValue&)`、`format_value(Heap&,Value,TextBuilder&)`，输入必须已有根；deep_copy/str回调接入统一NativeFn。Scalar显示委托03，不复制浮点算法；print/write/str共用同一显示器。

- [ ] **1. 写红测试。** 共享子数组显示 `[[1], [1]]`，自环显示 `[<cycle>]`；graph_display要求副本不同于原可变对象、两共享边指向同一副本、自环回到副本、改副本不改原对象；String可共享。精确golden包括 `Marker {}`、`Point { x: 1, y: 2 }`、根字符串原样、嵌套双引号和控制转义、0.0/-0.0/科学阈值。

```myb
let a = [];
append(a, a);
print(a);
```
- [ ] **2. 确认红。** 重建运行 `--case graph_display` 与 `python tools/run_blackbox.py --interpreter build/debug/myb.exe --manifest tests/blackbox/display/manifest.json`，应因图算法/精确显示未完成而失败。
- [ ] **3. 实现。** 迭代memo＋工作队列登记为NativeRootSet，原对象键/副本/半成品均追踪；新副本先登记再处理子边。显示器显式栈和当前路径集合，退出路径后共享对象可再展开，字段按声明顺序；TextBuilder增长计入压力，无MYB回调。str先make_string进out根，native交接复用首参数槽不清掉新结果。
- [ ] **4. 验证绿。** 两组为0；小图在每允许点强制GC并注入工作区失败，原图保留、无悬空根/半成品发布；另构建5万层图链，关闭逐点强制GC，仅按正常阈值回收，复制/显示不按图深递归宿主栈。运行语言规范12.1/设计末尾组合例（含assert的部分在15后接入），共享/复制/字段显示精确通过。
- [ ] **5. 提交。** 聚合display组后提交 `feat: preserve shared graphs in deep copy and display`。

### 任务 15：转换、断言与文本辅助内置

**Files:** Create `src/builtins/text_builtins.cpp`、`tests/internal/builtin_cases.cpp`、`tests/blackbox/builtins/manifest.json` 和 cases/expected；Modify scalar_builtins、array_builtins、registry、CMake、默认manifest。

**Interfaces:** Completes int/float/try_int/try_float/assert/split/join/trim/split_lines 与Array/String len；`invoke_builtin(const BuiltinDescriptor&,NativeContext&,span<const Value>,RootedValue&)` 只在预检和全部实参之后做值检查。回调成功必须写out；调试“已写”标记不以默认null代替协议验证。

- [ ] **1. 写红测试。** builtins/parse.myb：`print(try_int("bad")); print(try_float("-1e-9999")); print(int(-2.9));` 输出 `null\n-0.0\n-2\n`；内部随后assert(false,"stop")必须Runtime/Assertion，首错不受try失败污染。`assert(true,write("M"));` 先输出M再因message非String报Type；Bool/Null不能int/float转换。

```myb
print(try_int("bad"));
print(try_float("-1e-9999"));
print(int(-2.9));
```
- [ ] **2. 确认红。** 重建运行 `--case builtins` 和 `python tools/run_blackbox.py --interpreter build/debug/myb.exe --manifest tests/blackbox/builtins/manifest.json`，预期新回调/约束断言失败。
- [ ] **3. 实现。** 静态表固定参数数，普通无结果写null；解析调用03，try仅Content/Range转null，类型/资源故障不吞。join先检查全String再构造；split结果数组/片段逐步根接管，精确非重叠拆分；trim仅ASCII9–13/32，split_lines只CRLF/LF、单CR为内容。
- [ ] **4. 验证绿。** 两组为0；`split("a,,",",")` 精确为 `["a", "", ""]`，`split_lines("a\r\n\r\nb\r")` 为 `["a", "", "b\r"]` 的既定转义显示；空串split_lines=[]、`join([],",")==""`、末尾换行规则、中文/emoji、多码点与重叠分隔符、NBSP不trim、join混合元素失败均有独立期望。强制GC/资源故障不丢片段、不冒充null；14的完整组合例通过。
- [ ] **5. 提交。** 聚合builtins组后提交 `feat: implement conversions assertions and text builtins`。

### 任务 16：Windows 行输入与整文件读写

**Files:** Create `tests/internal/host_io_cases.cpp`、`tests/blackbox/io/manifest.json` 和二进制fixtures/cases/expected、`docs/Windows终端验收.md`；Modify windows_io、io_builtins、registry、TestHooks、CMake、默认manifest。

**Interfaces:** Completes02的HostIO读取/写文件接口，回调经Utf8Sink/TextBuilder再Heap.make_string交out；IoFailure只携宿主原因/代码，调用者决定入口2或程序1，bad_alloc/Capacity仍3。此项结束21回调全部实际实现，表中名称/数量/非空回调逐一核对。

- [ ] **1. 写红测试。** stdin fixture字节为 `b"ok\n\xff\n"`，连续两次read_line：先正常输出 `ok\n`，第二次Encoding/1，不能预读坏第二行导致首行失败。空stdin返回null，空行返回""，无换行末行先返回内容再null；`b"\x1a\n"` 在管道是内容。read_text去一次BOM保留CRLF/NUL，write_text字节无新增BOM/换行。

```json
{
  "id": "valid-line-before-invalid",
  "script": "cases/two_lines.myb",
  "stdin_file": "fixtures/valid_then_bad.bin",
  "expect_exit": 1,
  "expect_stdout_file": "expected/ok.stdout",
  "expect_stderr_regex": "(?s).*Encoding.*"
}
```
- [ ] **2. 确认红。** 重建运行 `--case host_io` 与 `python tools/run_blackbox.py --interpreter build/debug/myb.exe --manifest tests/blackbox/io/manifest.json`，应在I/O/编码/EOF行为处失败。
- [ ] **3. 实现。** 分标准句柄识别，控制台Read/WriteConsoleW严格桥接，管道/文件Read/WriteFile字节；保留跨块UTF-8/代理对，不因未返回行坏字节提前报错。输入零进度/ERROR_HANDLE_EOF/读端BROKEN_PIPE为EOF，写端broken pipe为失败；部分写入循环、零进度停止、保存原错误码，write_text显式检查close，RAII失败清理不抛。
- [ ] **4. 验证绿。** 两组为0；中文/空格相对路径按启动cwd，缺失父目录不创建、文件失败1/源入口读取失败2、路径NUL拒绝而文本NUL按长度保留，跨缓冲字符和OsRead/OsWrite/OsClose注入分类正确。分别记录 Windows Terminal/传统控制台的中文emoji、代理边界、LF/CRLF与仅Ctrl+Z整行EOF；管道测试不能充当这份控制台证据。
- [ ] **5. 提交。** 聚合io组、逐表核对21内置后提交 `feat: complete Unicode Windows text IO`；未实测的控制台组合在验收文档明确保留未完成，不能标成通过。

## M4：故障边界与最终验收

### 任务 17：完整诊断、低资源降级和跨模块故障验证

**Files:** Create `src/diagnostic/diagnostic.hpp`、`src/diagnostic/diagnostic.cpp`、`tests/internal/failure_integration_cases.cpp`、`tests/blackbox/errors/manifest.json` 和 cases/expected；Modify failure、Session、VM统一边界、TestHooks、WindowsHostIO、CMake、默认manifest。

**Interfaces:** Produces拥有存储的Diagnostic/出错时的实际调用链记录及 `report_failure(const Diagnostic&,HostIO&)`；EmergencyFailure固定记录和无分配最小报告路径保持独立。完成全部FaultPoint接缝；HostIO错误不直接成为语言异常对象，用户不获得catch/栈访问接口。

- [ ] **1. 写红测试。** 设计demo的inner在第2行 `return x + 1;` 失败，首行位置精确2:14，Type/1；实际链outer→inner，print不入用户链。错误参数/入口读取无虚构位置；invalid UTF-8为Frontend1。failure_fallback先登记Runtime/Type，再注入DiagnosticSnapshot/bad_alloc/OsWrite，保持exit1，报告最多一次；没有已有首错的宿主故障为3。

```cpp
EmergencyFailure first;
first.record_first({FailureDomain::Runtime, ReasonCode::Type, {}, {}});
first.record_first({FailureDomain::Resource, ReasonCode::Allocation, {}, {}});
REQUIRE(first.header().domain == FailureDomain::Runtime);
REQUIRE(first.header().reason == ReasonCode::Type);
```
- [ ] **2. 确认红。** 重建运行 `--case failure_integration` 和 `python tools/run_blackbox.py --interpreter build/debug/myb.exe --manifest tests/blackbox/errors/manifest.json`，应在排版/链/低资源路径处发现未满足断言。
- [ ] **3. 实现。** VM顶层边界在帧/源码可用时先保存诊断和实际调用链，再unwind；详细消息失败退固定首错，不为了清理分配/GC。真实executing_ip归失败，用户文本控制字符转义、无源码摘录；stderr失败停止递归。显式注销根幂等，Builder/VM先于Heap清空，Heap先于Schema，Registry最后；未知宿主异常为Internal3，不伪装成MYB错误。
- [ ] **4. 验证绿。** 两组为0；跨04/08/11/13/14/16枚举故障位置，要求无悬空登记/根、未提交函数不入链、标记失败不sweep、遍历计数在Heap整体清空前已归零。用额外RootedValue保留被遍历数组以证明解除确实发生，不能只因对象已销毁而得到计数零。源码含300层分组的语法合法表达式触发AST Capacity/3而非Syntax/1，补外部资源分类证据。满足Review Focus 3/5；try_*与首错共存、既有stdout/文件副作用不回滚均覆盖。
- [ ] **5. 提交。** 聚合errors组后提交 `fix: preserve diagnostics and cleanup across host failures`；不将OS强制终止/内存破坏写成受控可恢复测试承诺。

### 任务 18：全量黑盒、干净/并行/Release 构建与复现材料

**Files:** Modify `tests/blackbox/manifest.json`、全部分组manifest/期望、CMake/生成依赖、`docs/构建与测试.md`、`README.md`；Create `docs/需求覆盖表.md`、`docs/测试报告.md`；按结果修正实现和回归，不为通过测试更改语言含义。

**Interfaces:** Default manifest聚合所有正式用例（非只含smoke）；一条 `python tools/run_blackbox.py --interpreter <myb.exe>` 运行全部，失败/超时继续统计。覆盖表记录语言规则/内置/设计协议→独立用例→实际结果；测试报告区分自动、内部、真实控制台及未验证平台。

- [ ] **1. 写验收用例。** 补齐D01–D72及D63澄清的正常/边界/错误/组合输入，特别是21内置固定数量和类型、全部操作码正常/清理路径、--check不消费stdin/不执行I/O/不写字节码。把规范12.1–12.3、解释器设计末尾完整例纳入独立golden；应用/性能任务只记录后续入口，不冒充本阶段成果。

```myb
print("before");
let result = 10 / 0;
print("after");
```
- [ ] **2. 验证用例能检出缺口。** 构建后运行全内部测试和默认黑盒；只对新覆盖导致的失败建立修复目标，不破坏已通过实现来机械制造红色。驱动自身“错误golden/死循环”检出能力由01的自测保证。
- [ ] **3. 修复与复现。** 逐一修复真实差异；新建build/verify-clean进行干净配置/并行初建，修改.l/.y及相关头验证增量重生成，再恢复无语义改动。用含空格/中文路径的检出副本重现；Release单独build/release，MYB_BUILD_INTERNAL_TESTS=OFF，不依赖Debug测试接缝/生成物。记录实际运行所需GCC DLL/PATH，核心不依赖MSYS运行时。
- [ ] **4. 验证绿。** Debug执行 `& .\build\debug\myb_internal_tests.exe`、默认黑盒和驱动unittest；Release执行 `python tools/run_blackbox.py --interpreter build/release/myb.exe`，全部正式例通过、退出0。记录工具版本、命令、计数、失败修复、控制台证据；真实控制台未完成则本任务仍未完成。README从零指令可复现，git中只有手写源/规则/固定版本依赖源码/测试/文档。
- [ ] **5. 提交。** 完成全分支独立审查、处理实际问题并复验后提交 `docs: record reproducible MYB interpreter acceptance`；本计划不自动授权推送/合并/main写入。验收成功才将对应checkbox标完成，保存实际证据位置。

## 设计覆盖与任务归属

| 设计要求 | 主要任务 | 必须保留的证据 |
|---|---|---|
| 源文件执行、阶段顺序、CLI/--check | 02/06/07/08/17 | 入口2、前端/结构1、检查无副作用、动态错误仍可检查通过 |
| 源码/位置、Q01、Bison/Flex接入与fatal | 02/05/07 | 原字节偏移、Unicode列、EOF、失败后重解析、分配清理 |
| AST所有权/高度、只读Visitor | 05/07 | 基类析构、256边界、无默认漏节点、Group与省略信息 |
| 结构检查/声明点/未初始化 | 07/08/10 | 不可达结构错无输出、读取外层/自己、未初始化不同于null |
| 单元/常量/指令/容量分析 | 06/08 | 单元独立编号、完整栈效应、跳转与汇合、编译根和释放AST |
| Value/Schema/Heap/GC/增长 | 04/08/12/17 | 首次分配已有根、循环回收、半成品、工作表失败、阈值与容量 |
| 数值/比较/D72 | 03/09/14/15 | 精确位/文本、整数溢出/floor、混合比较、underflow/负零 |
| 帧/统一值区/调用/原生交接 | 06/10/11/17 | 父临时值、参数不重叠、提交时机、native结果不被清掉 |
| 对象访问/赋值/构造时序 | 12 | 完整RHS先于LHS、端点副作用、集合预检、逐字段即时检查 |
| for/块/循环出口与错误清理 | 10/13/17 | 每轮状态、别名禁增删、嵌套计数、continue/return/错误收尾 |
| deep_copy/显示/21内置 | 12/14/15/16 | 共享/循环/字段身份、统一格式、全部成功/类型/数量边界 |
| Windows/UTF-8/文件/管道/控制台Q02 | 02/16 | 字节fixture、中文路径、坏编码/EOF/部分I/O、真实终端记录 |
| 故障分类/首错/报告降级 | 02/11/13/17 | 0/1/2/3、不伪帧/位置、先保存诊断和调用链后清理、stderr失败不递归 |
| GitHub CI/强制合并条件 | CI-01/CI-02 | 全新 Windows job、非空测试集、原生命令失败非零、PR 正常→失败→恢复运行及合并状态、实际保护设置 |
| 构建/测试自动化 | 01/05/18 | 干净/增量/并行/Release、驱动自证、一条命令全量黑盒 |

## 自审记录与执行交接

- 已按设计逐节对照，解释器、21内置、GC/资源、Windows边界、CLI/诊断及构建/测试均有任务归属；原计划的测试值为预期；M1、M2 与 CI 已有实测证据，见阶段测试报告和 CI 与合并规则；M3–M4 的命令及验收仍为未来任务。
- 前期以内部协议/最小黑盒验收，中期按功能滚动聚合；07解析的新语法由08完整生成接管，11返回数组的对象验证由12/13接管，14完整例依赖15的assert。这些依赖不能被当作提前完成。
- 正式用户/Agent开发指南、>=200行应用及真实开发记录、Python同功能性能报告属于后续课程交付，继续按原路线图10–13独立安排；本计划不选择应用或新增游戏API。
- M1、CI-01、CI-02 已实施并验收。CI 工作流、代码/仅文档 PR、错误预期失败、恢复成功及远端强制规则均有实际证据；原 CI-02 验收未修改 main 的既有保护，main 验证 PR 已关闭且未合并。用户后续取消 `Restrict updates` 的现行配置见 [CI 与合并规则](../rules/CI与合并规则.md)；原 JSON 配置记录保留为历史验收证据。

M2 的任务 07–11 已于 2026-10-23 完成：Debug 46 个内部测试、10 个驱动测试、7 个 CLI 测试及 110 个黑盒通过；Release 7 个 CLI 测试及 110 个黑盒通过，31 条编译命令无测试依赖；独立审查通过。具体证据与统一提交的调整见 [M2 测试报告](../reports/M2测试报告.md)。

后续范围从 M3 任务 12“数组/结构构造、访问、写入及数组内置”起；M3–M4 尚未启动，继续按用户授权推进。
