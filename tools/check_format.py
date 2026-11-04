#!/usr/bin/env python3
"""check_format.py —— 排版门禁：仓库里的 `.c` / `.h` 必须与 `.clang-format` 一致。

为什么需要它：`.clang-format` 只描述"应该长什么样"，不强制任何人执行。没有门禁时
两种漂移都会发生——① 有人手写了一个缩进不对的块，评审时看不出来；② 有人跑一次
全仓格式化，把别人没碰过的行也搅进 diff。有了这条，`./build.sh run` 直接把
"排版与样式文件不一致"变成一个红灯，而不是评审意见。

**为什么钉版本**：clang-format 的排版结果**跨版本会变**（18 与 21 在一批头文件的
宏/注释对齐上就不一样）。不钉版本会出现最糟的一种红：A 按 18 排好，CI 上的 21 说
"A 不对"——文件没人碰过，却红在别人头上；反过来也一样。所以：
- 仓库基准 = **clang-format 18.1.8**（`PINNED_VERSION`），CI 用
  `python3 -m pip install clang-format==18.1.8` 装同一个二进制；
- 本机版本不一致时**不判红，打 SKIP 并说明**——用别的版本判红等于把自己的版本
  当成标准，这正是"假红"的来源。

判据：对 `git ls-files` 里的每个 `.c` / `.h`，跑
`clang-format --dry-run --Werror --style=file`；有任何一个文件输出告警就红。

退出码：0 通过（或版本不符/找不到 clang-format 时 SKIP）/ 1 有文件不符合 /
2 用法或环境问题（**2 不是通过**）。
"""
from __future__ import annotations

import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
SUFFIXES = (".c", ".h")
FORMAT_TAGS = ("clang-format", "clang-format-18", "clang-format-19",
               "clang-format-20", "clang-format-17", "clang-format-16",
               "clang-format-15", "clang-format-14")
STYLE_FILE = REPO / ".clang-format"

# 仓库基准版本。改它 = 改全仓的排版 diff，属于要人拍板的改动；改完必须重排全仓，
# 并同步更新 docs/build-and-usage.md 的门禁表与 .github/workflows/ci.yml 的安装行。
PINNED_VERSION = "18.1.8"
VERSION_RE = re.compile(r"clang-format version (\d+\.\d+\.\d+)")


def find_formatter() -> list[str] | None:
    """返回可执行命令（argv 前缀），找不到返回 None。环境变量优先，便于钉版本。"""
    env = os.environ.get("CLANG_FORMAT")
    if env:
        return [env]
    for tag in FORMAT_TAGS:
        found = shutil.which(tag)
        if found:
            return [found]
    # macOS：Xcode 工具链里一定有，但不在 PATH 上。找到也要过版本检查——
    # Apple 的构建常落后于上游（本机实测 21.0.0），不能直接拿去判红。
    xcrun = shutil.which("xcrun")
    if xcrun:
        try:
            resolved = subprocess.run([xcrun, "-f", "clang-format"],
                                      capture_output=True, text=True, timeout=20)
        except (OSError, subprocess.SubprocessError):
            resolved = None
        if resolved is not None and resolved.returncode == 0:
            path = resolved.stdout.strip()
            if path and Path(path).exists():
                return [path]
    return None


def formatter_version(formatter: list[str]) -> str | None:
    """取 `X.Y.Z`；取不到返回 None（按"版本不符"处理，宁可 SKIP 也不误判红）。"""
    try:
        proc = subprocess.run(formatter + ["--version"], capture_output=True,
                              text=True, timeout=20)
    except (OSError, subprocess.SubprocessError):
        return None
    if proc.returncode != 0:
        return None
    matched = VERSION_RE.search(proc.stdout)
    return matched.group(1) if matched else None


def repo_sources() -> list[Path]:
    """仓库里受版本管理的 C 源与头。没有 git（极少数情况）时退回目录遍历。"""
    try:
        listed = subprocess.run(["git", "-C", str(REPO), "ls-files"],
                                capture_output=True, text=True, timeout=60)
    except (OSError, subprocess.SubprocessError):
        listed = None
    names: list[str] = []
    if listed is not None and listed.returncode == 0:
        names = [n for n in listed.stdout.splitlines() if n.endswith(SUFFIXES)]
    else:
        for path in sorted(REPO.rglob("*")):
            if path.suffix in SUFFIXES and "build" not in path.parts and ".git" not in path.parts:
                names.append(str(path.relative_to(REPO)))
    return [REPO / n for n in names]


def check_files(formatter: list[str], files: list[Path]) -> list[str]:
    """返回不符合样式的文件（相对路径）。一次调用传全部文件，省掉逐文件启动的开销。"""
    if not files:
        return []
    cmd = formatter + ["--dry-run", "--Werror", f"--style=file:{STYLE_FILE}"]
    cmd += [str(f) for f in files]
    try:
        proc = subprocess.run(cmd, capture_output=True, text=True, timeout=600)
    except (OSError, subprocess.SubprocessError) as exc:
        print(f"check_format: 调用 clang-format 失败：{exc}", file=sys.stderr)
        raise SystemExit(2)
    if proc.returncode == 0:
        return []
    bad: list[str] = []
    pattern = re.compile(r"^(.*\.(?:c|h)):\d+:\d+: error:")
    for line in proc.stderr.splitlines():
        matched = pattern.match(line)
        if matched is not None:
            rel = matched.group(1)
            if rel not in bad:
                bad.append(rel)
    if not bad:  # 解析不到具体文件，宁可把原始输出交出去，也不要假装通过
        print(proc.stderr.rstrip(), file=sys.stderr)
        raise SystemExit(2)
    return bad


def selftest(formatter: list[str]) -> int:
    """在临时目录里搭最小现场：一个好文件、一个坏文件，验证两边都判对。"""
    failures = 0

    def run_case(name: str, source: str, expect_bad: bool) -> None:
        nonlocal failures
        with tempfile.TemporaryDirectory() as tmp:
            tmp_path = Path(tmp)
            (tmp_path / ".clang-format").write_text(STYLE_FILE.read_text(encoding="utf-8"),
                                                    encoding="utf-8")
            target = tmp_path / "sample.c"
            target.write_text(source, encoding="utf-8")
            bad = check_files(formatter, [target])
            got = bool(bad)
            ok = got == expect_bad
            print(f"{'PASS' if ok else 'FAIL'} selftest_{name}")
            if not ok:
                failures += 1

    good = "int f(int a)\n{\n    return a + 1;\n}\n"
    bad_src = "int f(int a)\n{\n      return a + 1;   \n}\n"  # 缩进 6 格 + 行尾空格
    run_case("clean_file_passes", good, expect_bad=False)
    run_case("misindented_file_is_caught", bad_src, expect_bad=True)

    print(f"SUMMARY pass={2 - failures} fail={failures} skip=0 check_format_selftest")
    return 0 if failures == 0 else 1


def main(argv: list[str]) -> int:
    if not STYLE_FILE.exists():
        print(f"check_format: 缺样式文件 {STYLE_FILE}", file=sys.stderr)
        return 2
    formatter = find_formatter()
    if formatter is None:
        print("格式: SKIP（没找到 clang-format；装一个即可："
              f"python3 -m pip install --user clang-format=={PINNED_VERSION}）")
        return 0
    version = formatter_version(formatter)
    if version != PINNED_VERSION:
        print(f"格式: SKIP（本机 clang-format {version or '版本未知'} ≠ 仓库基准 "
              f"{PINNED_VERSION}；跨版本排版结果不同，用别的版本判红只会制造假红）")
        print("      想本地强制："
              f"python3 -m pip install --user clang-format=={PINNED_VERSION} 之后重跑"
              "（或在 CI 上看）")
        return 0
    if "--selftest" in argv:
        return selftest(formatter)
    if len(argv) > 1:
        print("用法：check_format.py [--selftest]", file=sys.stderr)
        return 2

    files = repo_sources()
    bad = check_files(formatter, files)
    if bad:
        print(f"格式: FAIL（{len(bad)}/{len(files)} 个文件与 .clang-format 不一致，"
              f"clang-format {PINNED_VERSION}）")
        for rel in bad[:10]:
            print(f"  {Path(rel).relative_to(REPO)}")
        if len(bad) > 10:
            print(f"  …… 另有 {len(bad) - 10} 个")
        print(f"修法：clang-format -i <文件>（版本必须是 {PINNED_VERSION}）")
        return 1
    print(f"格式: PASS（{len(files)} 个 .c/.h 与 .clang-format 一致，"
          f"clang-format {PINNED_VERSION}）")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
