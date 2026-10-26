# Windows 终端验收

本文记录 M3 文本 I/O 的验证结果，供维护者补做真实终端验收。记录日期：2026-10-26。

## 当前结果

自动化文件和管道检查通过。真实控制台验收未完成。

当前执行环境的 stdin、stdout、stderr 调用 `GetConsoleMode` 均返回失败，错误码为 6。此环境无法提供 Windows Terminal 和传统控制台的人工输入、显示结果。

| 检查对象 | 结果 | 证据范围 |
|---|---|---|
| 匿名管道逐行读取、空行、EOF、无换行末行 | 通过 | `host_io` 内部测试和 I/O 黑盒 |
| 当前合法行之后的非法 UTF-8 | 通过 | 第一行正常返回，第二次读取才报告 Encoding |
| 管道中的 U+001A、BOM、NUL | 通过 | 按内容交出，行读取保留 BOM |
| 跨 4096 字节块的四字节码点 | 通过 | 管道读取和文件读取 |
| Unicode 路径、启动工作目录、覆盖写入 | 通过 | 文件 API 和黑盒隔离目录 |
| 文件 BOM、CRLF、NUL、空文件 | 通过 | 去开头一次 BOM，其他字节按长度保留 |
| 读、写、关闭失败及首错分类 | 通过 | OS 故障注入，真实匿名管道断开后写失败 |

## 待完成的真实终端检查

分别在 Windows Terminal 和传统控制台运行。每次记录 Windows 版本、终端版本、输入模式和实际显示结果。

| 操作 | Windows Terminal | 传统控制台 | 通过条件 |
|---|---|---|---|
| 输入、显示中文和 emoji | 未实测 | 未实测 | 返回与显示码点一致 |
| 输入 4095 个 ASCII 字符后接 emoji，再按 Enter | 未实测 | 未实测 | UTF-16 代理对跨块仍完整 |
| 输入普通行并按 Enter，检查 LF/CRLF | 未实测 | 未实测 | 返回内容去除行尾，其他 CR 保留 |
| 只输入 Ctrl+Z，再按 Enter | 未实测 | 未实测 | 当前及后续 `read_line()` 均返回 null |
| 在行中间输入 Ctrl+Z，再按 Enter | 未实测 | 未实测 | U+001A 保留为内容 |
| 输出超过 4096 UTF-16 单元的中文与 emoji | 未实测 | 未实测 | 显示完整，分块处没有缺字 |

使用以下程序检查逐行结果。Ctrl+Z 测试结束后重新启动程序。

```myb
let line = read_line();
while (line != null) {
    print(line);
    line = read_line();
}
print(read_line());
```

自动化检查命令：

```powershell
build/debug/myb_internal_tests.exe --case host_io
python tools/run_blackbox.py --interpreter build/debug/myb.exe --manifest tests/blackbox/io/manifest.json
```

文档用词与差异格式检查通过。
