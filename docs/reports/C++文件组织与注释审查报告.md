# C++ 文件组织与注释审查报告

日期：2026-10-24。审查分支：`codex/myb-m2`。审查代码版本：`4e30d63`。

本报告供维护者确定头文件实现的拆分范围，以及实现说明的补充范围。逐文件审查 `src/` 的 66 个文件与 `tests/internal/` 的 21 个文件，共 **87 个手写 C++ 文件：41 个 `.hpp`、46 个 `.cpp`**。生成目录 `build/`、第三方源码 `third_party/` 不纳入修改建议；`.l`、`.y` 仅用于交叉核对前端接口声明和生成边界。

## 1. 结论

| 范围 | 建议移出头文件 | 可选整理头文件 | 适合保留头文件 | 应补注释的 cpp | 可选补注释的 cpp | 本轮未发现明确注释缺口的 cpp |
|---|---:|---:|---:|---:|---:|---:|
| 产品源码 `src/` | 1 | 0 | 37 | 6 | 3 | 19 |
| 内部测试 `tests/internal/` | 0 | 1 | 2 | 7 | 2 | 9 |
| 合计 | **1** | **1** | **39** | **13** | **5** | **28** |

明确的头文件拆分项为 `src/frontend/flex_allocations.hpp` 中的 `allocate`、`release`、`reallocate`。`tests/internal/test_support.hpp` 的 `require` 只是可选整理项。

应优先补充产品实现说明的文件为：`src/bytecode/compiler.cpp`、`src/bytecode/verify.cpp`、`src/vm/vm.cpp`、`src/semantic/checker.cpp`、`src/source/source.cpp`、`src/runtime/heap.cpp`。测试辅助接口与测试数据的说明另列于第 5 节。

这些结论属于维护性审查。头文件中的类内定义在当前 C++ 编译方式下合法；“建议移出”表示文件组织可以改进。“应补注释”表示关键约束需要写入代码，不表示该段功能已经被判定为错误。

## 2. 判定方法

- **建议移出头文件**：普通非模板函数承载独立的分配、链表维护、算法或异常安全流程，其他编译单元不需要看到函数体。
- **适合保留头文件**：类型定义、接口声明、需要在使用处可见的模板定义、简短构造、访问器和简单状态维护。简短转发调用本身可以保留，即使被调用函数可能分配或抛异常。
- **可选整理**：能够移出，但当前小型组织方式也合理，不列为明确缺陷。
- **应补实现注释**：算法选择、检查顺序、根交接、失败清理或测试构造依据需要读者跨函数推导。补普通中文注释解释原因和约束。
- **应补接口注释**：仅在 `.cpp` 定义的复杂辅助接口或核心类型缺少必要契约；使用 Doxygen。已有说明需要扩充时直接扩充原注释。
- **不重复已有说明**：头文件已有 Doxygen 时，不在 `.cpp` 再抄一份。直观赋值、转发、枚举分派和能直接读懂的测试断言不要求逐行注释。

模板也可以只定义在 `.cpp`：当它仅在该编译单元内实例化时，定义放在那里即可。例如 `tests/internal/heap_cases.cpp` 的 `resource_failure` 不需要为了统一形式迁入头文件。判断依据是定义的实际使用范围。

项目规则依据：[AGENTS.md](../../AGENTS.md) 的“代码注释规范”；词表与文档格式依据：[文档规则](../rules/文档构建原则.md) 和 [禁用词表](../forbidden_terms.yaml)。本轮先盘点所有文件，再按三个独立产品模块组与一个测试组逐文件阅读，结合声明、定义、调用与包含关系作判断；注释数量仅辅助定位。

## 3. 头文件的拆分建议

### src/frontend/flex_allocations.hpp：建议移出

- **`FlexAllocationLedger::allocate`**，[src/frontend/flex_allocations.hpp:41](../../src/frontend/flex_allocations.hpp#L41)（第 41–63 行）。非模板实现同时处理容量上限、测试故障转译、malloc 失败、头插双向登记和存活计数；53～62 行包含 malloc、prev/next 更新和返回用户区域。scanner.hpp 按值持有此类型，生成词法器、scanner.cpp、driver.cpp 及直接包含该头的内部测试均会见到这段普通实现，调用方不依赖函数体可见性。

  建议：头文件保留现有 Doxygen 与方法声明；将函数体移至拟新增 src/frontend/flex_allocations.cpp，并将该文件加入 myb_core 源文件列表。测试故障头的包含可随实现移入该 .cpp。保留类布局、简单构造和 live_total 访问器。

- **`FlexAllocationLedger::release`**，[src/frontend/flex_allocations.hpp:69](../../src/frontend/flex_allocations.hpp#L69)（第 69–81 行）。非模板释放路径从用户指针回退到块头，分别修复前驱、表头与后继，再 free 并减计数；72～80 行构成与 allocate 配套的链表算法，不只是成员访问或小型状态赋值。现有类内定义合法，此项属于实现组织建议。

  建议：将函数体与 allocate 一并移至拟新增 src/frontend/flex_allocations.cpp，声明处继续保留有效指针前提、指针失效与 noexcept 约定。

- **`FlexAllocationLedger::reallocate`**，[src/frontend/flex_allocations.hpp:90](../../src/frontend/flex_allocations.hpp#L90)（第 90–98 行）。虽只有少量语句，仍负责先 allocate、再按 min(旧尺寸, n) 复制、最后 release 的分配和异常安全流程；94～96 行的顺序保证分配失败时旧块仍在表中。这是普通非模板实现，跨翻译单元调用无需可见函数体。

  建议：移至拟新增 src/frontend/flex_allocations.cpp，与 allocate/release 集中维护；保留声明处失败后旧块有效的契约，并可在实现内简短解释先分配后释放的顺序。clear、析构和 live_total 均为短小无额外分配的调用或访问，可继续留在头文件。

### tests/internal/test_support.hpp：可选整理

- **`require`**，[tests/internal/test_support.hpp:26](../../tests/internal/test_support.hpp#L26)（第 26–32 行）。普通非模板函数在头文件中构造ostringstream和异常文本；目前测试支持刻意集中于小型头文件，尚不构成明确拆分缺陷。

  建议：若希望测试支持层也统一声明/实现分离，可将require移到新增test_support.cpp并登记CMake；宏、类型及简短登记构造可保留。

拆分 `flex_allocations.hpp` 时，需要新建 `src/frontend/flex_allocations.cpp` 并登记到 CMake 的 `myb_core`。现有接口 Doxygen 留在头文件；`clear`、析构、`live_total` 的短小转发或访问可以保留，也可以随统一风格移出。无需改变分配协议或对外接口。

适合保留的代表：`ast/factory.hpp` 的模板工厂、`ast/node.hpp` 的 `ReadOnlyNodes<T>`；`ast/nodes.hpp` 的只读访问器；`diagnostic/failure.hpp` 的简短首错记录；`runtime/roots.hpp` 的根槽访问与转发。这些文件的具体结论见第 7 节。

## 4. 产品 cpp 的注释补充项

以下按文件列出。标为“可选”的条目用于进一步改善阅读，不计入该文件的必要补充项。

### src/bytecode/compiler.cpp：应补注释

- **应补，实现注释：`BytecodeCompiler::compile`**。位置：[src/bytecode/compiler.cpp:27](../../src/bytecode/compiler.cpp#L27)（第 27–69 行）。

  依据：先遍历顶层项目创建全部 StructSchema，再遍历 structures 填充字段；结构字段的 schema_id 在第二遍从完整结构集合中查找。现有声明只说明生成完整程序，未说明拆成两遍的原因。

  建议：在两遍交界处补普通实现注释：先固定全部结构的身份与编号，再解析字段中的结构引用，因此字段可引用源码后方声明的结构；字段解析期不再追加 schemas。

- **应补，实现注释：`BytecodeCompiler::visit(const LetStmt &)`**。位置：[src/bytecode/compiler.cpp:255](../../src/bytecode/compiler.cpp#L255)（第 255–267 行）。

  依据：局部名称先写入 scopes_ 并发出 DECL_LOCAL，随后才生成初始化表达式；这个顺序使 let 自读绑定到新建的未初始化局部，而声明前的读取仍可绑定外层。现有 binding 声明只说明按当前环境解析，未解释这里修改环境的时机。

  建议：在 scopes_.back().emplace 前补普通实现注释，说明声明点在初始化表达式之前生效，初始化期间的同名读取须报告未初始化，不能继续读取外层绑定。

- **应补，实现注释：`BytecodeCompiler::visit(const ForStmt &)`**。位置：[src/bytecode/compiler.cpp:329](../../src/bytecode/compiler.cpp#L329)（第 329–350 行）。

  依据：ITER_NEXT、局部区清理、ITER_END 和 break 修补共同形成循环退出协议；continue 另在第 365–368 行跳回 ITER_NEXT。仅从 emit 次序难以看出为什么 break 必须落在 ITER_END，而 continue 必须保留活动遍历。

  建议：在循环回边与退出点附近补普通实现注释：每轮清理使循环变量及体内局部恢复未声明状态；continue 清理局部后进入下一轮；耗尽与 break 共用 ITER_END，以解除一次遍历登记。说明 active_iterations_ 同时供不可达语句核验保留词法遍历上下文。

- **可选，实现注释：`BytecodeCompiler::visit(const UnaryExpr &)`**。位置：[src/bytecode/compiler.cpp:181](../../src/bytecode/compiler.cpp#L181)（第 181–187 行）。

  依据：可选：checked_->literals 中记录 UnaryExpr 时直接发出常量并跳过子表达式。这条特殊路径对应 Checker 为直接负最小整数建立的规范化记录；原因需跨模块追踪，Checker 已在其实现处说明。

  建议：可在 contains 分支补一句实现注释，说明复用已规范化的 Int64 最小值，跳过不能独立表示的正幅值孩子；保持具体识别规则由 Checker 负责。

- **可选，实现注释：`BytecodeCompiler::visit(const LogicalExpr &)`**。位置：[src/bytecode/compiler.cpp:200](../../src/bytecode/compiler.cpp#L200)（第 200–208 行）。

  依据：可选：DUP 后的条件跳转只消费副本，短路路径保留左值，继续求值路径先 DROP 左值再检查右值。操作码说明已给出各自栈效应，组合目的仍需读者推导。

  建议：可补一句实现注释，说明复制条件是为了让短路和右侧求值两条路径最终都留下恰好一个严格 Bool。

### src/bytecode/verify.cpp：应补注释

- **应补，实现注释：`verify(CodeUnit &, EmergencyFailure &) / seed、analyze`**。位置：[src/bytecode/verify.cpp:127](../../src/bytecode/verify.cpp#L127)（第 127–145 行）。

  依据：seed 对同一 ip 的再次到达要求整个 State 完全相等，只有首次到达才入队；这决定了工作队列算法为何无需反复求取更大的栈深，也保证调用、构造和遍历协议在合流处一致。现有第 30 行与第 300 行注释说明了检查阶段，未说明此核心合流规则。

  建议：在 states/work 或 seed 定义前补普通实现注释：每条到达同一指令的路径须具有相同栈深及未完成协议；出现差异属于生成错误，不能用最大深度合并；一致的状态只需分析一次。

- **应补，实现注释：`verify(CodeUnit &, EmergencyFailure &) / 不可达片段分析`**。位置：[src/bytecode/verify.cpp:286](../../src/bytecode/verify.cpp#L286)（第 286–305 行）。

  依据：入口可达片段分析完成后，语句入口以零操作数深度和 boundary_iterations 恢复词法遍历栈，再处理无入口记录的剩余片段。声明已说明不可达片段不计入 max_operand，但未解释为何不可达语句不能一律从空遍历栈开始。

  建议：在语句入口循环前补普通实现注释，说明 return/break 等转移后的语句仍可能位于 for 的词法范围内，需恢复生成器记录的遍历栈，才能核验后续 ITER_NEXT/ITER_END；最终的空状态入口用于缺少语句入口记录的剩余片段。

- **应补，接口注释：`State`**。位置：[src/bytecode/verify.cpp:9](../../src/bytecode/verify.cpp#L9)（第 9–15 行）。

  依据：仅在 cpp 中定义的核心核验状态已有 @brief，但 calls 的 pair 与 constructions 的 tuple 没有成员名；其第二、第三分量分别参与第 196–223 行的深度与字段次序检查，约束无法从类型本身识别。

  建议：扩充现有 Doxygen，逐项说明 calls=(调用点号, PRECALL 时深度)，constructions=(构造方案号, 下个待检查字段序号, CHECK_STRUCT 时深度)，iterations 为按嵌套次序保存的活动站点。保留现有注释并补契约，无需重复写在 verify.hpp。

### src/runtime/heap.cpp：应补注释

- **应补，实现注释：`Heap::before_growth`**。位置：[src/runtime/heap.cpp:55](../../src/runtime/heap.cpp#L55)（第 55–68 行）。

  依据：第64行通过 !failure_.is_set() 在首错已经记录时禁止压力回收，即使 force 为 true 也跳过 collect。heap.hpp 第109—113行说明增长预检和发布期间的 GC 约束，第66行注释说明大请求阈值，均未解释首错状态为何参与回收判定。解释器设计第751行要求记录首错后不为清理触发 GC；这是该检查顺序中的独立约束。

  建议：在第64行前补普通实现注释，说明首错后会话进入终止和清理流程，后续增长检查不得再启动 GC，并说明该规则也覆盖强制回收测试模式。保留现有接口 Doxygen，无需在定义处重复。

### src/semantic/checker.cpp：应补注释

- **应补，实现注释：`Checker::visit(const AssignStmt &)`**。位置：[src/semantic/checker.cpp:209](../../src/semantic/checker.cpp#L209)（第 209–218 行）。

  依据：210～215 行只剥最外层 GroupExpr 并先验证目标最终为 NameExpr/FieldExpr/IndexExpr，216～217 行在形状合法后先访问右侧、再访问原始目标；该检查顺序及保留原树的理由没有普通实现说明，AssignStmt 的类型说明仅说明保存字段。

  建议：在目标形状检查前补普通注释：只剥最外层分组识别写入形状，保留原始树供后续遍历；形状非法时直接拒绝，合法后按右侧再目标的顺序检查子树，这里不读取绑定或运行值。说明实际检查顺序，避免写成求值实现。

- **可选，实现注释：`Checker::visit(const ForStmt &)`**。位置：[src/semantic/checker.cpp:240](../../src/semantic/checker.cpp#L240)（第 240–247 行）。

  依据：可选：source() 的遍历先于循环变量作用域的建立，随后调用 block_contents 而非 body().accept；checker.hpp 已说明 block_contents 用于共享作用域，FunctionDecl 分支也已有同类说明，此处可补一句连接来源检查与作用域建立的顺序。

  建议：可写明先在外层上下文检查来源表达式，再让循环变量与循环体最外层共享一个声明集合；使用 block_contents 避免为循环体再开一层。

### src/source/source.cpp：应补注释

- **应补，实现注释：`SourceManager::locate`**。位置：[src/source/source.cpp:56](../../src/source/source.cpp#L56)（第 56–59 行）。

  依据：按行内码点计列时，decode_one 失败或目标偏移落在码点内部都会直接 break 并返回当前列，而非再报告编码错误；已有 60 行注释仅说明 CRLF。add 的声明说明编码失败仍保留记录，因此定位函数不能把整条记录已经通过 UTF-8 验证作为前提，这一宽容停止条件需要在实现处说明。

  建议：在行内解码循环处补普通注释：诊断可能定位保留的非法源码记录，只累计目标之前完整合法的码点；遇非法序列或目标落在多字节内部时保留当前列，供首错位置输出。现有 CRLF 注释继续保留。

### src/vm/vm.cpp：应补注释

- **应补，实现注释：`VM::execute / 每轮活动帧借用`**。位置：[src/vm/vm.cpp:167](../../src/vm/vm.cpp#L167)（第 167–188 行）。

  依据：current 是 frames_.back() 的引用，need/local/drop 按引用捕获它；第 292–299 行的 CALL 可使帧表增长，RETURN 会移除当前帧。当前实现随后结束本轮并重新取得帧引用，这一有效期约束没有在借用处说明。VM 类注释给出跨增长保存索引的总则，不能直接解释本轮引用何时停止使用。

  建议：在取得 current 附近补普通实现注释：本轮帧引用及捕获它的辅助函数只在帧表变动前有效；CALL/RETURN 后直接进入下一轮并重新取帧，后续不得在同一分支使用这些借用。

- **应补，实现注释：`VM::execute / Op::BINARY`**。位置：[src/vm/vm.cpp:250](../../src/vm/vm.cpp#L250)（第 250–258 行）。

  依据：ValueOps::binary 可能创建受管结果。调用时两个输入仍在有效操作数范围内，结果先由 RootedValue 接管，再写入左槽，最后 drop 右槽。声明处介绍了 VM 根与 native 调用交接，但没有说明此受控分配指令的输入根和结果发布顺序。

  建议：在 BINARY 分支补普通实现注释：运算期间保留两个输入为操作数根；结果经临时根接收，写回仍有效的左槽后再收缩栈，交接阶段不增长 VM 存储或触发 GC。

### src/cli/session.cpp：可选补注释

- **可选，实现注释：`Session::run / Failure 处理`**。位置：[src/cli/session.cpp:70](../../src/cli/session.cpp#L70)（第 70–76 行）。

  依据：可选：保存首错后，仅在记录缺少 span 且 domain/reason 与捕获异常相同时补位置。首错保存及错误映射在声明和相关类型中已有说明，这里的同类检查原因仍可更直接地表达。

  建议：可在补 span 的条件前加一句普通实现注释，说明保留已记录的首错类别与 OS 信息，仅从同类捕获异常补齐缺少的位置。

### src/frontend/scanner.cpp：可选补注释

- **可选，实现注释：`ScannerContext::escape`**。位置：[src/frontend/scanner.cpp:65](../../src/frontend/scanner.cpp#L65)（第 65–87 行）。

  依据：可选：Unicode 转义分支直接使用 i = 3、i + 1 < text.size()、十六进制移位和四种 UTF-8 前缀；声明已说明词法形状与非法标量检查，实现处仍可用一条普通注释连接这些索引与位运算的目的。现有契约足以确定输入前提，不属于缺接口注释。

  建议：可在幅值循环前说明跳过已由词法规则确认的 \u{ 前缀与末尾右花括号，累积十六进制幅值；在输出前说明先验证标量，再按码点上界选择最短 UTF-8 宽度。避免逐个解释掩码语句。

- **可选，实现注释：`flex_fatal`**。位置：[src/frontend/scanner.cpp:90](../../src/frontend/scanner.cpp#L90)（第 90–99 行）。

  依据：可选：分类依据 Flex 后端文本中的 memory/alloc、overflow/capacity，条件同时命中时 Capacity 优先；声明已覆盖 Resource/Internal 契约，但实现对文本关键词的依赖及优先顺序只体现在 strstr 和三元表达式中。

  建议：可补一条普通注释说明这里适配 Flex 后端错误文本，按容量优先、其次分配、其余内部约束分类，便于调整后端版本时核对关键词。无需重复函数的 @throws。

### src/runtime/decimal_codec.cpp：可选补注释

- **可选，实现注释：`DecimalCodec::parse_int`**。位置：[src/runtime/decimal_codec.cpp:95](../../src/runtime/decimal_codec.cpp#L95)（第 95–120 行）。

  依据：第104—106行先完整检查剩余字符，再于第111—115行累计幅值并判断范围。两遍扫描决定了超长数字前缀带非法尾字符时先返回 Content；现有第107行注释只解释无符号幅值及最小负整数，未解释两遍扫描的选择。

  建议：可在第104行前加普通实现注释，说明先排除文本内容错误，再进行范围累计，保留当前 Content/Range 分类顺序。无需重复 decimal_codec.hpp 的数值范围和返回类型说明。

`verify.cpp::State` 已有 `@brief`，需要补的是元组各分量的含义及协议约束，不能描述成完全没有注释。`driver.cpp` 的 `yylex` 声明注释实际位于 `parser.y`，也不列为缺少接口说明。

## 5. 内部测试 cpp 的注释补充项

测试脚本与预期已经能直接说明普通语义时，不再要求重复写说明。以下聚焦测试辅助接口、固定数值和容量布局的隐含依据。

### tests/internal/decimal_cases.cpp：应补注释

- **实现注释：`parse_cases / format_cases / 固定种子回读`**。位置：[tests/internal/decimal_cases.cpp:41](../../tests/internal/decimal_cases.cpp#L41)（第 41–84 行）。

  依据：长十进制文本和位模式缺少取值依据；仅由数值外观难以识别中点取偶与显示阈值的覆盖。

  建议：分组说明两个精确中点、2^53附近、最大有限/最小次正规数及阈值邻居；说明固定种子回读用于补充固定期望。

### tests/internal/fixtures.cpp：应补注释

- **接口注释：`MemoryHostIO / counted_print / counted_write / VmFixture::State`**。位置：[tests/internal/fixtures.cpp:5](../../tests/internal/fixtures.cpp#L5)（第 5–46 行）。

  依据：文件内定义的测试宿主和计数回调没有说明替代范围与计数用途；使用静态描述表序号转发还隐含print/write顺序。

  建议：在仅定义处补中文Doxygen：宿主返回EOF并计数、不模拟真实文本文件；回调借用同一NativeContext/参数/输出根，只增加计数再转发；State持有真实Session与借用服务。

### tests/internal/frontend_cases.cpp：应补注释

- **实现注释：`ast_contract 高度超限后的所有权`**。位置：[tests/internal/frontend_cases.cpp:18](../../tests/internal/frontend_cases.cpp#L18)（第 18–26 行）。

  依据：测试先把e以std::move传入工厂，再要求失败后e仍可读，依赖工厂在转移所有权前检查高度。

  建议：解释该断言在验证失败前不消费子树；不是随意在move后继续使用对象。

- **实现注释：`ast_dispatch_and_cleanup 高度254/256`**。位置：[tests/internal/frontend_cases.cpp:166](../../tests/internal/frontend_cases.cpp#L166)（第 166–173 行）。

  依据：254层表达式与256上限之间的差值依赖ExprStmt和AstProgram两层。

  建议：注明根到叶高度包含表达式语句与程序根，避免维护时把254误改为256。

### tests/internal/heap_cases.cpp：应补注释

- **接口注释：`trace_values / resource_failure`**。位置：[tests/internal/heap_cases.cpp:16](../../tests/internal/heap_cases.cpp#L16)（第 16–31 行）。

  依据：测试根回调直接借用vector并枚举当前元素，宿主容器变化和登记有效期是GC测试成立的前提；辅助函数仅定义在本文件。

  建议：在定义处补Doxygen，明确context为有效vector、回调只枚举现有元素且不修改集合；说明resource_failure要求受控错误已登记首错。保留已有场景说明。

### tests/internal/native_handoff_cases.cpp：应补注释

- **接口注释：`make_text / echo`**。位置：[tests/internal/native_handoff_cases.cpp:37](../../tests/internal/native_handoff_cases.cpp#L37)（第 37–48 行）。

  依据：两个回调仅在 cpp 定义并承担主动 GC 验证，目前只有行内说明，没有中文 Doxygen 职责概述。NativeFn 的头文件已经给出通用参数和输出根协议，现有行内注释也已说明 out 先成根、echo 仅依靠 VM 实参区保活。

  建议：在定义前补中文 @brief，分别说明 make_text 验证新建值在 out 发布后经回收存活，echo 验证只有 VM 实参区时仍可保活并返回同一值。通用生命周期沿用 NativeFn，已有根交接原因无需重复。

### tests/internal/source_cases.cpp：应补注释

- **实现注释：`source_positions 字节偏移与码点列`**。位置：[tests/internal/source_cases.cpp:8](../../tests/internal/source_cases.cpp#L8)（第 8–18 行）。

  依据：含BOM、中文、Tab、emoji、CRLF/CR/LF的字节串直接配偏移7/13/15/17，缺少字节与码点列的对应说明。

  建议：用一条分段注释标出BOM不计列、中文和emoji各占一列、Tab一列、CRLF一次换行；说明这些偏移对应的边界。

### tests/internal/vm_failure_cases.cpp：应补注释

- **实现注释：`vm_call_failure_values / states / frames`**。位置：[tests/internal/vm_failure_cases.cpp:211](../../tests/internal/vm_failure_cases.cpp#L211)（第 211–218 行）。

  依据：值区通过 target 的局部区触发增长已有第123行说明；剩余的深度14与初始帧容量16的对应关系没有写出，单看三个调用难以判断故障为何落在帧表准备。

  建议：在 VmFrames 用例旁补充：recurse(0) 到 recurse(14) 共15层，再加顶层已占16个活动帧，target 需要第17帧。深度2仅为值区和状态区用例选择较浅递归，不必重复已有值区增长说明。

### tests/internal/checker_cases.cpp：可选补注释

- **实现注释：`frontend_failure_cleanup_and_locations 固定位置表`**。位置：[tests/internal/checker_cases.cpp:198](../../tests/internal/checker_cases.cpp#L198)（第 198–221 行）。

  依据：输入与精确偏移已直接配对，但多类错误混放同一数组。

  建议：可按非法字符、尾逗号、转义、EOF和多字节字符补分组注释，现有逐项断言已足以说明结果。

### tests/internal/toolchain_cases.cpp：可选补注释

- **实现注释：`toolchain 依赖的溢出/下溢返回`**。位置：[tests/internal/toolchain_cases.cpp:35](../../tests/internal/toolchain_cases.cpp#L35)（第 35–45 行）。

  依据：此处直接验证fast_float在result_out_of_range时写入0/inf，与MYB拒绝非有限结果的规则处于不同层次。

  建议：可说明这是依赖库契约测试，MYB语义由DecimalCodec再分类，避免把inf断言理解为语言接受inf。

## 6. 建议处理顺序

1. 拆分 `flex_allocations.hpp` 的三个普通实现，保持接口契约、异常安全顺序和测试故障位置。
2. 补充 `compiler.cpp`、`verify.cpp`、`vm.cpp` 的算法与根交接说明，并扩充 `State` 的现有 Doxygen。
3. 补充 `checker.cpp`、`source.cpp`、`heap.cpp` 的检查顺序与错误路径原因。
4. 补充测试辅助回调契约和固定数值、字节偏移、帧容量的构造依据。
5. 可选项再按统一风格决定；不为增加注释数量重复接口说明。

本报告没有实施上述改动。未来拆分涉及 CMake 与 C++ 编译单元，需要运行项目规定的 Debug、Release 验收；仅增加注释时也应检查词表、差异与注释是否描述真实契约。

## 7. 全部文件审查清单

表中“无本轮新增项”表示本次审查未发现需要列出的明确说明缺口，结合现有声明处注释和实现判断；文件并不需要因为缺少函数前注释而逐个复制 Doxygen。

### 7.1 头文件（41 个）

| 文件 | 判定 | 核对要点 |
|---|---|---|
| [src/ast/factory.hpp:1](../../src/ast/factory.hpp#L1) | 适合保留 | 模板工厂及高度计算辅助；实例化处需要定义。 |
| [src/ast/node.hpp:1](../../src/ast/node.hpp#L1) | 适合保留 | 节点基本状态、模板只读容器及其迭代器。 |
| [src/ast/nodes.hpp:1](../../src/ast/nodes.hpp#L1) | 适合保留 | 节点类型、简短所有权接收与只读访问器；普通节点构造已分离。 |
| [src/ast/visitor.hpp:1](../../src/ast/visitor.hpp#L1) | 适合保留 | 纯虚访问接口、前向声明和默认析构。 |
| [src/builtins/registry.hpp:1](../../src/builtins/registry.hpp#L1) | 适合保留 | 类型/接口声明或简短构造、访问、转发；普通算法已分离。 |
| [src/bytecode/binding_layout.hpp:1](../../src/bytecode/binding_layout.hpp#L1) | 适合保留 | 类型/接口声明或简短构造、访问、转发；普通算法已分离。 |
| [src/bytecode/compiler.hpp:1](../../src/bytecode/compiler.hpp#L1) | 适合保留 | 类型/接口声明或简短构造、访问、转发；普通算法已分离。 |
| [src/bytecode/instruction.hpp:1](../../src/bytecode/instruction.hpp#L1) | 适合保留 | 类型/接口声明或简短构造、访问、转发；普通算法已分离。 |
| [src/bytecode/program.hpp:1](../../src/bytecode/program.hpp#L1) | 适合保留 | 产物类型、根登记构造及注销转发；名称登记与扫描已分离。 |
| [src/bytecode/verify.hpp:1](../../src/bytecode/verify.hpp#L1) | 适合保留 | 类型/接口声明或简短构造、访问、转发；普通算法已分离。 |
| [src/cli/options.hpp:1](../../src/cli/options.hpp#L1) | 适合保留 | 类型/接口声明或简短构造、访问、转发；普通算法已分离。 |
| [src/cli/session.hpp:1](../../src/cli/session.hpp#L1) | 适合保留 | 类型/接口声明或简短构造、访问、转发；普通算法已分离。 |
| [src/diagnostic/failure.hpp:1](../../src/diagnostic/failure.hpp#L1) | 适合保留 | 错误记录和简短首错状态操作；异常接口及访问器。 |
| [src/frontend/driver.hpp:1](../../src/frontend/driver.hpp#L1) | 适合保留 | 简短状态访问与树所有权交付；解析流程已在 cpp。 |
| [src/frontend/flex_allocations.hpp:1](../../src/frontend/flex_allocations.hpp#L1) | 建议移出 | FlexAllocationLedger::allocate；FlexAllocationLedger::release；FlexAllocationLedger::reallocate，详见前文。 |
| [src/frontend/scanner.hpp:1](../../src/frontend/scanner.hpp#L1) | 适合保留 | 简短匹配位置更新、EOF范围与错误转发；转义算法已分离。 |
| [src/platform/host_io.hpp:1](../../src/platform/host_io.hpp#L1) | 适合保留 | 类型/接口声明或简短构造、访问、转发；普通算法已分离。 |
| [src/platform/utf8_sink.hpp:1](../../src/platform/utf8_sink.hpp#L1) | 适合保留 | 类型/接口声明或简短构造、访问、转发；普通算法已分离。 |
| [src/runtime/decimal_codec.hpp:1](../../src/runtime/decimal_codec.hpp#L1) | 适合保留 | 类型/接口声明或简短构造、访问、转发；普通算法已分离。 |
| [src/runtime/display.hpp:1](../../src/runtime/display.hpp#L1) | 适合保留 | 类型/接口声明或简短构造、访问、转发；普通算法已分离。 |
| [src/runtime/heap.hpp:1](../../src/runtime/heap.hpp#L1) | 适合保留 | 类型/接口声明或简短构造、访问、转发；普通算法已分离。 |
| [src/runtime/object.hpp:1](../../src/runtime/object.hpp#L1) | 适合保留 | 类型/接口声明或简短构造、访问、转发；普通算法已分离。 |
| [src/runtime/operators.hpp:1](../../src/runtime/operators.hpp#L1) | 适合保留 | 类型/接口声明或简短构造、访问、转发；普通算法已分离。 |
| [src/runtime/roots.hpp:1](../../src/runtime/roots.hpp#L1) | 适合保留 | 根类型声明、简短访问与转发；链表登记和注销已分离。 |
| [src/runtime/schema.hpp:1](../../src/runtime/schema.hpp#L1) | 适合保留 | 类型/接口声明或简短构造、访问、转发；普通算法已分离。 |
| [src/runtime/text_builder.hpp:1](../../src/runtime/text_builder.hpp#L1) | 适合保留 | 类型/接口声明或简短构造、访问、转发；普通算法已分离。 |
| [src/runtime/value.hpp:1](../../src/runtime/value.hpp#L1) | 适合保留 | 类型/接口声明或简短构造、访问、转发；普通算法已分离。 |
| [src/runtime/value_ops.hpp:1](../../src/runtime/value_ops.hpp#L1) | 适合保留 | 类型/接口声明或简短构造、访问、转发；普通算法已分离。 |
| [src/semantic/checked_ast.hpp:1](../../src/semantic/checked_ast.hpp#L1) | 适合保留 | 类型/接口声明或简短构造、访问、转发；普通算法已分离。 |
| [src/semantic/checker.hpp:1](../../src/semantic/checker.hpp#L1) | 适合保留 | 类型/接口声明或简短构造、访问、转发；普通算法已分离。 |
| [src/source/source.hpp:1](../../src/source/source.hpp#L1) | 适合保留 | 类型/接口声明或简短构造、访问、转发；普通算法已分离。 |
| [src/source/span.hpp:1](../../src/source/span.hpp#L1) | 适合保留 | 类型/接口声明或简短构造、访问、转发；普通算法已分离。 |
| [src/source/utf8.hpp:1](../../src/source/utf8.hpp#L1) | 适合保留 | 类型/接口声明或简短构造、访问、转发；普通算法已分离。 |
| [src/vm/call.hpp:1](../../src/vm/call.hpp#L1) | 适合保留 | 类型/接口声明或简短构造、访问、转发；普通算法已分离。 |
| [src/vm/frame.hpp:1](../../src/vm/frame.hpp#L1) | 适合保留 | 类型/接口声明或简短构造、访问、转发；普通算法已分离。 |
| [src/vm/globals.hpp:1](../../src/vm/globals.hpp#L1) | 适合保留 | 声明与根登记构造、清空转发；名称查找与写入已分离。 |
| [src/vm/value_store.hpp:1](../../src/vm/value_store.hpp#L1) | 适合保留 | 类型/接口声明或简短构造、访问、转发；普通算法已分离。 |
| [src/vm/vm.hpp:1](../../src/vm/vm.hpp#L1) | 适合保留 | 声明、服务借用说明和析构清理转发。 |
| [tests/internal/fixtures.hpp:1](../../tests/internal/fixtures.hpp#L1) | 适合保留 | 结果类型与接口声明；会话实现已在 cpp。 |
| [tests/internal/test_hooks.hpp:1](../../tests/internal/test_hooks.hpp#L1) | 适合保留 | 测试故障枚举、计数记录与接口声明。 |
| [tests/internal/test_support.hpp:1](../../tests/internal/test_support.hpp#L1) | 可选整理 | require，详见前文。 |

### 7.2 实现文件（46 个）

| 文件 | 判定 | 核对要点 |
|---|---|---|
| [src/ast/nodes.cpp:1](../../src/ast/nodes.cpp#L1) | 无本轮新增项 | 成员移动构造、默认析构与单次Visitor分派；声明处已有契约。 |
| [src/builtins/io_builtins.cpp:1](../../src/builtins/io_builtins.cpp#L1) | 无本轮新增项 | 借用实参、显示分配及输出根交接已有定义处说明。 |
| [src/builtins/registry.cpp:1](../../src/builtins/registry.cpp#L1) | 无本轮新增项 | 目标与数量预检短小，检查顺序已由声明说明。 |
| [src/bytecode/binding_layout.cpp:1](../../src/bytecode/binding_layout.cpp#L1) | 无本轮新增项 | 预分槽与子树区间已有定义说明，顶层let例外已有实现说明。 |
| [src/bytecode/compiler.cpp:1](../../src/bytecode/compiler.cpp#L1) | 应补注释 | BytecodeCompiler::compile；BytecodeCompiler::visit(const LetStmt &)；BytecodeCompiler::visit(const ForStmt &)；BytecodeCompiler::visit(const UnaryExpr &)；BytecodeCompiler::visit(const LogicalExpr &)，详见前文。 |
| [src/bytecode/program.cpp:1](../../src/bytecode/program.cpp#L1) | 无本轮新增项 | 常量根扫描、名称登记与容量检查可由现有契约及直观流程理解。 |
| [src/bytecode/verify.cpp:1](../../src/bytecode/verify.cpp#L1) | 应补注释 | verify(CodeUnit &, EmergencyFailure &) / seed、analyze；verify(CodeUnit &, EmergencyFailure &) / 不可达片段分析；State，详见前文。 |
| [src/cli/options.cpp:1](../../src/cli/options.cpp#L1) | 无本轮新增项 | 短小命令选项分派，声明已说明入口规则。 |
| [src/cli/session.cpp:1](../../src/cli/session.cpp#L1) | 可选补注释 | Session::run / Failure 处理，详见前文。 |
| [src/diagnostic/failure.cpp:1](../../src/diagnostic/failure.cpp#L1) | 无本轮新增项 | 先登记首错再构造异常已在声明处说明，阶段分派直观。 |
| [src/frontend/driver.cpp:1](../../src/frontend/driver.cpp#L1) | 无本轮新增项 | 解析栈异常展开后清理已有说明；yylex声明注释见parser.y。 |
| [src/frontend/scanner.cpp:1](../../src/frontend/scanner.cpp#L1) | 可选补注释 | ScannerContext::escape；flex_fatal，详见前文。 |
| [src/main.cpp:1](../../src/main.cpp#L1) | 无本轮新增项 | 入口与统一退出边界已有定义处说明。 |
| [src/platform/windows_io.cpp:1](../../src/platform/windows_io.cpp#L1) | 无本轮新增项 | 编码、句柄、部分IO及失败清理已有契约和局部说明。 |
| [src/runtime/decimal_codec.cpp:1](../../src/runtime/decimal_codec.cpp#L1) | 可选补注释 | 可选解释Content检查先于范围累计；其余数值转换已有说明。 |
| [src/runtime/display.cpp:1](../../src/runtime/display.cpp#L1) | 无本轮新增项 | 当前标量显示分派直观，缓冲和格式契约在声明处。 |
| [src/runtime/heap.cpp:1](../../src/runtime/heap.cpp#L1) | 应补注释 | 应补首错状态参与GC判定的原因；其他发布、标记和失败清理已有说明。 |
| [src/runtime/roots.cpp:1](../../src/runtime/roots.cpp#L1) | 无本轮新增项 | 根链登记、注销及稳定槽扫描已有契约与局部说明。 |
| [src/runtime/text_builder.cpp:1](../../src/runtime/text_builder.cpp#L1) | 无本轮新增项 | 受控增长、记账与载荷转移已有说明。 |
| [src/runtime/toolchain_contract.cpp:1](../../src/runtime/toolchain_contract.cpp#L1) | 无本轮新增项 | 编译期环境断言，不承载复杂运行算法。 |
| [src/runtime/value_ops.cpp:1](../../src/runtime/value_ops.cpp#L1) | 无本轮新增项 | 精确混合比较、UTF-8排序、MIN/-1与floor修正已有说明。 |
| [src/semantic/checker.cpp:1](../../src/semantic/checker.cpp#L1) | 应补注释 | Checker::visit(const AssignStmt &)；Checker::visit(const ForStmt &)，详见前文。 |
| [src/source/source.cpp:1](../../src/source/source.cpp#L1) | 应补注释 | SourceManager::locate，详见前文。 |
| [src/source/utf8.cpp:1](../../src/source/utf8.cpp#L1) | 无本轮新增项 | 多字节形状与非法标量过滤已有说明，计数为直接组合。 |
| [src/vm/call.cpp:1](../../src/vm/call.cpp#L1) | 无本轮新增项 | 边界加法检查短小直接，声明已有容量失败契约。 |
| [src/vm/globals.cpp:1](../../src/vm/globals.cpp#L1) | 无本轮新增项 | 查找、存在性/初始化检查和写入均与头文件契约直接对应。 |
| [src/vm/value_store.cpp:1](../../src/vm/value_store.cpp#L1) | 无本轮新增项 | 两数组准备完成后再resize的提交顺序已有实现说明。 |
| [src/vm/vm.cpp:1](../../src/vm/vm.cpp#L1) | 应补注释 | VM::execute / 每轮活动帧借用；VM::execute / Op::BINARY，详见前文。 |
| [tests/internal/bytecode_cases.cpp:1](../../tests/internal/bytecode_cases.cpp#L1) | 无本轮新增项 | 合法/非法与可达/不可达的构造、协议组及常量故障已有说明。 |
| [tests/internal/bytecode_smoke_cases.cpp:1](../../tests/internal/bytecode_smoke_cases.cpp#L1) | 无本轮新增项 | 短小指令序列与栈深/下溢断言直观。 |
| [tests/internal/checker_cases.cpp:1](../../tests/internal/checker_cases.cpp#L1) | 可选补注释 | frontend_failure_cleanup_and_locations 固定位置表，详见前文。 |
| [tests/internal/decimal_cases.cpp:1](../../tests/internal/decimal_cases.cpp#L1) | 应补注释 | parse_cases / format_cases / 固定种子回读，详见前文。 |
| [tests/internal/failure_cases.cpp:1](../../tests/internal/failure_cases.cpp#L1) | 无本轮新增项 | 首错及阶段映射用例短小，输入与期望直接对应。 |
| [tests/internal/fixtures.cpp:1](../../tests/internal/fixtures.cpp#L1) | 应补注释 | MemoryHostIO / counted_print / counted_write / VmFixture::State，详见前文。 |
| [tests/internal/frontend_cases.cpp:1](../../tests/internal/frontend_cases.cpp#L1) | 应补注释 | ast_contract 高度超限后的所有权；ast_dispatch_and_cleanup 高度254/256，详见前文。 |
| [tests/internal/heap_cases.cpp:1](../../tests/internal/heap_cases.cpp#L1) | 应补注释 | trace_values / resource_failure，详见前文。 |
| [tests/internal/main.cpp:1](../../tests/internal/main.cpp#L1) | 无本轮新增项 | 筛选、逐例清故障、异常计数和非空集检查直接可读。 |
| [tests/internal/native_handoff_cases.cpp:1](../../tests/internal/native_handoff_cases.cpp#L1) | 应补注释 | make_text / echo，详见前文。 |
| [tests/internal/smoke_cases.cpp:1](../../tests/internal/smoke_cases.cpp#L1) | 无本轮新增项 | 各独立脚本与退出/输出断言直接对应。 |
| [tests/internal/source_cases.cpp:1](../../tests/internal/source_cases.cpp#L1) | 应补注释 | source_positions 字节偏移与码点列，详见前文。 |
| [tests/internal/test_hooks.cpp:1](../../tests/internal/test_hooks.cpp#L1) | 无本轮新增项 | 一次故障命中前清除设置的行为已在头文件说明。 |
| [tests/internal/toolchain_cases.cpp:1](../../tests/internal/toolchain_cases.cpp#L1) | 可选补注释 | toolchain 依赖的溢出/下溢返回，详见前文。 |
| [tests/internal/value_ops_cases.cpp:1](../../tests/internal/value_ops_cases.cpp#L1) | 无本轮新增项 | 数值、身份和根交接测试已有场景说明及直接期望。 |
| [tests/internal/vm_call_cases.cpp:1](../../tests/internal/vm_call_cases.cpp#L1) | 无本轮新增项 | 独立脚本直接验证返回、词法查找、递归与真实调用链。 |
| [tests/internal/vm_failure_cases.cpp:1](../../tests/internal/vm_failure_cases.cpp#L1) | 应补注释 | vm_call_failure_values / states / frames，详见前文。 |
| [tests/internal/vm_scalar_cases.cpp:1](../../tests/internal/vm_scalar_cases.cpp#L1) | 无本轮新增项 | 独立脚本直接验证绑定状态、短路与循环；初始准备故障断言清楚。 |

## 8. 检查记录

- 文件清单与四组审查记录逐项比对：87/87，文件无重复、无遗漏。
- 所有问题位置均按 `4e30d63` 的实际文件核对，行号范围有效。
- 本报告的源码相对链接、词表、行尾空白和差异格式检查通过。
- 本轮新增本报告，未修改源码、构建文件或既有未提交的测试计划。
- 本轮为静态组织与注释审查；验证范围为文件覆盖、证据位置和文档检查，未执行构建及行为测试。

检查通过。
