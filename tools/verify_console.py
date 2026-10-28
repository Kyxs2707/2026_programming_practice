"""以隐藏 Win32 控制台验证实际 MYB 控制台输入和输出。"""

from __future__ import annotations

import argparse
import ctypes
from ctypes import wintypes as w
import json
import os
from pathlib import Path
import platform
import subprocess
import sys


class COORD(ctypes.Structure):
    _fields_ = [("X", w.SHORT), ("Y", w.SHORT)]


class KEY_EVENT(ctypes.Structure):
    _fields_ = [("down", w.BOOL), ("repeat", w.WORD), ("virtual_key", w.WORD),
                ("scan_code", w.WORD), ("character", w.WCHAR), ("control", w.DWORD)]


class INPUT_DATA(ctypes.Union):
    _fields_ = [("key", KEY_EVENT), ("padding", ctypes.c_byte * 16)]


class INPUT_RECORD(ctypes.Structure):
    _fields_ = [("kind", w.WORD), ("data", INPUT_DATA)]


def win32():
    """声明句柄宽度和参数类型，保留 Windows 原始错误代码。"""
    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    signatures = {
        "GetStdHandle": ([w.DWORD], w.HANDLE),
        "GetConsoleMode": ([w.HANDLE, ctypes.POINTER(w.DWORD)], w.BOOL),
        "SetConsoleMode": ([w.HANDLE, w.DWORD], w.BOOL),
        "SetConsoleCP": ([w.UINT], w.BOOL),
        "SetConsoleOutputCP": ([w.UINT], w.BOOL),
        "GetConsoleCP": ([], w.UINT),
        "GetConsoleOutputCP": ([], w.UINT),
        "SetConsoleScreenBufferSize": ([w.HANDLE, COORD], w.BOOL),
        "SetConsoleCursorPosition": ([w.HANDLE, COORD], w.BOOL),
        "FillConsoleOutputCharacterW": ([w.HANDLE, w.WCHAR, w.DWORD, COORD,
                                         ctypes.POINTER(w.DWORD)], w.BOOL),
        "ReadConsoleOutputCharacterW": ([w.HANDLE, w.LPWSTR, w.DWORD, COORD,
                                         ctypes.POINTER(w.DWORD)], w.BOOL),
        "WriteConsoleInputW": ([w.HANDLE, ctypes.POINTER(INPUT_RECORD), w.DWORD,
                                ctypes.POINTER(w.DWORD)], w.BOOL),
        "WriteConsoleW": ([w.HANDLE, w.LPCWSTR, w.DWORD, ctypes.POINTER(w.DWORD),
                            w.LPVOID], w.BOOL),
        "FlushConsoleInputBuffer": ([w.HANDLE], w.BOOL),
    }
    for name, (arguments, result) in signatures.items():
        function = getattr(kernel, name)
        function.argtypes = arguments
        function.restype = result
    return kernel


def checked(result):
    if not result:
        raise ctypes.WinError(ctypes.get_last_error())
    return result


def units(text):
    raw = text.encode("utf-16-le", errors="surrogatepass")
    return [chr(int.from_bytes(raw[i:i + 2], "little")) for i in range(0, len(raw), 2)]


def inject(kernel, handle, text):
    """按 UTF-16 单元送入按键事件，控制台行模式负责 Enter 编辑。"""
    characters = units(text)
    for start in range(0, len(characters), 256):
        chunk = characters[start:start + 256]
        records = (INPUT_RECORD * (2 * len(chunk)))()
        for index, character in enumerate(chunk):
            virtual_key = 13 if character in "\r\n" else 0
            for offset, down in enumerate((True, False)):
                records[2 * index + offset].kind = 1
                records[2 * index + offset].data.key = KEY_EVENT(
                    down, 1, virtual_key, 0, character, 0)
        written = w.DWORD()
        checked(kernel.WriteConsoleInputW(handle, records, len(records), ctypes.byref(written)))
        if written.value != len(records):
            raise AssertionError("控制台未接收全部按键事件")


def read_screen(kernel, handle, count):
    buffer = ctypes.create_unicode_buffer(count)
    copied = w.DWORD()
    checked(kernel.ReadConsoleOutputCharacterW(handle, buffer, count, COORD(0, 0),
                                             ctypes.byref(copied)))
    return "".join(buffer[:copied.value])


def run_myb(interpreter, script, directory, timeout=15):
    """子进程直接继承控制台；所有标准句柄均保持控制台类型。"""
    with subprocess.Popen([str(interpreter), str(script)], cwd=directory) as process:
        try:
            return process.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()
            raise


def worker(arguments):
    kernel = win32()
    directory = arguments.output.resolve()
    report = {"platform": platform.platform(), "python": sys.version,
              "interpreter": str(arguments.interpreter),
              "creation": "CREATE_NEW_CONSOLE, SW_HIDE",
              "code_page": arguments.code_page, "cases": []}
    try:
        handles = {name: kernel.GetStdHandle(number) for name, number in
                   (("stdin", -10), ("stdout", -11), ("stderr", -12))}
        modes = {}
        for name, handle in handles.items():
            mode = w.DWORD()
            checked(kernel.GetConsoleMode(handle, ctypes.byref(mode)))
            modes[name] = mode.value
        report["console_modes_before"] = modes
        checked(kernel.SetConsoleCP(arguments.code_page))
        checked(kernel.SetConsoleOutputCP(arguments.code_page))
        # 保留行编辑和控制字符处理，关闭回显以便独立检查程序输出。
        checked(kernel.SetConsoleMode(handles["stdout"], 0x0001 | 0x0002))
        report["console_modes_used"] = {"stdin": 3, "stdout": 3}
        report["console_code_pages"] = [kernel.GetConsoleCP(), kernel.GetConsoleOutputCP()]
        checked(kernel.SetConsoleScreenBufferSize(handles["stdout"], COORD(200, 200)))
        input_cases = [
            ("unicode_empty_control_z", 3, "first\r中文😀\r\rA\x1aB\r\x1a\r", "first|中文😀||A\x1aB"),
            ("surrogate_boundary", 3, "a" * 4095 + "😀\r\x1a\r", "a" * 4095 + "😀"),
            ("control_z_eof", 3, "\x1a\r", ""),
        ]
        scripts = Path(__file__).resolve().parents[1] / "tests" / "console"
        for name, mode, text, expected in input_cases:
            checked(kernel.SetConsoleMode(handles["stdin"], mode))
            checked(kernel.FlushConsoleInputBuffer(handles["stdin"]))
            output_file = directory / "lines.txt"
            output_file.unlink(missing_ok=True)
            inject(kernel, handles["stdin"], text)
            exit_code = run_myb(arguments.interpreter, scripts / "read_lines.myb", directory)
            actual = output_file.read_bytes() if output_file.exists() else None
            if exit_code != 0 or actual != expected.encode("utf-8"):
                raise AssertionError(f"{name}: exit={exit_code}, actual={actual!r}")
            (directory / f"{name}.utf8.bin").write_bytes(actual)
            report["cases"].append({"name": name, "result": "PASS",
                                    "input_mode": mode, "utf8_bytes": len(actual),
                                    "input_utf16_units": len(units(text))})
        # 读屏 API 对非 BMP 字符可能只返回替代字符。使用同一宿主的一次
        # 原生宽字符写入作参考，独立比较 MYB 的多次分块写入效果。
        output = "a" * 4095 + "😀" + "中文😀" * 1200 + " END"
        (directory / "output.txt").write_bytes(output.encode("utf-8"))
        def clear_screen():
            cleared = w.DWORD()
            checked(kernel.FillConsoleOutputCharacterW(handles["stdout"], " ", 40000,
                                                      COORD(0, 0), ctypes.byref(cleared)))
            checked(kernel.SetConsoleCursorPosition(handles["stdout"], COORD(0, 0)))

        clear_screen()
        reference_text = output + "\n"
        written = w.DWORD()
        checked(kernel.WriteConsoleW(handles["stdout"], reference_text, len(units(reference_text)),
                                     ctypes.byref(written), None))
        if written.value != len(units(reference_text)):
            raise AssertionError("原生参考输出未写完")
        reference = read_screen(kernel, handles["stdout"], 20000)
        (directory / "reference-utf16.bin").write_bytes(reference.encode("utf-16-le", "surrogatepass"))
        clear_screen()
        exit_code = run_myb(arguments.interpreter, scripts / "write_unicode.myb", directory)
        screen = read_screen(kernel, handles["stdout"], 20000)
        (directory / "screen-utf16.bin").write_bytes(screen.encode("utf-16-le", "surrogatepass"))
        if exit_code != 0 or screen != reference:
            at = next((i for i, (a, b) in enumerate(zip(screen, reference)) if a != b), None)
            raise AssertionError(f"write_unicode: exit={exit_code}, first_difference={at}, "
                                 f"actual={screen[max(0, (at or 0)-4):(at or 0)+12]!r}")
        report["cases"].append({"name": "write_unicode_boundary", "result": "PASS",
                                "utf16_units": len(units(output)), "screen_width": 200,
                                "reference": "single native WriteConsoleW",
                                "screen_replacement_characters": screen.count("\ufffd")})
        report["result"] = "PASS"
    except BaseException as error:
        report["result"] = "FAIL"
        report["error"] = repr(error)
        if "handles" in locals():
            try:
                report["screen_on_failure"] = read_screen(kernel, handles["stdout"], 20000).rstrip()
            except OSError as screen_error:
                report["screen_error"] = repr(screen_error)
    (directory / "result.json").write_text(json.dumps(report, ensure_ascii=True, indent=2), encoding="utf-8")
    return 0 if report["result"] == "PASS" else 1


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--interpreter", required=True, type=Path)
    parser.add_argument("--output", type=Path, default=Path("build/m4-console"))
    parser.add_argument("--worker", action="store_true", help=argparse.SUPPRESS)
    parser.add_argument("--code-page", type=int, default=65001, help=argparse.SUPPRESS)
    arguments = parser.parse_args()
    arguments.interpreter = arguments.interpreter.resolve(strict=True)
    if os.name != "nt":
        parser.error("本验收需要 Windows 控制台 API")
    arguments.output.mkdir(parents=True, exist_ok=True)
    if arguments.worker:
        return worker(arguments)
    failed = False
    for code_page in (65001, 936):
        directory = arguments.output.resolve() / f"cp{code_page}"
        directory.mkdir(parents=True, exist_ok=True)
        result_file = directory / "result.json"
        result_file.unlink(missing_ok=True)
        startup = subprocess.STARTUPINFO()
        startup.dwFlags = subprocess.STARTF_USESHOWWINDOW
        startup.wShowWindow = 0
        command = [sys.executable, str(Path(__file__).resolve()), "--worker", "--interpreter",
                   str(arguments.interpreter), "--output", str(directory), "--code-page", str(code_page)]
        with subprocess.Popen(command, creationflags=subprocess.CREATE_NEW_CONSOLE,
                              startupinfo=startup) as process:
            try:
                exit_code = process.wait(timeout=60)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
                exit_code = 1
        result = json.loads(result_file.read_text(encoding="utf-8")) if result_file.exists() else {}
        print(f"CP{code_page}: {result.get('result', 'FAIL')} "
              f"({len(result.get('cases', []))} cases); {result_file}")
        if result.get("error"):
            print(ascii(result["error"]))
        failed = failed or exit_code != 0 or result.get("result") != "PASS"
    return int(failed)


if __name__ == "__main__":
    raise SystemExit(main())
