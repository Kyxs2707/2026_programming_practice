# MYB

BUPT 2025–2026 学年程序设计实践：小型通用脚本语言与 C++20 字节码解释器，首版面向 Windows。

当前实现已完成 **M4 故障边界与自动化验收**：UTF-8 源码 → Flex/Bison → AST → 全文件结构检查 → 字节码生成与核验 → 显式帧 VM。支持动态变量、块作用域、比较与短路、if/while/for、用户函数和递归、数组与命名结构、深复制、统一显示及全部 21 个内置。Windows 文件、管道和真实 Win32 控制台自动检查通过；Windows Terminal 与传统控制台的人工交互和字形验收仍待完成，任务 18 保留未完成状态。

```powershell
& .\build\debug\myb.exe .\examples\hello.myb
# 输出 3
& .\build\debug\myb.exe --check .\examples\hello.myb
```

首次使用见 [构建与测试](docs/guides/构建与测试.md)。语言规则见 [语言规范](docs/specs/MYB语言规范.md)，实现安排见 [解释器设计](docs/specs/MYB解释器设计.md) 和 [实施计划](docs/plans/MYB解释器实施计划.md)，当前结果见 [M4 测试报告](docs/reports/M4测试报告.md)、[需求覆盖表](docs/reports/需求覆盖表.md) 和 [Windows 终端验收](docs/reports/Windows终端验收.md)。历史结果保留在 [M3 测试报告](docs/reports/M3测试报告.md)、[M2 测试报告](docs/reports/M2测试报告.md) 与 [M1 测试报告](docs/reports/测试报告_v0.1.md)。

函数与循环示例：`myb examples/functions.myb` 输出 `6`、`25` 两行。`--check` 已能检查完整语言语法并生成、核验全部代码，运行时能力按里程碑验收。

M1–M3 综合自检见 [m1_m3_comprehensive.myb](examples/m1_m3_comprehensive.myb)，覆盖主要语言成功路径和全部 21 个内置。在仓库根目录运行：

```powershell
& .\build\debug\myb.exe .\examples\m1_m3_comprehensive.myb
```

输入一行标签后按 Enter，空行或 EOF 使用默认值。断言全部通过后输出 `M1-M3 ALL PASS`；报告写入 `build/m1_m3_report.txt`，其中含换行与 NUL 的读写检查内容。

默认黑盒入口（372 个用例）：

```powershell
python tools/run_blackbox.py --interpreter build/debug/myb.exe
```

开发改动位于 `dev` 或其派生分支；`main` 仅通过 PR 合并。

CI 使用同一脚本运行 Windows Debug/Release 干净构建与回归：

```powershell
pwsh -File ./tools/ci.ps1 -Configuration Debug -Msys2Root D:/msys64
pwsh -File ./tools/ci.ps1 -Configuration Release -Msys2Root D:/msys64
```

脚本需要 PowerShell 7+（`pwsh`）；MSYS2 安装根按实际环境传入。自动回归与远端合并条件的验收记录见 [CI 与合并规则](docs/rules/CI与合并规则.md)。

Windows 构建输出包含 `myb.exe` 和三个 GCC 运行库 DLL。保留这些文件在同一目录即可从普通终端直接运行，无需给用户安装 MSYS2 或配置开发工具 PATH。
