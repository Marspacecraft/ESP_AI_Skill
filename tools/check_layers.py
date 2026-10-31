"""check_layers.py —— include 矩阵（比 purity 更细的分层检查）。

比 check_purity 多做一件事：purity 只管"不碰 SDK"，这里管"不碰别人的层"。
矩阵出自 docs/code-structure.md §2；改矩阵要同步改那张表。

用法：
    python tools/check_layers.py
    python tools/check_layers.py --verbose

退出码：0 通过 / 1 有跨层 include / 2 环境问题。
"""

from __future__ import annotations

import argparse
import re
import sys

from common import EXIT_CODE, EXIT_ENV, EXIT_OK, REPO_ROOT, Report, rel, walk_files

INCLUDE_RE = re.compile(r'^\s*#\s*include\s*[<"]([^>"]+)[>"]', re.M)

SOURCE_SUFFIX = (".c", ".h", ".cpp", ".hpp")

# (适用目录, 允许 include 的仓库内前缀, 说明)
# 只判仓库内的头；系统头（stdio.h 之类）不判。
MATRIX: list[tuple[str, tuple[str, ...], str]] = [
    ("apps/demo/", ("apps/demo/", "api/af_api/", "common/"),
     "业务 App：只看得到 api/af_api/ 与 common/（framework/ 一段都没有）"),
    ("apps/system/", ("apps/system/", "api/af_api/", "common/", "framework/include/"),
     "系统 App：多了 framework/include/（唯一的跨层读例外）"),
    ("framework/", ("framework/", "api/fp_api/", "common/", "platform/port/"),
     "框架：机制面无 api/af_api/，要原语走 platform/port/"),
    ("api/af_api/", ("api/af_api/", "common/"),
     "App 面接口：只依赖 common/"),
    ("api/fp_api/", ("api/fp_api/", "common/"),
     "平台面接口：只依赖 common/"),
    ("api/", ("api/", "common/"),
     "api/ 本身（含未细分的头）"),
    ("common/", ("common/",),
     "公共库：不依赖项目内任何别的目录"),
    ("platform/drivers/", ("platform/drivers/", "platform/port/", "api/fp_api/", "common/"),
     "公共驱动：不得认识 apps/ 与 framework/"),
    ("tests/", (), "测试可以 include 一切（它不进入固件镜像）"),
]



def header_index() -> dict[str, list[str]]:
    """仓库内头文件 → {基名: [从仓库根起算的路径, ...]}。

    代码里 include 常常只写基名（`#include "fw_app.h"`），光看字符串判断不出
    它属于哪一层；先建索引把基名映射回真实路径，才判得动。
    基名在多个目录同名时无法判断，报 LAY-AMBIG 而不是猜。
    """
    idx: dict[str, list[str]] = {}
    for p in REPO_ROOT.rglob("*.h"):
        if ".git" in p.parts:
            continue
        idx.setdefault(p.name, []).append(rel(p))
    return idx


IN_REPO_ROOTS = ("apps/", "api/", "common/", "framework/", "platform/")

def allowed_for(path: str) -> tuple[str, ...] | None:
    """返回该路径适用哪一行矩阵；多行命中取最长前缀。"""
    best: tuple[int, tuple[str, ...], str] | None = None
    for prefix, allowed, _ in MATRIX:
        if path.startswith(prefix) and (best is None or len(prefix) > best[0]):
            best = (len(prefix), allowed, prefix)
    return best[1] if best else None


def platform_board_allowed(path: str) -> tuple[str, ...] | None:
    """platform/<板>/ 每块板自成一行矩阵：可以 include 自己、port、fp_api、common。"""
    m = re.match(r"^platform/([^/]+)/", path)
    if not m:
        return None
    board = m.group(1)
    if board in {"port", "drivers"}:
        return None  # 由 MATRIX 管
    return (f"platform/{board}/", "platform/port/", "api/fp_api/", "common/")


def check(verbose: bool) -> Report:
    files = walk_files(SOURCE_SUFFIX, ("apps/", "api/", "common/", "framework/", "platform/"))
    rep = Report("check_layers", checked=False)
    if not files:
        return rep.skip("还没有 C 源文件")
    rep.checked = True

    index = header_index()
    for f in files:
        path = rel(f)
        allowed = allowed_for(path) or platform_board_allowed(path)
        if allowed is None:
            rep.warn("LAY-NOMATRIX", f"{path} 没有适用的 include 矩阵", hint="在 tools/check_layers.py 的 MATRIX 里补一行")
            continue
        text = f.read_text(encoding="utf-8", errors="replace")
        for m in INCLUDE_RE.finditer(text):
            header = m.group(1).replace("\\", "/")
            # 仓库内路径式的 include 直接判；只写基名的先去索引里查它属于哪一层；
            # 剩下的（stdio.h 之类）交给编译器。
            if not header.startswith(IN_REPO_ROOTS):
                owners = index.get(header.split("/")[-1], [])
                if not owners:
                    continue
                if len(owners) > 1:
                    rep.warn(
                        "LAY-AMBIG",
                        f"include 了 {header}，仓库里有多个同名头：{', '.join(sorted(owners))}",
                        path=path,
                        hint="改成从仓库根起算的完整路径，否则分层判定靠猜",
                    )
                    continue
                header = owners[0]
            if any(header.startswith(a) for a in allowed):
                continue
            line = text[: m.start()].count("\n") + 1
            rep.error(
                "LAY-CROSS",
                f"include 了 {header}，不在本层允许的范围内",
                path=path,
                line=line,
                hint=f"允许：{', '.join(allowed) or '（无）'}。跨层只能经 api/",
            )
    if verbose:
        for prefix, allowed, why in MATRIX:
            print(f"  {prefix:22s} ← {', '.join(allowed) or '（全部）'}   # {why}")
    return rep


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(prog="check_layers.py")
    ap.add_argument("--verbose", action="store_true")
    args = ap.parse_args(argv)
    try:
        rep = check(args.verbose)
    except RuntimeError as exc:
        print(f"check_layers: 环境问题 —— {exc}", file=sys.stderr)
        return EXIT_ENV
    rep.render()
    return EXIT_OK if rep.ok else EXIT_CODE


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
