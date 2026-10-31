"""check_headers.py —— 每个头文件能单独 include（自包含）。

为什么是独立检查：单元测试常常先 include 了别的头，于是漏 include 的
问题被掩盖。只有"单独 include 本头文件"的编译才有机会暴露它
（docs/code-structure.md §10 末尾那段踩坑记录）。

用法：
    python tools/check_headers.py
    python tools/check_headers.py --show-ok

退出码：0 通过 / 1 有头不能自包含 / 2 环境问题（找不到编译器）。
"""

from __future__ import annotations

import argparse
import sys
import tempfile
from pathlib import Path

from common import (
    EXIT_CODE,
    EXIT_ENV,
    EXIT_OK,
    REPO_ROOT,
    Report,
    env_or_none,
    need,
    rel,
    run,
    walk_files,
)

HEADER_SCOPES = ("common/", "api/", "framework/", "platform/")


def compiler() -> str:
    for cand in (env_or_none("CC") or "cc", "clang", "gcc"):
        try:
            return need(cand, "check_headers 要真的编一遍")
        except Exception:  # noqa: BLE001
            continue
    raise RuntimeError("找不到可用的 C 编译器（cc / clang / gcc）")


def include_dirs() -> list[str]:
    dirs: set[str] = set()
    for p in REPO_ROOT.rglob("*.h"):
        if ".git" not in p.parts:
            dirs.add(str(p.parent))
    for scope in HEADER_SCOPES:
        d = REPO_ROOT / scope
        if d.is_dir():
            dirs.add(str(d))
    return sorted(dirs)


def check(show_ok: bool) -> Report:
    headers = walk_files((".h",), HEADER_SCOPES)
    rep = Report("check_headers", checked=False)
    if not headers:
        return rep.skip("还没有头文件")
    cc = compiler()
    # 仓库根也进 -I：否则 `#include "common/include/x.h"` 这种写全路径的方式找不到文件
    incs = [f"-I{REPO_ROOT}"] + [f"-I{d}" for d in include_dirs()]
    rep.checked = True

    with tempfile.TemporaryDirectory(prefix="hdrchk-") as tmp:
        for h in headers:
            tu = Path(tmp) / "tu.c"
            tu.write_text(f'#include "{rel(h)}"\n', encoding="utf-8")
            cp = run([cc, "-fsyntax-only", "-x", "c", *incs, str(tu)])
            if cp.returncode == 0:
                if show_ok:
                    print(f"  ok   {rel(h)}")
                continue
            first = "\n".join(cp.stderr.strip().splitlines()[:4])
            rep.error(
                "HDR-SELF",
                f"单独 include 失败：{rel(h)}",
                hint="这个头缺 include 或者 include 顺序有隐式依赖。\n" + first,
            )
    return rep


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(prog="check_headers.py")
    ap.add_argument("--show-ok", action="store_true")
    args = ap.parse_args(argv)
    try:
        rep = check(args.show_ok)
    except RuntimeError as exc:
        print(f"check_headers: 环境问题 —— {exc}", file=sys.stderr)
        return EXIT_ENV
    rep.render()
    return EXIT_OK if rep.ok else EXIT_CODE


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
