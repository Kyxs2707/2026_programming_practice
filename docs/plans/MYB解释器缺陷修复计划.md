# MYB 解释器缺陷修复实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development or superpowers:executing-plans to implement this plan task-by-task. 使用 `- [ ]` 记录实际进度；本文件编写完成不启动实现。

**Goal:** 修复 2026-10-27 审计确认的 B01 调用查找、B02 结构字段平方成本和 B03 入口诊断路径问题，保留现有语言行为并给出修复前后证据；每个 PR 的 CI 必须通过 B01、B02 两项性能回归，任一项失败即阻止合并。

**Architecture:** VM 在发布首帧前构建只读 `ExecutionIndex`，统一提供调用目标、Schema、字段和构造槽位查询；原始字节码及 Schema 布局保持不变。入口读取在打开文件前保存绝对路径的 UTF-8 文本，错误诊断复用该文本。

**Tech Stack:** C++20、UCRT64 GCC 15.2.0、Flex 2.6.4、Bison 3.8.2、CMake 3.24+、Ninja 1.11+、Python 3.10+、PowerShell 7+；沿用已有依赖。

**Spec:** [缺陷审计报告](../reports/缺陷审计报告_2026-10-27.md)、[解释器设计](../specs/MYB解释器设计.md)“字节码生成、绑定与 VM 执行”第 1、8 节及“CLI、诊断与统一边界”、[语言规范](../specs/MYB语言规范.md)第 5、7、8.2 节。前序状态见[解释器实施计划](MYB解释器实施计划.md)。

日期：2026-10-28。状态：计划已编写，任务均待实施。当前检出为 `codex/myb-m4`，HEAD 为 `3db7dc1e7b8cdccab6512f4d7e517b16f265b448`；M1–M4 的既有验收记录保持原样。下文的数据结构和性能比例是本次计划的实施选择与验收目标，尚无修复后的实测结果。

2026-10-28，用户确认每次 PR 的 CI 都必须完成 B01、B02 性能回归检查，任一项失败不能合并；该要求列为任务 4 和实施完成条件。本轮仅补充计划，工作流及测试尚未实施。

## Global Constraints（全局约束）

- 在 `dev` 或其派生分支执行；开始实施前再次核对分支、工作区和用户新增修改。保留现有未提交的审计报告及 `tests/audit/20261027/reproduce.py`；本轮计划不提交、推送或合并它们。
- 保持普通名称的动态绑定、21 个内置、字节码操作码及 `--check` 现有阶段。`--check` 完成生成与核验后返回，不为本计划进入 VM 或准备执行索引。
- 未知目标、错误数量和非法构造集合仍在执行对应 PRECALL、CHECK_STRUCT 时失败；预计算只记录结果，不提前报告 MYB 错误。
- 保持从左到右的实参求值、构造逐字段即时检查、字段 Int64→Float64 提升、原有错误位置、调用链和 native 根交接。
- 索引只含宿主文本、编号和有明确寿命的借用，不含受管 Value；不新增 GC 根。有效查询不分配、不触发 GC，不使用线性查找作为空索引回退。
- 退出码仍为 0 成功、1 MYB 错误、2 用法或入口读取失败、3 受控宿主资源或内部故障；保留首错及 OS 代码，清理不分配、不运行 MYB。
- 新增或修改的跨模块接口、核心类型和复杂约束使用中文 Doxygen 注释，说明借用寿命、发布和失败责任；文档遵守[文档规则](../rules/文档构建原则.md)与[禁用词表](../forbidden_terms.yaml)。
- Debug、Release 分目录；构建与原始证据保存在 `build/`。每个指向 dev/main 的 PR 均运行 B01、B02 必过性能回归，包括仅文档 PR；Debug 检查实际工作量，Release 串行测量规模增长，两者都须通过。修复前后两份 EXE 的严格对照另列任务 5。

## 方案选择与依赖

| 方案 | 收益与成本 | 本计划选择 |
|---|---|---|
| VM 持有执行索引，入帧前准备 | 使用实际 BuiltinRegistry；兼容手造程序；增加一个执行准备模块 | 采用 |
| 把调用与字段缓存写入 CompiledProgram、StructSchema | 产物自带索引；需要调整编译器、内置登记传递及多处聚合初始化 | 本轮不采用 |
| 仅建字段表，每次构造重新验证集合 | 可消除平方查找；每轮仍需集合检查和辅助存储 | 采用一次准备状态与槽映射，减少执行期分配 |

B01 → B02 → PR 性能回归接入 → 集成验收；B03 可与 B01/B02 并行。B01、B02 共用 `execution_index.*` 和 `vm.*`，由同一实施者顺序修改。若 B03 并行，共用的 `CMakeLists.txt`、`test_hooks.hpp` 由主 Agent 合并，避免相互覆盖。

## Review Focus（容易遗漏的五类输入）

1. 自定义内置、构造后才替换回调的测试登记表，以及空回调：读取实际 Registry，空回调仍在 CALL 才失败。归任务 1。
2. 手造程序没有填充 `name_ids`，重复文本使用不同 NameId：调用按编号匹配，Schema 按文本匹配，保持现有首项优先。归任务 1。
3. 未执行的未知调用或非法构造，以及字段值出错后的副作用：准备期只记录状态，执行顺序与现有检查一致。归任务 1、2。
4. 索引准备中途分配失败、VM 换程序重用、强制 GC 下嵌套结构：无部分索引发布、无旧程序查询、无悬空元数据。归任务 1、2。
5. 相对入口在不同 cwd 打开/读取/关闭失败，以及诊断再次缺内存：显示对应绝对路径，仍退出 2且不伪造行列或调用链。归任务 3。

## 文件与接口

所有路径相对仓库根；标为新增的文件在实施时建立。

| 文件 | 职责 | 任务 |
|---|---|---|
| 新增 `src/vm/execution_index.hpp/.cpp` | 只读名称解析、字段索引和构造准备状态 | 1、2 |
| `src/vm/vm.hpp/.cpp` | 进入执行时准备索引，操作码查询索引，退出时清理 | 1、2 |
| 新增 `tests/internal/execution_index_cases.cpp` | 索引、手造程序、构造映射、准备失败及工作量增长测试 | 1、2、4 |
| `tests/internal/vm_call_cases.cpp`、`vm_object_cases.cpp` | 语义、返回、共享对象和 GC 回归 | 1、2 |
| `src/platform/windows_io.cpp` | 提前缓存入口绝对路径文本 | 3 |
| `tests/cli/test_cli.py`、`tests/internal/host_io_cases.cpp` | 真实 CLI 路径、入口故障与首错验证 | 3 |
| `tests/internal/test_hooks.hpp/.cpp`、`CMakeLists.txt` | Debug 故障位置和工作量统计、新模块与测试接入 | 1–4 |
| `tests/blackbox/objects/{cases,expected}/`、该组及默认 `manifest.json` | 补充多单元/多 Schema 组合；复用已有单项语义用例 | 2 |
| 新增 `tools/check_performance.py`、`tests/driver/test_check_performance.py` | 每个 PR 的两项 Release 性能回归及驱动自测 | 4 |
| `tools/ci.ps1`、`.github/workflows/ci.yml` | 必过性能检查、非零退出传播与结果上传 | 4 |
| 新增 `tests/audit/20261027/verify_fixes.py` | 两份 EXE 对照及严格修复验收，复用既有生成函数 | 5 |
| 现有未跟踪的 `tests/audit/20261027/reproduce.py`、原审计报告 | 按现有字节纳入实施交付，提供导入依赖及历史依据 | 4、5 |
| 新增 `docs/reports/MYB缺陷修复验收报告.md` | 实测结果、原始证据路径与完成状态 | 5 |
| `docs/specs/MYB解释器设计.md`、`docs/reports/需求覆盖表.md` | 同步执行索引归属和新增回归映射 | 5 |

`ExecutionIndex` 采用下列接口；任务 1 先实现调用与 Schema 部分，任务 2 补齐字段与构造部分。命名在后续任务中保持一致：

```cpp
enum class CallTargetKind : std::uint8_t { Missing, User, Builtin };
struct ResolvedCallTarget {
    CallTargetKind kind{CallTargetKind::Missing};
    std::uint32_t function_index{};
    const BuiltinDescriptor *builtin{};
};
enum class ConstructionStatus : std::uint8_t {
    UnknownType, NotConstructible, FieldSetMismatch, Ready
};
struct PreparedConstruction {
    ConstructionStatus status{ConstructionStatus::UnknownType};
    std::optional<SchemaId> schema;
    std::vector<std::uint32_t> slots;
};
// ExecutionIndex 的公开方法；全部类型位于 myb 命名空间。
void prepare(const CompiledProgram &, const BuiltinRegistry &, EmergencyFailure &);
const ResolvedCallTarget &call(NameId, ErrorSite) const;
const StructSchema &schema(NameId, ErrorSite) const;
std::size_t field_slot(const StructSchema &, NameId, ErrorSite) const;
const PreparedConstruction &construction(std::uint32_t unit,
                                         std::uint32_t plan, ErrorSite) const;
void clear() noexcept;
```

方法契约：`call`、`construction` 只检查索引边界并返回记录；合法的 Missing/无效构造由操作码报告。`schema` 保留现行构造目标 Name/Type 分类；`field_slot` 对缺失字段报告 Runtime/Name。内部编号越界或对象 Schema 不属于当前程序时报告 Internal/Invariant。`prepare` 使用内部的无动态异常解析结果，不调用会报告 MYB Name/Type 的 `precheck` 或 `schema`。

`VM::execute` 不对手造程序自动调用可修改产物的 `verify`。`prepare` 负责在任何索引访问前检查函数名称/单元编号、Schema 非空及 id 与位置对应、构造 type/fields 的 NameId 和相关单元编号；损坏元数据报告 Internal/Invariant/3。查询期边界检查继续保留。

VM、Program、Schema、实际 Registry 的描述表及其名称内容覆盖一次执行；执行期间不可修改。内置描述指针只在 `execute` 准备时取得。索引在局部临时对象中构建成功后用无异常 `swap` 发布；每次执行重新准备。诊断保存后，`unwind_all` 清除索引再撤销 `program_`；`clear` 幂等且不分配。

定向检查按[构建文档](../guides/构建与测试.md)先设置 PATH 并配置 Debug，再使用下列命令形式；每任务把命名用例或分组替换为该任务列出的项目。新增用例在实现前不可执行。

```powershell
cmake --build build/debug --parallel 4
& ./build/debug/myb_internal_tests.exe --case execution_index_lookup
python tools/run_blackbox.py --interpreter build/debug/myb.exe --manifest tests/blackbox/calls/manifest.json
$env:MYB_INTERPRETER = (Resolve-Path ./build/debug/myb.exe).Path
python -m unittest discover -s tests/cli -v
```

## 任务 1：修复 B01 调用目标及 Schema 查找

**Files:** 新增 `execution_index.*`、`execution_index_cases.cpp`；修改 `vm.*`、`vm_call_cases.cpp`、`test_hooks.hpp`、`CMakeLists.txt`。

**Interfaces:** 消费原始 `CompiledProgram` 与实际 `BuiltinRegistry`；提供上述 `prepare`、`call`、`schema`、`clear`，供任务 2 扩展。

- [ ] **1. 固定修复前材料。** 按[构建文档](../guides/构建与测试.md)确认可用 Release，复制 EXE 与三个 GCC DLL 到新目录 `build/bugfix-20261028/reference-release/`，记录提交和文件 SHA-256。以新 `--output build/bugfix-20261028/before` 运行原审计复现器，确认三个旧现象仍在；保留 `build/bug-audit-20261027/` 原证据。已有目标目录含结果时另取新目录。
- [ ] **2. 增加并运行失败测试。** 新建 `execution_index_lookup`、`execution_index_manual_program`、`execution_index_invalid_metadata`、`execution_index_failure_cleanup`：覆盖 5000 个无关函数后的用户/内置目标、前向和相互递归、自定义回调、用户函数优先、重复登记首项优先；手造 `names={"f","f"}` 且函数仅使用编号 1 时，编号 0 仍为 Missing。同文本的多个 NameId 都能构造对应 Schema。无效函数/构造编号、空 Schema 和错位 SchemaId 均受控失败。验证新接口未实现；用既有复现证明旧版本的性能问题。
- [ ] **3. 建立并接入只读索引。** 按 NameId 大小建立调用目标和可选 SchemaId 数组；函数按精确编号、源码顺序填充，保留首项；内置仅填未命中的项，使用实际 Registry 的 `find`。Schema 先建名称到首个 Schema 的表，再为每个名称编号填充；不依赖 `program.name_ids`。VM 在现有 `execute` 的 `try` 内、任何帧发布前准备；PRECALL/CALL 直接取记录，构造目标查询直接取得 Schema。保留原用户帧准备和 native 调用代码。
- [ ] **4. 钉住动态分类与失败时机。** 普通变量/未知名/类型名被调用时仍为 PRECALL 的 Runtime/Name；数量错误为 Runtime/Type；基础类型 `Bool/Int64/Float64/String/Array`、函数、内置被构造时为 Runtime/Type，普通变量/未知名为 Runtime/Name。空内置回调通过目标/数量预检，实参执行后在 CALL 报 Internal/Invariant。未知调用位于未执行分支时正常运行，`--check` 安静成功。
- [ ] **5. 验证准备失败与清理。** 新增仅 Debug 可用的 `FaultPoint::ExecutionIndex`，在会分配的向量增长和映射插入前命中；首处及部分索引形成后的第 N 次失败均为 Resource/3，无 MYB 输出、无用户帧和伪造源码位置。直接验证发布前失败不交付半成品；VM 清理后切换另一程序执行，不使用旧索引。该重用测试先保存并核对首错，再原位执行 `first = EmergencyFailure{}`、清空测试输出及故障设置，最后重用同一 VM。实际容器容量先检查，再转换为 uint32 编号。
- [ ] **6. 运行针对性回归并提交。** 构建 Debug，运行新增四个命名用例、现有 `vm_calls`、`native_root_handoff`、`native_requires_result`、`vm_return_roots`、`vm_call_failure_values`、`vm_call_failure_states`、`vm_call_failure_frames`、`failure_iteration_vm_prepare` 和 calls 黑盒组；全部通过后提交 `fix: 建立调用目标与结构名称索引`，只包含任务文件。

## 任务 2：修复 B02 字段集合与槽位平方成本

**Files:** 扩展 `execution_index.*`、`execution_index_cases.cpp`；修改 `vm.*`、`vm_object_cases.cpp`、objects 与默认黑盒清单，沿用任务 1 的故障位置。

**Interfaces:** 依赖任务 1 的只读名称解析；补齐 `field_slot`、`construction` 和 `PreparedConstruction`。原始 `ConstructionPlan::fields` 与 `StructSchema::fields` 继续保存源码顺序和声明顺序。

- [ ] **1. 增加并运行映射测试。** 新增 `execution_index_construction`：反序填写结构得到正确槽映射；零字段实例身份独立；同字段名在不同 Schema 的槽号不同；两个用户函数中同编号构造方案互不串用；嵌套构造结果正确。先验证缺少准备接口或映射行为，记录旧版本字段规模复现结果。
- [ ] **2. 线性准备字段与构造数据。** 每个 Schema 建立 `unordered_map<std::string,uint32_t>`，键拥有内容；重复声明字段等损坏元数据报告 Internal/Invariant。按单元号、方案号保存准备记录。类型解析失败仅记录 UnknownType/NotConstructible；数量不等或字段未知/重复记录 FieldSetMismatch；有效时用按声明槽编号的临时已访问数组，在一遍查找中保存按源码顺序排列的 `slots`。SchemaId 为 0 仍是有效编号，不用 0 作为缺失标记。
- [ ] **3. 接入操作码。** CHECK_STRUCT 根据准备状态在字段求值前报告原 Name/Type 错误；CHECK_FIELD 通过 `slots[ins.b]` 即时检查动态值及提升；BUILD_STRUCT 按映射放入声明槽。GET_FIELD/SET_FIELD 使用字段表，先核对 `program.schemas[id].get() == &schema` 和全部编号边界。查询使用已有 `program.names[name]`，不临时复制字符串。显示、复制及 GC 仍使用原对象和 Schema。
- [ ] **4. 验证错误时序与副作用。** 新增 `execution_index_deferred_errors`，逐一覆盖未知/不可构造类型、缺失/未知/重复字段：`--check` 成功、未执行分支正常；实际执行先保留 `write("before")`，再失败，任何字段的 `write("field")` 均不执行。有效集合中首个字段类型失败后，后续字段不求值；错误位置仍归失败字段初始化。复用既有 `objects-field-set-check`、`objects-field-set-precheck` 等用例。
- [ ] **5. 覆盖资源与 GC。** 在字段表、已访问数组、槽映射和后续单元准备处注入分配失败，要求无部分索引发布、退出 3且可清理；强制 GC 下验证嵌套对象字段、结构返回和共享字段。独立 Heap/显示用的聚合 Schema、手造 CodeUnit/Program 的原始核验测试继续通过，不新增准备调用要求。
- [ ] **6. 回归并提交。** 运行新增两个命名用例、`vm_objects`、`vm_objects_gc`、`object_result_roots`、`bytecode_verify`、objects 黑盒组和默认黑盒。只为既有清单缺少的多单元/多 Schema 组合新增固定 MYB 用例与独立输出，新增 ID 同步汇总到默认清单。通过后提交 `fix: 预计算结构字段匹配与写入槽位`。

准备阶段的平均工作量随名称、声明字段及构造字段总量线性增长，另计文本字节处理。运行期 CHECK_STRUCT 为 O(1)，CHECK_FIELD 为 O(1)，BUILD_STRUCT 为 O(F)；字段表查询为平均 O(1)，不将 unordered_map 的最坏情况宣称为常数。

`execution_index_deferred_errors` 至少包含以下精确断言，证明预计算没有提前报告动态错误：

```cpp
VmFixture f;
const auto source = "struct Pair{left:Int64;right:Int64;}write(\"before\");"
                    "Pair{left:write(\"field\"),left:1};";
const auto checked = f.check(source);
REQUIRE(checked.exit_code == 0 && checked.stdout_bytes.empty());
const auto run = f.run(source);
REQUIRE(run.exit_code == 1 && run.stdout_bytes == "before");
REQUIRE(run.failure->reason == ReasonCode::Type);
```

## 任务 3：修复 B03 入口失败的绝对路径诊断

**Files:** 修改 `src/platform/windows_io.cpp`、`tests/cli/test_cli.py`、`tests/internal/host_io_cases.cpp`、`test_hooks.hpp`。HostIO、EntryBytes、Session 和诊断接口保持不变。

**Interfaces:** `WindowsHostIO::read_entry(std::string_view)` 返回原有 EntryBytes；发生入口 IoFailure 时，消息选择已提交的绝对路径文本，尚未取得时使用原参数。

- [ ] **1. 增加并确认失败测试。** CLI 新增 `test_missing_source_reports_absolute_path`、`test_missing_source_is_resolved_per_cwd`，在中文空格目录下分别运行普通模式和 `--check`。两个 cwd 使用同一缺失文件名，各自诊断须包含对应绝对路径；原实现应失败。固定期望：退出 2、stdout 空、无虚构 `:行:列:` 和调用链；只在路径断言中统一两种分隔符，保留 stderr 原字节。
- [ ] **2. 缓存成功解析的路径。** 在 `try` 外持有 `std::string absolute_path_utf8`；两次 GetFullPathNameW、长度检查和严格 UTF-8 转换完成后提交缓存，再调用 CreateFileW。成功时把缓存移入 EntryBytes。catch 的首项动作仍为记录 EntryIO 首错和 OS 代码，随后复用缓存或原参数拼消息；不在 catch 再解析或转换路径，控制字符交给原诊断转义。
- [ ] **3. 验证入口故障链。** 内部新增 `host_io_entry_failure`，对相对现有文件注入 OsRead/ERROR_READ_FAULT 和 OsClose/ERROR_INVALID_HANDLE，要求绝对路径、原 OS 代码、无 SourceSpan；关闭失败清理后能再次独占打开。补充仅 Debug 的 `EntryPathBuffer`、`EntryPathText`、`EntryDiagnostic` 位置：前两处首次分配失败为 Resource/3；最后一处位于原 I/O 首错记录后，消息分配失败仍保留 EntryIO/2。通过真实 Session 边界和 stderr 句柄验证退出码，不能仅检查直接调用的异常头。
- [ ] **4. 验证回退与既有行为。** `EntryPathBuffer`、`EntryPathText` 在缓存提交前同时消费 `TestHooks::fail_os_next`：前者注入 IO/ERROR_INVALID_NAME，后者注入 Encoding/ERROR_NO_UNICODE_TRANSLATION；均抛原 IoFailure 并验证回退原参数、退出 2。同位置的 `hit` 仍单独覆盖首次分配失败。正常读取后触发前端错误和 `print(1/0)`，绝对源码路径及真实行列保持正确。含 LF、Tab、U+2028 的错误名称只产生一条概要物理行；stderr 不可写仍保留退出类别。
- [ ] **5. 回归并提交。** 运行 `host_io_entry_failure`、原 `host_io`、诊断/首错内部用例和两配置 CLI；切换配置时按构建文档重新设置 `MYB_INTERPRETER`。全部通过后提交 `fix: 在入口读取失败时保留绝对路径`。

## 任务 4：每个 PR 必须通过的两项性能回归

**Files:** 新增 `tools/check_performance.py`、`tests/driver/test_check_performance.py`；扩展 `execution_index_cases.cpp`、`test_hooks.hpp/.cpp`；修改 `tools/ci.ps1`、`.github/workflows/ci.yml`。本任务将原 `reproduce.py` 按现有字节纳入交付，新驱动按固定仓库路径导入其生成函数，不调用 `main`。

**Interfaces:** `check_performance.py --interpreter <当前Release exe> --output <结果目录>`；一次运行完整报告 B01（内置及后部用户函数）、B02 两项结果。全部通过退出 0；性能超限、缺项、零用例、错误输出或超时退出 1；参数错误或重测后仍不稳定退出 2。结果写入 `build/ci-logs/release/performance/`，并沿用当前必需检查 `windows-debug`、`windows-release`。

- [ ] **1. 增加 Debug 工作量回归。** 新增 `execution_index_call_scaling`、`execution_index_field_scaling`。B01 固定调用次数，对比 0/128/1024 个无关函数，分别覆盖内置和后部用户函数；准备期允许随声明增长，执行期候选检查数不得随无关函数线性增长。B02 固定构造次数，对比 64/128/256 字段，准备期和每轮构造的实际处理量使用线性上界；覆盖字段匹配、重复检查、CHECK_FIELD、BUILD_STRUCT 和字段槽解析。计数放在实际候选访问或比较位置，不能只数查询接口调用；加入非空输入的计数有效性断言，防止全零假通过。统计仅在内部测试构建启用，原有调用数量、结果及副作用仍需核对。
- [ ] **2. 增加 Release 规模计时。** 新驱动只运行当前提交的 Release；B01 对比 0/1000/5000 个无关函数，B02 对比 256/512/1024 字段。预热后至少测量 5 轮，同时记录总耗时与 `--check` 耗时，全部输出先校验。沿用任务 5 的 E 定义、100 ms 最短有效估计时长和 20% 波动公式；该项各规模统一循环数，按最小规模自动校准，最多重新校准或重测 2 次，每项总预算 180 秒。要求 B01 两类目标的 `E(5000)/E(0) ≤ 1.5`，B02 的 `E(1024)/E(256) ≤ 6`。预算耗尽、不稳定或数据非有限均不能通过；保留全部轮次，不挑选最快值。B01 失败后继续收集 B02，最终统一返回结果。每个 PR 不依赖本地旧 EXE，也不要求重新构建旧缺陷版本。
- [ ] **3. 验证驱动自身能检出问题。** 驱动自测覆盖正常结果、旧式线性/平方增长、正确输出但性能超限、零计数或零用例、B01/B02 缺项、超时、非有限数据、持续波动和预算耗尽。分别验证只有 B01 失败、只有 B02 失败都会返回非零，且另一个项目仍有记录。用原缺陷参考版验证新回归能检出两项旧问题；修复后的当前版全部通过。
- [ ] **4. 接入每次 PR 的必需 CI。** `windows-debug` 的全内部测试自动包含两项工作量回归；`tools/ci.ps1` 的 Release 分支在功能回归后通过 `Invoke-CiCommand` 调用 `check_performance.py`。保持现有 job 名称和 dev/main PR 触发，不按路径跳过，不设置 `continue-on-error`；任何非零退出使对应必需检查失败。上传步骤使用 `always()`，保留两项规模、当前提交、EXE/工具信息、阈值及原始结果；结果上传成功不替代测试通过。核对 dev/main 合并要求仍覆盖两个既有检查。
- [ ] **5. 证明失败确实阻止合并。** 在验证 PR 的临时提交中先单独使 B01 判据失败，确认必需检查失败且不可合并；恢复后再单独使 B02 失败，重复确认；最后恢复正式判据并取得最新提交的两项通过记录。保存三类结果与工作流链接，最终提交不保留临时失败设置。仅文档 PR 同样确认两个性能项目均被执行；没有结果或未实际运行不能记为通过。
- [ ] **6. 回归并提交。** 两配置 CI、驱动自测、实际失败/恢复验证全部完成后，提交 `test: 将两项性能回归加入每次PR检查`；验收证据随任务 5 汇总。若远端验证未完成，保留本任务未完成状态。

实施后的本地等价入口如下；当前仅列入计划：

```powershell
& ./build/debug/myb_internal_tests.exe --case execution_index_call_scaling
& ./build/debug/myb_internal_tests.exe --case execution_index_field_scaling
python tools/check_performance.py --interpreter build/release/myb.exe --output build/ci-logs/release/performance
```

## 任务 5：严格性能对照与最终验收

**Files:** 新增 `tests/audit/20261027/verify_fixes.py`、`docs/reports/MYB缺陷修复验收报告.md`；同步设计、需求覆盖表和本计划状态。复用任务 4 已纳入交付的原 `reproduce.py`，将原审计报告按现有字节纳入实施交付，保持历史观察含义。本轮仅编写计划，不进行该提交。

- [ ] **1. 建立严格对照入口。** 新脚本导入原 `reproduce.py` 的生成函数，不调用其 `main`。支持 `--before <Release exe>`、`--after <Release exe>`、`--after-debug <Debug exe>`、`--output <新目录>`、`--rounds`、`--calls`、`--constructions` 和 `--timeout`；Debug 参数只用于 B03，不参加计时比较。拒绝覆盖既有结果。记录审计日期、实际运行时间、提交、所有 EXE/DLL 的 SHA-256、程序字节及逐轮记录。先把 `--after` 指向旧程序，分别确认 B01、B02、B03 判据均得到失败状态，保留全部子结果，不在首项失败后跳过其他检查。
- [ ] **2. 验证全部正确性与 PR 性能检查。** 按构建文档准备 PATH，完成下面两个 CI 命令，要求退出 0、正式测试全部通过、任务 4 两项性能回归均执行并通过、Release 不含内部测试依赖。实际计数随新增测试记录，不能沿用旧的 76/372。新 CLI 与组合黑盒须被默认入口发现。

  ```powershell
  pwsh -File ./tools/ci.ps1 -Configuration Debug -Msys2Root D:/msys64
  pwsh -File ./tools/ci.ps1 -Configuration Release -Msys2Root D:/msys64
  git diff --check
  ```

- [ ] **3. 串行测量两份 Release。** 使用任务 1 保留的独立参考 EXE/DLL 与新 Release，暂停 CI 和其他压力任务；程序字节、循环数、机器、工具信息相同。每个规模先预热，再至少 5 轮交替运行修复前/后 EXE，同时保留 `--check` 和总耗时。测试 B01 的 0/1000/5000 个无关函数及内置/后部用户函数，B02 的 256/512/1024 字段。原始输出全部正确后才比较时间；中位数差继续称为“估计执行”。
- [ ] **4. 应用验收判据。** 定义 `E = median(总耗时) - median(--check耗时)`。B01 两类目标均要求修复后 `E(5000)/E(0) ≤ 1.5`；B02 要求 `E(1024)/E(256) ≤ 6`；各自大规模修复后 E 不超过同条件旧版本的 25%。用于比值的 E 至少 100 ms；每组正常执行的波动定义为 `(最大值-最小值)/中位数`。E 更短、非正或波动超过 20% 时，本组记为待复测，两版本同步扩大循环数或消除竞争后重跑。B03 对 `--after`、`--after-debug` 均严格要求绝对路径、退出 2、无伪造位置和链。保存所有子结果后，全部通过退出 0，存在失败退出 1，仅存在待复测或参数错误退出 2。静态复核同时确认热路径没有相应线性全表扫描或重复平方匹配。
- [ ] **5. 独立复核并更新记录。** 检查自定义 Registry、动态错误阶段、首错/GC/清理、手造产物兼容、默认测试接入及性能方法。验收报告保存两配置计数、修复前后原始记录路径、比值、PR 两项性能检查的失败/恢复与合并拦截证据及未完成项；设计补充执行索引归属及寿命，需求覆盖表关联新增用例。原审计数据保留原日期和原结论，通过新报告链接说明修复状态。完成文档链接、用词和差异格式检查后再勾选任务，提交 `test: 补充三项缺陷修复验收与记录`，包含新严格脚本及其复现依赖。

严格对照的预定命令如下；脚本由本任务新增，当前尚不能执行：

```powershell
python tests/audit/20261027/verify_fixes.py `
  --before build/bugfix-20261028/reference-release/myb.exe `
  --after build/release/myb.exe `
  --after-debug build/debug/myb.exe `
  --output build/bugfix-20261028/comparison `
  --rounds 7 --calls 1000000 --constructions 3000 --timeout 180
```

## 当前交付与实施完成标准

本轮交付此计划及前序计划中的入口链接，并按用户确认补充每次 PR 必过的两项性能回归；不修改解释器、测试或工作流实现。补充后检查通过：两份计划的 27 个本地链接有效，禁用词零命中，差异格式检查通过；独立复核确认必过 CI 要求和任务编号一致。

实施完成需同时满足：每个 PR 必跑 B01/B02 性能回归且任一失败均阻止合并，B01/B02 的严格性能对照通过，B03 的两配置路径断言通过，Debug/Release 全量 CI 通过，独立复核无未处理阻塞问题，正式验收报告保存实际结果。任一项未通过即保留对应未完成状态。
