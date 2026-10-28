"""Observe the actual Unicode CLI, independent of the manifest runner."""
import os
from pathlib import Path
import subprocess
import shutil
import tempfile
import unittest

INTERPRETER = Path(os.environ.get("MYB_INTERPRETER", "build/debug/myb.exe")).resolve()


class CliTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="myb-cli-")
        self.addCleanup(self.temp.cleanup)
        self.cwd = Path(self.temp.name)

    def invoke(self, *args, stdin=b"", cwd=None):
        return subprocess.run([str(INTERPRETER), *args], cwd=cwd or self.cwd,
                              input=stdin, capture_output=True, timeout=5)

    def test_help_version_and_invalid_usage(self):
        version = self.invoke("--version")
        self.assertEqual((version.returncode, version.stdout, version.stderr),
                         (0, b"myb 0.1.0 (language 0.2)\n", b""))
        help_result = self.invoke("--help")
        self.assertEqual(help_result.returncode, 0)
        self.assertIn(b"Usage:", help_result.stdout)
        self.assertEqual(help_result.stderr, b"")
        for args in ((), ("--bogus",), ("--check",), ("--help", "x"),
                     ("a", "b"), ("a", "--check"), ("--check", "--check", "x")):
            with self.subTest(args=args):
                failed = self.invoke(*args)
                self.assertEqual(failed.returncode, 2, failed.stderr)
                self.assertEqual(failed.stdout, b"")

    def test_relocated_application_runs_without_development_path(self):
        application = self.cwd / "独立 运行目录"
        application.mkdir()
        interpreter = application / "myb.exe"
        shutil.copy2(INTERPRETER, interpreter)
        for library in INTERPRETER.parent.glob("*.dll"):
            shutil.copy2(library, application / library.name)
        environment = os.environ.copy()
        windows = Path(environment["SYSTEMROOT"])
        environment["PATH"] = os.pathsep.join([str(windows / "System32"), str(windows)])
        version = subprocess.run([str(interpreter), "--version"], cwd=application,
                                 env=environment, input=b"", capture_output=True, timeout=5)
        self.assertEqual((version.returncode, version.stdout, version.stderr),
                         (0, b"myb 0.1.0 (language 0.2)\n", b""))
        script = application / "程序.myb"
        script.write_text('write_text("报告.txt", "北邮😀");print(read_text("报告.txt"));',
                          encoding="utf-8")
        run = subprocess.run([str(interpreter), str(script)], cwd=application,
                             env=environment, input=b"", capture_output=True, timeout=5)
        self.assertEqual((run.returncode, run.stdout, run.stderr),
                         (0, "北邮😀\n".encode("utf-8"), b""))
        self.assertEqual((application / "报告.txt").read_bytes(), "北邮😀".encode("utf-8"))

    def test_missing_source_has_no_invented_location_or_call_chain(self):
        result = self.invoke("missing file.myb")
        self.assertEqual(result.returncode, 2)
        self.assertEqual(result.stdout, b"")
        self.assertIn(b"missing file.myb", result.stderr)
        self.assertNotRegex(result.stderr, rb":\d+:\d+:")
        self.assertNotIn(b"call chain", result.stderr)

    def test_missing_source_reports_absolute_path(self):
        cwd = self.cwd / "中文 空格目录"
        cwd.mkdir()
        name = "不存在 文件.myb"
        expected = str(cwd / name).replace("/", "\\").encode("utf-8")
        for mode in [(), ("--check",)]:
            with self.subTest(mode=mode):
                result = self.invoke(*mode, name, cwd=cwd)
                self.assertEqual((result.returncode, result.stdout), (2, b""))
                self.assertIn(expected, result.stderr.replace(b"/", b"\\"))
                self.assertNotRegex(result.stderr, rb":\d+:\d+:")
                self.assertNotIn(b"  at ", result.stderr)
                self.assertEqual(result.stderr.count(b"\n"), 1)

    def test_missing_source_is_resolved_per_cwd(self):
        directories = [self.cwd / "目录 一", self.cwd / "目录 二"]
        for directory in directories:
            directory.mkdir()
        name = "same name.myb"
        for index, cwd in enumerate(directories):
            expected = str(cwd / name).replace("/", "\\").encode("utf-8")
            other = str(directories[1 - index] / name).replace("/", "\\").encode("utf-8")
            for mode in [(), ("--check",)]:
                with self.subTest(cwd=cwd, mode=mode):
                    result = self.invoke(*mode, name, cwd=cwd)
                    self.assertEqual((result.returncode, result.stdout), (2, b""))
                    path_text = result.stderr.replace(b"/", b"\\")
                    self.assertIn(expected, path_text)
                    self.assertNotIn(other, path_text)
                    self.assertNotRegex(result.stderr, rb":\d+:\d+:")
                    self.assertNotIn(b"  at ", result.stderr)

    def test_entry_path_controls_are_escaped(self):
        name = "missing\n\t\u2028source.myb"
        result = self.invoke(name)
        self.assertEqual((result.returncode, result.stdout), (2, b""))
        self.assertIn(b"missing\\n\\t\\u{2028}source.myb", result.stderr)
        self.assertEqual(result.stderr.count(b"\n"), 1)
        self.assertNotIn(b"\t", result.stderr)
        self.assertNotIn("\u2028".encode("utf-8"), result.stderr)

    def test_loaded_source_keeps_absolute_path_and_location(self):
        script = self.cwd / "已有 中文源.myb"
        expected_path = str(script).replace("/", "\\").encode("utf-8")
        for source, location, category in [
                (b"\nlet value = ;", b":2:13: ", b"frontend error"),
                (b"\nprint(1/0);", b":2:8: ", b"runtime error [Arithmetic]")]:
            with self.subTest(category=category):
                script.write_bytes(source)
                result = self.invoke(script.name)
                self.assertEqual((result.returncode, result.stdout), (1, b""))
                self.assertTrue(result.stderr.replace(b"/", b"\\").startswith(
                    expected_path + location + category), result.stderr)

    def test_stderr_failure_preserves_entry_exit(self):
        readonly_file = self.cwd / "readonly.bin"
        readonly_file.write_bytes(b"")
        with readonly_file.open("rb") as readonly:
            result = subprocess.run([str(INTERPRETER), "missing.myb"], cwd=self.cwd,
                                    stdout=subprocess.PIPE, stderr=readonly, timeout=5)
        self.assertEqual((result.returncode, result.stdout), (2, b""))

    def test_unicode_filename_and_terminator_need_no_suffix(self):
        name = "-北邮 文件"
        (self.cwd / name).write_text('print("北邮😀");', encoding="utf-8")
        result = self.invoke("--", name)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout, "北邮😀\n".encode("utf-8"))
        self.assertEqual(result.stderr, b"")
        check = self.invoke("--check", "--", name, stdin=b"\xff\x00")
        self.assertEqual((check.returncode, check.stdout, check.stderr), (0, b"", b""))

    def test_stdin_is_not_treated_as_source(self):
        result = self.invoke("-", stdin=b"print(3);")
        self.assertEqual(result.returncode, 2)
        self.assertEqual(result.stdout, b"")

    def test_check_preserves_stdin_position_and_working_files(self):
        script = self.cwd / "check-effects.myb"
        script.write_text('write_text("created.txt", read_line());'
                          'print(read_text("missing.txt"));', encoding="utf-8")
        data = self.cwd / "input.bin"
        data.write_bytes(b"\xff\x00sentinel\n")
        before = {p.name: p.read_bytes() for p in self.cwd.iterdir()}
        with data.open("rb", buffering=0) as stream:
            result = subprocess.run([str(INTERPRETER), "--check", str(script)],
                                    cwd=self.cwd, stdin=stream, capture_output=True, timeout=5)
            self.assertEqual(stream.tell(), 0)
        self.assertEqual((result.returncode, result.stdout, result.stderr), (0, b"", b""))
        self.assertEqual({p.name: p.read_bytes() for p in self.cwd.iterdir()}, before)

    def test_readonly_stdout_reports_runtime_io_at_call_site(self):
        script = self.cwd / "readonly-output.myb"
        script.write_bytes(b"\nprint(3);")
        with script.open("rb") as readonly:
            result = subprocess.run([str(INTERPRETER), str(script)], cwd=self.cwd,
                                    stdin=subprocess.DEVNULL, stdout=readonly,
                                    stderr=subprocess.PIPE, timeout=5)
        self.assertEqual(result.returncode, 1)
        # The Windows host reports native paths, including when MinGW Python's
        # pathlib uses forward slashes for the same absolute file name.
        native_path = str(script).replace("/", "\\")
        self.assertIn((native_path + ":2:1: runtime error [IO]:").encode("utf-8"), result.stderr)
        self.assertEqual(script.read_bytes(), b"\nprint(3);")

    def test_host_command_output_failure_is_exit_three(self):
        script = self.cwd / "readonly.bin"
        script.write_bytes(b"")
        with script.open("rb") as readonly:
            result = subprocess.run([str(INTERPRETER), "--version"], cwd=self.cwd,
                                    stdout=readonly, stderr=subprocess.PIPE, timeout=5)
        self.assertEqual(result.returncode, 3)
        self.assertIn(b"internal error [IO]", result.stderr)

    def test_stderr_failure_preserves_existing_myberror_exit(self):
        script = self.cwd / "failure.myb"
        script.write_bytes(b"print(1/0);")
        with script.open("rb") as readonly:
            result = subprocess.run([str(INTERPRETER), str(script)], cwd=self.cwd,
                                    stdout=subprocess.PIPE, stderr=readonly, timeout=5)
        self.assertEqual(result.returncode, 1)
        self.assertEqual(result.stdout, b"")


if __name__ == "__main__":
    unittest.main()
