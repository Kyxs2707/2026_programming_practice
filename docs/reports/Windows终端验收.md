# Windows 终端验收

本文记录 Windows 文本 I/O 的实际验收结果，供维护者判断自动检查范围和完成终端人工检查。记录日期：2026-10-27。

## 当前结果

文件、管道和隐藏 Win32 控制台自动检查通过。Windows Terminal、传统控制台分别在代码页 936、65001 下完成四组人工输入、EOF、长输出和完整显示检查。传统控制台复制文本的单空格差异单列保留。

当前宿主进程的标准句柄属于管道。验收脚本另启用 `CREATE_NEW_CONSOLE` 和 `SW_HIDE` 创建隐藏控制台；其 stdin、stdout、stderr 的 `GetConsoleMode` 均成功，因此 MYB 实际执行 `ReadConsoleW`、`WriteConsoleW` 分支。

| 检查对象 | 结果 | 证据范围 |
|---|---|---|
| 匿名管道逐行读取、空行、EOF、无换行末行 | 通过 | `host_io` 内部测试和 I/O 黑盒 |
| 当前合法行之后的非法 UTF-8 | 通过 | 第一行正常返回，第二次读取才报告 Encoding |
| 管道中的 U+001A、BOM、NUL | 通过 | 按内容交出，行读取保留 BOM |
| 跨 4096 字节块的四字节码点 | 通过 | 管道读取和文件读取 |
| Unicode 路径、启动工作目录、覆盖写入 | 通过 | 文件 API 和黑盒隔离目录 |
| 文件 BOM、CRLF、NUL、空文件 | 通过 | 去开头一次 BOM，其他字节按长度保留 |
| 读、写、关闭失败及首错分类 | 通过 | OS 故障注入，真实匿名管道断开后写失败 |

## 隐藏 Win32 控制台自动检查

环境为 Windows 11，系统版本 `10.0.26300`。工具为 UCRT64 GCC `15.2.0`、Python `3.12.12`、CMake `4.1.0`、Ninja `1.13.0`、Flex `2.6.4`、Bison `3.8.2`。独立 Debug 构建启用内部测试，独立 Release 构建关闭内部测试。

每种配置分别设置输入、输出代码页为 `65001` 和 `936`。输入模式为 `ENABLE_PROCESSED_INPUT | ENABLE_LINE_INPUT`，数值 `3`，关闭回显。输出模式数值为 `3`。脚本用 `WriteConsoleInputW` 注入 UTF-16 按键事件，MYB 直接继承三个控制台标准句柄。

| 自动检查 | Debug UTF-8 | Debug 936 | Release UTF-8 | Release 936 | 判断依据 |
|---|---|---|---|---|---|
| 普通行、中文和 emoji、空行、行中 U+001A | 通过 | 通过 | 通过 | 通过 | `read_line()` 结果经 `write_text()` 输出，与独立 UTF-8 预期逐字节一致；Enter 产生的 CRLF 被去除 |
| 4095 个 ASCII 字符后接 emoji、Enter | 通过 | 通过 | 通过 | 通过 | 结果为 4099 个 UTF-8 字节，跨 4096 UTF-16 单元读取仍保留完整代理对 |
| 仅 Ctrl+Z、Enter 与重复读取 EOF | 通过 | 通过 | 通过 | 通过 | 首次返回 null；之后再次读取仍为 null |
| 8901 个 UTF-16 单元的中文、emoji 和 ASCII 输出 | 通过 | 通过 | 通过 | 通过 | MYB 分块输出与同一宿主单次原生 `WriteConsoleW` 的 20000 个读屏单元相同 |

读屏使用 `ReadConsoleOutputCharacterW`。本机该 API 将输出中的 1201 个 emoji 映射为 U+FFFD，原生参考输出也相同。逐单元比较验证分块与代码页行为；emoji 字形和用户实际看到的显示结果仍按下一节人工记录。

在仓库根目录运行：

```powershell
& D:/msys64/ucrt64/bin/python.exe tools/verify_console.py `
    --interpreter build/m4-build/clean-debug/myb.exe --output build/m4-console-debug
& D:/msys64/ucrt64/bin/python.exe tools/verify_console.py `
    --interpreter build/m4-build/release/myb.exe --output build/m4-console-release
```

2026-10-27 使用本次完整构建矩阵生成的 Debug、Release 程序重跑上述命令。每条命令退出 `0`，两种代码页各输出 `PASS (4 cases)`，合计 16 项通过。结果保存在 `build/m4-console-debug/cp65001/result.json` 等四个组合目录；每个目录还包含输入结果字节文件、原生参考与 MYB 输出的 UTF-16 读屏文件。验收源码为 [控制台验收器](../../tools/verify_console.py)、[行输入程序](../../tests/console/read_lines.myb) 和 [输出程序](../../tests/console/write_unicode.myb)。

## 用户人工复验

2026-10-27，用户运行 `./build/release/myb.exe ./tests/console/interactive.myb` 并回传下列输出，随后确认代码页为 `936`。该次回传未说明终端类型，基础结果按代码页单列。

| 操作 | 回传结果 | 判断 |
|---|---|---|
| 输入 `你好😀` 后按 Enter | `RECEIVED ["你好😀"]` | 中文与 emoji 返回内容正确 |
| 单独按 Enter | `RECEIVED [""]` | 空行返回空字符串 |
| 单独 Ctrl+Z 后按 Enter | `EOF: read_line() returned null.` | 正常 EOF |
| EOF 后再次读取 | `REPEATED EOF: null` | EOF 状态保持 |
| 执行结束 | `DONE: leaving the interactive check.` | 执行至结束提示 |

本次输入后继续等待下一行，收到 Ctrl+Z 后结束循环。长行跨块、行中 Ctrl+Z、长输出显示及其余终端/代码页组合仍按下表补做。

### 补充回传：Windows Terminal，65001

用户补充了输入与长输出两份终端文本，并确认两份输出来自同一个 Windows Terminal 窗口，长输出时仍为代码页 `65001`；实际中文和 emoji 均显示正常，没有方框、问号或缺字。终端版本与具体输入模式未提供。

| 检查 | 核对结果 |
|---|---|
| 长行输入 | 完整 `RECEIVED` 内容与 `long-line.txt` 逐字一致：4095 个 `a` 加一个 emoji，共 4096 个码点、4097 个 UTF-16 单元 |
| 普通行与行尾 | `RECEIVED ["plain-123"]`，无残留行尾 |
| 中文、emoji 与空行 | 分别为 `RECEIVED ["你好😀"]`、`RECEIVED [""]` |
| 行中 Ctrl+Z | `RECEIVED ["ab\u{1a}cd"]`，随后继续等待 |
| EOF 与重复 EOF | 单独 Ctrl+Z 后显示首次 EOF、`REPEATED EOF: null` 和 `DONE` |
| 长输出内容 | 与 `output.txt` 逐字一致：7700 个码点、8901 个 UTF-16 单元，包含 1201 个 emoji，末尾为 ` END` |
| 长输出退出码 | `MYB exit: 0` |
| 可见字形 | 用户确认全部正常 |

交互记录已执行至 `DONE`，未单独打印交互程序的数值退出码。原始回传及核对结果保存在 `build/manual-user-evidence/2026-10-27/` 的 `input-transcript.txt`、`output-transcript.txt`、`result.json`。

首次输入回传的 936 段在 `RECEIVED` 行中途直接接入 `Active code page: 65001`，缺少该次完整结果。该缺口已由下面的新一轮 936 完整记录补齐。

### 再次回传：936 完整记录

用户提供 `CASE: Windows Terminal 936` 的完整交互与长输出，以及 `CASE: 传统控制台 936` 的完整交互，并确认两种窗口的中文和 emoji 实际字形均正常。

| 组合 | 交互内容 | 交互退出码 | 长输出内容 | 长输出退出码 |
|---|---|---|---|---|
| Windows Terminal 936 | 长行逐字匹配，普通行、中文、空行、行中 Ctrl+Z、EOF/重复 EOF/DONE 全部通过 | 0 | 与 `output.txt` 逐字相等，末尾 ` END` 完整 | 0 |
| 传统控制台 936 | 同上，长行逐字匹配并运行至 DONE | 未单独打印 | 尚未运行成功 | 未取得 |

传统控制台长输出的另一段回传在 `C:\Users\MECHREVO` 执行相对路径命令，`Resolve-Path` 找不到项目文件，MYB 未启动。空白的 `INTERACTIVE exit` 来自未取得该窗口进程退出值，不能作为 MYB 退出状态。

四份输入指南已将各命令块改为独立定位项目目录，并在交互结束后自动保存、打印退出码；传统窗口启动命令也会定位项目。从用户目录启动的四段长输出命令已实测退出 0，标准输入、输出、错误仍是控制台句柄，证据位于 `build/guide-command-check/`。本次用户原始回传及核对结果位于 `build/manual-user-evidence/2026-10-27/retest-936/`。

### 传统控制台 936 长输出补验

用户补充传统控制台输出并确认可正常运行。回传明确显示代码页 `936`、末尾 ` END` 和 `MYB exit: 0`。4095 个 `a`、1200 个 `中`、1200 个 `文`、1201 个 emoji 全部存在，完整显示检查通过。

复制文本有 7701 个码点、8902 个 UTF-16 单元，比源文件多一个 U+0020 空格；差异位于源文本从零计数的码点位置 4147。其余字符顺序相同。本项记录显示完整与成功退出，不记为复制文本逐字相等；实际窗口列宽未提供，空格原因尚未确定。

原始回传和完整比较结果保存在 `build/manual-user-evidence/2026-10-27/console-host-936-output/`。此前未取得的传统控制台 936 长输出结果已补齐。

### 传统控制台 65001 最后一组验收

用户回传 `CASE: 传统控制台 65001`、`OUTPUT CASE: 传统控制台 65001`，并确认本轮也正常。长行 `RECEIVED` 与输入文件逐字相等，普通行、中文/emoji、空行、行中 Ctrl+Z、EOF、重复 EOF 和 `DONE` 均符合预期，`INTERACTIVE exit: 0`。

长输出包含全部 4095 个 `a`、1200 组 `中文😀` 和末尾 ` END`，`MYB exit: 0`。复制文本在同一位置 4147 多一个 U+0020 空格，与传统控制台 936 记录一致；保留原始比较，不将它写为逐字相等。用户回传与核对结果保存在 `build/manual-user-evidence/2026-10-27/console-host-65001/`。

至此四组人工功能与完整显示验收全部完成。终端版本、实际输入模式等未提供的元数据按记录保留为空，不补写推测值。

## 真实终端检查汇总

输入文件、逐步按键、长输出命令和回传格式见 [终端人工检查输入](../guides/终端人工检查输入.md)。

分别在 Windows Terminal 和传统控制台运行。每次记录 Windows 版本、终端版本、输入模式和实际显示结果。

| 操作 | Windows Terminal 936 | Windows Terminal 65001 | 传统控制台 936 | 传统控制台 65001 | 通过条件 |
|---|---|---|---|---|---|
| 输入、显示中文和 emoji | 通过 | 通过 | 通过 | 通过 | 返回与显示码点一致 |
| 输入 4095 个 ASCII 字符后接 emoji，再按 Enter | 通过 | 通过 | 通过 | 通过 | UTF-16 代理对跨块仍完整 |
| 输入普通行并按 Enter，检查 LF/CRLF | 通过 | 通过 | 通过 | 通过 | 返回内容去除 Enter 产生的行尾 |
| 只输入 Ctrl+Z，再按 Enter | 通过 | 通过 | 通过 | 通过 | 当前及后续 `read_line()` 均返回 null |
| 在行中间输入 Ctrl+Z，再按 Enter | 通过 | 通过 | 通过 | 通过 | U+001A 保留为内容 |
| 输出超过 4096 UTF-16 单元的中文与 emoji | 通过 | 通过 | 通过，复制空格差异见上文 | 通过，复制空格差异见上文 | 显示完整，分块处没有缺字 |

分别在两个终端运行 [交互检查程序](../../tests/console/interactive.myb)。程序先说明输入方式，每次等待输入前显示 `WAITING FOR INPUT`；收到一行后显示 `RECEIVED` 和 `str([line])`，空行显示 `[""]`，行内 U+001A 显示 `\u{1a}`。正常 EOF 后依次显示首次 EOF、`REPEATED EOF: null` 和 `DONE` 并退出。Ctrl+Z 测试结束后重新启动程序。使用系统工具记录终端版本，并记录 `chcp` 结果；分别检查代码页 `65001` 与 `936`。

按上方对应组合的检查指南运行完整命令块，每组分别回传输出。

### Ctrl+C 调查记录

2026-10-26 的隐藏控制台调查结果保存在 `build/m4-ctrlc/result.json`，本次复核其 12 条 MYB 记录。输入模式为 `3`，代码页为 `936`：

| 触发方式 | 子进程属性 | 实际结果 |
|---|---|---|
| `GenerateConsoleCtrlEvent(CTRL_C_EVENT, 0)` | 默认 Ctrl+C 处理 | 3/3 次退出，Windows 退出状态均为 `0xC000013A`；没有返回行内容 |
| 同上 | 从父进程继承忽略 Ctrl+C 的属性 | 3/3 次在两秒内继续等待；调查程序随后注入 Ctrl+Z、Enter 清理，返回文件中的 `null` 来自该清理步骤 |
| `WriteConsoleInputW` 注入 U+0003 按键事件 | 两种属性各三次 | 6/6 次未触发进程退出；清理时读取到 U+0003 与 U+001A 字符 |

向输入缓冲写 U+0003 只验证字符输入，不能替代真实 Ctrl+C 信号。当前证据没有复现用户在 Ctrl+C 操作后看到 null 的现象。Ctrl+C 按中断检查记录，不计入正常 EOF 验收；Windows Terminal 与传统控制台仍需用户实际按键复验，并记录退出状态和屏幕内容。首次 EOF、重复 EOF 的通过标准使用单独 Ctrl+Z、Enter。

## 文件、管道复查

```powershell
./build/m4-build/clean-debug/myb_internal_tests.exe --case host_io
& D:/msys64/ucrt64/bin/python.exe tools/run_blackbox.py `
    --interpreter build/m4-build/clean-debug/myb.exe --manifest tests/blackbox/io/manifest.json
```

文档用词与差异格式检查通过。
