"""tests/tinytest.h 的 Python 版 —— **同一套输出契约**。

C 与 Python 两种测试程序，`tests/test.py` 用同一套规则解析，所以两边都必须只打印
这四种行：

    PASS <case>
    FAIL <case> -- <说明> | <file>:<line>
    SKIP <case> -- <原因>
    SUMMARY pass=N fail=M skip=K <program>

用法：

    from tinytest import case, run, eq, contains

    @case
    def policy_rejects_unknown_tag():
        out = sh("tools/policy.py", "role", "apps/demo/x.c")
        contains("2", out.stdout)

    if __name__ == "__main__":
        raise SystemExit(run("test_gates"))

断言失败就抛异常、当前用例立即结束，不影响其他用例。
"""

import subprocess
import sys
import tempfile
import traceback
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
TIMEOUT_S = 60

CASES = []


class _Fail(Exception):
    pass


class _Skip(Exception):
    pass


def case(fn):
    """把一个函数注册成用例；函数名就是打印出来的用例名。"""
    CASES.append(fn)
    return fn


def fail(msg):
    raise _Fail(str(msg))


def skip(reason):
    raise _Skip(str(reason))


def true(cond, msg="断言失败"):
    if not cond:
        fail(msg)


def eq(expected, actual, what=""):
    if expected != actual:
        fail(f"{what + ': ' if what else ''}期望 {expected!r}, 实际 {actual!r}")


def contains(needle, haystack, what=""):
    if needle not in haystack:
        head = str(haystack).strip().splitlines()
        first = head[0] if head else ""
        fail(f"{what + ': ' if what else ''}输出里没有 {needle!r}（首行: {first!r}）")


class Result:
    """sh() 的返回值：只带测试真正会看的三样东西。"""

    def __init__(self, returncode, stdout, stderr):
        self.returncode = returncode
        self.stdout = stdout
        self.stderr = stderr


def sh(*args, cwd=None):
    """跑一个命令，返回 Result（utf-8 解码，不抛异常）。

    这个环境禁止给子进程建管道（`CreatePipe` → PermissionError），
    所以输出走临时文件。别改成 capture_output=True。
    """
    cmd = [sys.executable] + [str(a) for a in args]
    with tempfile.TemporaryFile() as out, tempfile.TemporaryFile() as err:
        proc = subprocess.run(cmd, cwd=str(cwd or REPO), stdin=subprocess.DEVNULL,
                              stdout=out, stderr=err, timeout=TIMEOUT_S)
        out.seek(0)
        err.seek(0)
        return Result(proc.returncode,
                      out.read().decode("utf-8", "replace"),
                      err.read().decode("utf-8", "replace"))


def run(program):
    """跑全部用例，打印契约行，返回进程退出码。"""
    n_pass = n_fail = n_skip = 0
    for fn in CASES:
        name = fn.__name__
        try:
            fn()
        except _Skip as exc:
            n_skip += 1
            print(f"SKIP {name} -- {exc}")
        except _Fail as exc:
            n_fail += 1
            print(f"FAIL {name} -- {exc} | tests/{Path(sys.argv[0]).name}:0")
        except Exception:  # noqa: BLE001 —— 未预期的异常也是失败，但要留下栈
            n_fail += 1
            last = traceback.format_exc().strip().splitlines()[-1]
            print(f"FAIL {name} -- 未预期异常: {last} | tests/{Path(sys.argv[0]).name}:0")
        else:
            n_pass += 1
            print(f"PASS {name}")
        sys.stdout.flush()
    print(f"SUMMARY pass={n_pass} fail={n_fail} skip={n_skip} {program}")
    sys.stdout.flush()
    return 1 if n_fail else 0
