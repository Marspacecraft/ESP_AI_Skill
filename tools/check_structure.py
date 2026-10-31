"""check_structure.py —— 目录树与 policy.txt 的一致性。

两件事（docs/code-structure.txt §0 记着这条缺口）：
  1. 每个已存在的顶层目录，必须被某个 role 前缀覆盖（否则谁都写不了）；
  2. 每条 policy 前缀都必须出现在 docs/code-structure.md 的目录树里
     —— 改了一边忘另一边，是这份文档最容易漂的地方。

不做的：逐行解析目录树的归属标注。树里的注释是给人看的，
机器只要知道"这条前缀有没有被记录过"就够了。

关于第 2 条的匹配方式：目录树是用 `├─`/`└─` 画出来的，子目录只写自己那一段
（`platform/` 下面才有 `metalio_eink_4_plus/`），所以不能直接拿
`platform/metalio_eink_4_plus/` 去全文搜索。判据放宽成：
  完整路径出现  或  （父路径出现 且 叶子名出现）。
这样"某块板的名字从树里消失了"仍然抓得到，但不会因为树的写法而误报。

用法：
    python tools/check_structure.py
    python tools/check_structure.py --verbose

退出码：0 通过 / 1 有漂移 / 2 环境问题。
"""

from __future__ import annotations

import argparse
import sys

from common import EXIT_CODE, EXIT_ENV, EXIT_OK, REPO_ROOT, Report, load_policy, top_level_dirs

DOC = "docs/code-structure.md"
# 顶层目录里不该参与"必须有人能写"判定的：版本库、构建产物、解释器缓存、
# 以及编辑器/同步工具自己建的隐藏目录（.obsidian/.vscode/…）。


def _normalize(text: str) -> str:
    """把目录树的画线字符去掉，让 `├─ platform/` 变成 `platform/`。"""
    for ch in "│├└─┬┼┤├┘┐┌":
        text = text.replace(ch, " ")
    return text


def _mentioned(prefix: str, text: str) -> bool:
    """前缀是否被这份文档记录过（见文件头的匹配方式）。"""
    needle = prefix.rstrip("/")
    if needle in text:
        return True
    parts = needle.split("/")
    if len(parts) < 2:
        return False
    parent = "/".join(parts[:-1])
    leaf = parts[-1]
    return parent in text and leaf in text


def check(verbose: bool) -> Report:
    policy = load_policy()
    rep = Report("check_structure", checked=True)

    doc = REPO_ROOT / DOC
    doc_text = doc.read_text(encoding="utf-8", errors="replace") if doc.is_file() else ""
    if not doc_text:
        rep.warn("STR-NODOC", f"{DOC} 不存在或为空，无法做「前缀是否被记录」的比对")
    doc_flat = _normalize(doc_text)

    # 1. 顶层目录必须有写者——但"有 read 行、没有 role 行"是合法状态：
    #    那是**只读给人看、只有人能写**的路径（`skills/` 就是这样：考卷不能被应试者改）。
    for d, nfiles in top_level_dirs():
        writers = policy.writers_of(d)
        if not writers:
            readers = policy.readers_of(d)
            if readers and nfiles > 0:
                if verbose:
                    print(f"  {d:20s} {nfiles:4d} 文件  ← 只有人能写（AI 只读）")
                continue
            if nfiles == 0:
                rep.warn(
                    "STR-ORPHAN-EMPTY",
                    f"顶层空目录 {d} 没有任何 role 前缀覆盖",
                    hint="空目录只是残骸（多半是拼错或建错的），删掉它；真要保留就在 policy.txt 里给个写者",
                )
            else:
                rep.error(
                    "STR-ORPHAN",
                    f"顶层目录 {d}（{nfiles} 个文件）没有任何 role 前缀覆盖，也没有 read 行",
                    hint="人在 policy.txt 的某条 role 行尾加这个前缀（写者）或某条 read 行加它（只读、人维护）。"
                         "如果不是代码（如笔记库），把它移出仓库或加进 common.IGNORED_TOP",
                )
        elif verbose:
            print(f"  {d:20s} {nfiles:4d} 文件  ← {', '.join(policy.role_names(writers))}")

    # 2. 每条 policy 前缀都要在目录树文档里出现过
    if doc_text:
        for r in policy.roles:
            for prefix in r.prefixes:
                if not _mentioned(prefix, doc_flat):
                    rep.error(
                        "STR-UNDOC",
                        f"policy 里的前缀 {prefix} 在 {DOC} 里找不到",
                        hint="目录树是「代码放哪」的唯一说明；加了前缀就要在树里加一行（或反之）",
                    )
        for rule in policy.reads:
            if not _mentioned(rule.prefix, doc_flat):
                rep.warn("STR-UNDOC-READ", f"read 前缀 {rule.prefix} 在 {DOC} 里找不到")

    return rep


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(prog="check_structure.py")
    ap.add_argument("--verbose", action="store_true")
    args = ap.parse_args(argv)
    try:
        rep = check(args.verbose)
    except RuntimeError as exc:
        print(f"check_structure: 环境问题 —— {exc}", file=sys.stderr)
        return EXIT_ENV
    rep.render()
    return EXIT_OK if rep.ok else EXIT_CODE


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
