#!/usr/bin/env python3
"""ESP AI Skill 统一测试入口（唯一入口，`tools/run_tests.py` 已迁到这里）。

用法
----
    python tests/test.py                    跑全部层，**只统计**
    python tests/test.py api                跑 tests/api 整层，只统计
    python tests/test.py api common         收窄到 tests/api/common，**逐用例打印**
    python tests/test.py api common str     更窄
    python tests/test.py -v sys             强制逐用例
    python tests/test.py -q api             强制只统计
    python tests/test.py --list             只列要跑什么

口径
----
- **收窄到子路径就详细，只给一层就只统计**：跑某个身份的全部用例时刷屏没用，
  一口气跑整层时能看出"哪个程序、几条通过"。`-v` / `-q` 可覆盖。
- 每个用例程序是**独立进程**，带超时。生命周期测试会故意让任务不返回，
  同进程跑会拖死整批。
- 目录分层镜像源码树：`tests/<层>/<子路径>` ↔ 源码 `<层>/<子路径>`。
  所以 `python tests/test.py api common` 就是"api 下的 common 用例"。
- 程序可以是 `.c`（用 `cc` 编）或 `.py`（用当前解释器跑）。两者都只认
  `PASS/FAIL/SKIP/SUMMARY` 四种行——见 `tests/tinytest.h`。
- 头一段写 `// REQUIRES: framework/include/fw_app.h`（C）或
  `# REQUIRES: ...`（py），依赖的头不存在时该程序判 `SKIP`（**不是通过**）。

退出码：0 全通过 / 1 有失败 / 2 环境问题（没有 `cc`、目录不存在等）。
"""

import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
TESTS = REPO / "tests"
BUILD = REPO / "build" / "tests"
CC = None            # 见 find_cc()
TIMEOUT_S = 20

# 找一个能用的 C 编译器。顺序：$CC → cc → gcc → clang → cl（MSVC）。
# 不写死 cc：Windows 上通常只有 clang 或 cl，没有 cc 这个别名。
CC_CANDIDATES = ["cc", "gcc", "clang", "cl"]

# PATH 里找不到时的兜底绝对路径。Windows 上 LLVM / msys 装好了却没进 PATH 是常态
# （bash 里 `which clang` 能找到，Python 进程的 PATH 里却没有）。
CC_FALLBACKS = [
    r"C:\Program Files\LLVM\bin\clang.exe",
    r"C:\Program Files (x86)\LLVM\bin\clang.exe",
    r"C:\msys64\mingw64\bin\gcc.exe",
    r"C:\msys64\ucrt64\bin\gcc.exe",
    r"C:\MinGW\bin\gcc.exe",
    "/usr/bin/clang",
    "/usr/bin/gcc",
    "/opt/homebrew/bin/clang",
]

# 不当作"层"的目录
NOT_A_LAYER = {"support", "__pycache__", "build"}

# 每一层编测试时额外链接哪些源码根（相对仓库根）。
# 只在这里定义一次——测试文件自己说了不算，否则"测什么"就能被被测方改。
LINK = {
    "api": ["common", "api"],
    "common": ["common"],
    "framework": ["common", "api", "framework"],
    "app": ["common", "api", "apps/demo"],
    "sys": ["common", "api", "framework", "apps/system"],
    "platform": ["common", "platform"],
    "port": [],       # 平台一致性套件必须能独立编进固件，不链接任何项目源码
    "tools": [],      # 门禁自测：只跑 python，不编 C
}

INCLUDE_DIRS = [
    ".",
    "tests",
    "common/include",
    "api/af_api",
    "api/fp_api",
    "framework/include",
    "platform/port",
]

LINE_RE = re.compile(r"^(PASS|FAIL|SKIP) (\S+)")
SUM_RE = re.compile(r"^SUMMARY pass=(\d+) fail=(\d+) skip=(\d+) (\S+)")
REQ_RE = re.compile(r"^\s*(?://|#)\s*REQUIRES:\s*(\S+)\s*$", re.MULTILINE)


def _exec(cmd, timeout):
    """跑命令并收回输出。

    这个环境禁止给子进程建管道（`_winapi.CreatePipe` → PermissionError），
    所以走临时文件：写进去、再读回来。别改成 capture_output=True。
    """
    with tempfile.TemporaryFile() as out, tempfile.TemporaryFile() as err:
        proc = subprocess.run(cmd, cwd=str(REPO), stdin=subprocess.DEVNULL,
                              stdout=out, stderr=err, timeout=timeout)
        out.seek(0)
        err.seek(0)
        o = out.read().decode("utf-8", "replace")
        e = err.read().decode("utf-8", "replace")
    return proc.returncode, o, e


def find_cc():
    """$CC 优先，否则按 cc/gcc/clang/cl 顺序找第一个存在的。"""
    want = os.environ.get("CC")
    if want:
        return want if shutil.which(want) else None
    for cand in CC_CANDIDATES:
        if shutil.which(cand):
            return cand
    for cand in CC_FALLBACKS:
        if Path(cand).is_file():
            return cand
    return None


def layers():
    if not TESTS.is_dir():
        return []
    return sorted(p.name for p in TESTS.iterdir()
                  if p.is_dir() and p.name not in NOT_A_LAYER)


def discover(layer, sub):
    """返回命中 sub 的测试程序，按路径排序。

    sub 是目录 → 收窄到那个子树（`api common`）；
    sub 不是目录 → 当成文件名/用例名前缀过滤（`tools gates` 或 `tools test_gates`）。
    """
    root = TESTS / layer
    if not root.is_dir():
        return []
    found = []
    for p in sorted(root.rglob("*")):
        if not (p.is_file() and p.name.startswith("test_")):
            continue
        if p.suffix not in (".c", ".py"):
            continue
        found.append(p)

    if not sub:
        return found

    subdir = root.joinpath(*sub)
    if subdir.is_dir():
        return [p for p in found if subdir == p.parent or subdir in p.parents]

    want = "/".join(sub)
    return [p for p in found
            if want in p.relative_to(root).as_posix()
            or p.stem.startswith(want.rsplit("/", 1)[-1])]


def required_headers(path):
    try:
        head = path.read_text(encoding="utf-8", errors="replace")[:4000]
    except OSError:
        return []
    return REQ_RE.findall(head)


def sources_for(layer):
    srcs = []
    for rel in LINK.get(layer, []):
        base = REPO / rel
        if base.is_dir():
            srcs.extend(str(p) for p in sorted(base.rglob("*.c")))
    return srcs


def support_sources(layer):
    if layer == "port":
        return []
    sup = TESTS / "support"
    if not sup.is_dir():
        return []
    return [str(p) for p in sorted(sup.glob("*.c"))]


def build_c(layer, src, idx):
    BUILD.mkdir(parents=True, exist_ok=True)
    out = BUILD / f"{layer}__{src.stem}__{idx}"
    if os.name == "nt":
        out = out.with_suffix(".exe")
    cmd = [CC, "-std=c11", "-O0", "-g", "-Wall", "-Wextra"]
    for inc in INCLUDE_DIRS:
        cmd += ["-I", inc]
    cmd += [str(src)]
    cmd += support_sources(layer)
    cmd += sources_for(layer)
    cmd += ["-o", str(out)]
    rc, cout, cerr = _exec(cmd, 180)
    if rc != 0:
        first = ""
        for line in (cerr or cout or "").splitlines():
            if "error" in line.lower():
                first = line.strip()
                break
        return None, f"编译失败: {first or '见编译输出'}"
    return out, None


def run_one(layer, path, idx):
    """跑一个测试程序，返回 (pass, fail, skip, 行列表, 错误)。"""
    reqs = required_headers(path)
    for r in reqs:
        if not (REPO / r).exists():
            return 0, 0, 1, [f"SKIP {path.stem} -- 依赖 {r} 还不存在"], None

    try:
        if path.suffix == ".c":
            exe, err = build_c(layer, path, idx)
            if err:
                return 0, 1, 0, [f"FAIL {path.stem} -- {err}"], None
            cmd = [str(exe)]
        else:
            cmd = [sys.executable, str(path)]
        rc, out_s, err_s = _exec(cmd, TIMEOUT_S)
    except subprocess.TimeoutExpired:
        return 0, 1, 0, [f"FAIL {path.stem} -- 超时 {TIMEOUT_S}s"], None
    except FileNotFoundError as exc:
        return 0, 0, 0, [], f"环境问题: {exc}"

    lines = out_s.splitlines()
    p = f = s = 0
    for line in lines:
        m = LINE_RE.match(line)
        if m:
            if m.group(1) == "PASS":
                p += 1
            elif m.group(1) == "FAIL":
                f += 1
            else:
                s += 1
    if p + f + s == 0:
        f = 1
        tail = err_s.strip().splitlines()
        lines.append(f"FAIL {path.stem} -- 没有任何 PASS/FAIL/SKIP 行"
                     + (f"（stderr: {tail[-1]})" if tail else ""))
    return p, f, s, lines, None


def main(argv):
    ap = argparse.ArgumentParser(add_help=True, description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("scope", nargs="*", help="<层> [子路径 ...]")
    ap.add_argument("-v", "--verbose", action="store_true", help="强制逐用例打印")
    ap.add_argument("-q", "--quiet", action="store_true", help="强制只统计")
    ap.add_argument("--list", action="store_true", help="只列要跑什么")
    args = ap.parse_args(argv)

    global CC
    CC = find_cc()
    if CC is None:
        print("环境问题: 找不到 C 编译器（试过 $CC / cc / gcc / clang / cl）")
        return 2

    known = layers()
    # 无参数 = 全部层；第一段不是层名 = 跨层按程序名过滤
    if not args.scope:
        want_layers = known
        sub = []
    elif args.scope[0] in known:
        want_layers = [args.scope[0]]
        sub = args.scope[1:]
    else:
        want_layers = known
        sub = args.scope

    # 收窄到子路径 → 详细；只给一层 → 只统计
    verbose = args.verbose or (len(sub) >= 1 and not args.quiet)

    plan = []
    for layer in want_layers:
        for path in discover(layer, sub):
            plan.append((layer, path))

    if args.list:
        for layer, path in plan:
            print(f"{layer}\t{path.relative_to(REPO).as_posix()}")
        print(f"共 {len(plan)} 个测试程序")
        return 0

    if not plan:
        where = " ".join(args.scope) if args.scope else "tests/"
        print(f"没有找到测试：{where}")
        print("这不是通过——只是还没写测试。")
        return 0

    if verbose:
        print(f"逐用例模式：{len(plan)} 个程序\n")

    tot_p = tot_f = tot_s = 0
    errors = []
    started = time.time()
    for idx, (layer, path) in enumerate(plan):
        p, f, s, lines, err = run_one(layer, path, idx)
        if err:
            errors.append(err)
            continue
        tot_p, tot_f, tot_s = tot_p + p, tot_f + f, tot_s + s
        if verbose:
            print(f"-- {path.relative_to(REPO).as_posix()}")
            for line in lines:
                print("   " + line)
            print()
        else:
            flag = "FAIL" if f else ("SKIP" if (s and not p) else "ok")
            print(f"  {path.stem:<40} pass={p} fail={f} skip={s}  {flag}")

    elapsed = time.time() - started
    print()
    print(f"测试: pass={tot_p} fail={tot_f} skip={tot_s} "
          f"程序={len(plan)} 层={len(want_layers)} 用时={elapsed:.1f}s")
    if tot_s and not tot_f:
        print("注意: 有 SKIP —— SKIP 不是通过，看每条的原因。")
    if errors:
        for e in errors:
            print(f"环境问题: {e}")
        return 2
    return 1 if tot_f else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
