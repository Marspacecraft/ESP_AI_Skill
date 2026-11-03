#!/usr/bin/env python3
"""link 面门禁：一个头能被谁 `#include`，要看它**闭包**里有没有别人的私有物。

为什么需要这一条：`policy.txt` 里写的 `link 1 2 3 api/af_api/` 只说明"App 可以
include `api/af_api/*.h`"。但如果 `af_api/app.h` 顺手 include 了
`platform/port/pf_ops.h`，App 的编译单元里就照样出现了平台表 —— 写面管住了，
读面被"抄近路"绕过。所以判据必须是**依赖闭包**，不是单个文件。

判据（对每个 tag T、每个 T 有权 link 的头 H）：

    closure(H) ∩ (repo 内所有文件)  ⊆  link_set(T)

`closure` 用 `clang -M` 取，取的是预处理之后的真实依赖（`#include_next`、
相对路径、条件编译都对），不是正则。

退出码：0 通过 / 1 有越界 / 2 环境或用法问题（**2 不是通过**）。

用法：
    python3 tools/check_link.py            # 全量
    python3 tools/check_link.py --selftest # 自检（含一条必红用例）
"""
from __future__ import annotations

import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
POLICY = REPO / "policy.txt"

# 头文件后缀：只有这些会进闭包判定（.c 是消费者，不判）。
HEADER_SUFFIXES = (".h", ".hpp")

# 这些目录下的头**不对外**，不参与"谁能 link 它"的判定。
PRIVATE_PREFIXES = ("tests/",)


class PolicyError(Exception):
    """策略文件自相矛盾或语法不对。"""


def parse_link_lines(policy_path: Path) -> dict[str, list[str]]:
    """读出 `link <tags...> <paths...>`，返回 {tag: [路径前缀]}。

    tags 是数字（1–5），第一个非数字的 token 起是路径。路径按目录前缀匹配，
    与 write 面同一套语义。
    """
    out: dict[str, list[str]] = {}
    try:
        text = policy_path.read_text(encoding="utf-8")
    except OSError as exc:
        raise PolicyError(f"读不到 {policy_path}: {exc}") from exc

    for lineno, raw in enumerate(text.splitlines(), 1):
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        tokens = line.split()
        if tokens[0] != "link":
            continue
        tags: list[str] = []
        paths: list[str] = []
        for tok in tokens[1:]:
            if not paths and re.fullmatch(r"\d+", tok):
                tags.append(tok)
            else:
                paths.append(tok)
        if not tags or not paths:
            raise PolicyError(f"policy.txt:{lineno} link 行缺 tags 或缺路径：{line!r}")
        for tag in tags:
            out.setdefault(tag, []).extend(paths)
    if not out:
        raise PolicyError("policy.txt 里没有任何 link 行")
    return out


def link_set_for(tag: str, links: dict[str, list[str]]) -> list[str]:
    """某个 tag 的可链接前缀集合（按 policy.txt 的顺序，便于报错时引用）。"""
    return links.get(tag, [])


def allowed_by(path: str, prefixes: list[str]) -> bool:
    """`path` 是否落在某个前缀下。目录前缀以 `/` 结尾 = 前缀匹配。"""
    for prefix in prefixes:
        if prefix.endswith("/"):
            if path.startswith(prefix):
                return True
        elif path == prefix:
            return True
    return False


def repo_headers() -> list[str]:
    """仓库里所有**对外**头文件（repo 相对路径）。"""
    out: list[str] = []
    for dirpath, dirnames, filenames in os.walk(REPO):
        dirnames[:] = [d for d in dirnames if d not in ("build", ".git", "__pycache__")]
        for name in filenames:
            if not name.endswith(HEADER_SUFFIXES):
                continue
            rel = Path(dirpath, name).relative_to(REPO).as_posix()
            if rel.startswith(PRIVATE_PREFIXES):
                continue
            out.append(rel)
    return sorted(out)


def closure(header: str) -> tuple[set[str], str]:
    """用 `clang -M` 取一个头的依赖闭包（只保留仓库内文件）。

    返回 (仓库内文件集合, 原始输出)。`clang` 不在 PATH 上时抛 RuntimeError。
    """
    clang = shutil.which("clang") or shutil.which("cc")
    if clang is None:
        raise RuntimeError("找不到 clang/cc：闭包判定需要它")

    cmd = [clang, "-M", "-I.", "-Icommon/include", "-Itests", header]
    proc = subprocess.run(cmd, cwd=REPO, capture_output=True, text=True)
    if proc.returncode != 0:
        raise RuntimeError(f"clang -M 失败（{header}）：\n{proc.stderr.strip()}")

    deps: set[str] = set()
    for token in proc.stdout.replace("\\\n", " ").split():
        token = token.rstrip(":")
        if not token or token == header:
            continue
        candidate = (REPO / token).resolve()
        if not str(candidate).startswith(str(REPO)):
            continue  # 系统头/第三方头不参与判定
        if not candidate.is_file():
            continue  # `clang -M` 的第一行是目标文件（`xxx.o:`），不是依赖
        deps.add(candidate.relative_to(REPO).as_posix())
    deps.add(header)
    return deps, proc.stdout


def check(links: dict[str, list[str]], headers: list[str], quiet: bool = False) -> int:
    """跑一遍全量判定，返回退出码。"""
    violations: list[str] = []
    cache: dict[str, set[str]] = {}

    for tag in sorted(links, key=int):
        prefixes = link_set_for(tag, links)
        for header in headers:
            if not allowed_by(header, prefixes):
                continue  # 这个 tag 本来就 link 不到它，不用判它的闭包
            if header not in cache:
                deps, _ = closure(header)
                cache[header] = deps
            for dep in sorted(cache[header]):
                if not allowed_by(dep, prefixes):
                    violations.append(
                        f"tag {tag} 可以 link `{header}`，但它的闭包里有 `{dep}`，"
                        f"而 tag {tag} 的 link 集合是 {prefixes}"
                    )

    if violations:
        print(f"link 面: FAIL（{len(violations)} 处越界）")
        for item in violations:
            print(f"  越界: {item}")
        return 1

    if not quiet:
        seen = len({h for h in headers})
        print(f"link 面: PASS（{len(links)} 个 tag，{seen} 个对外头，闭包逐条包含关系成立）")
    return 0


def selftest() -> int:
    """自检：用自己的判定函数跑三条真命题 + 一条必红。"""
    cases: list[tuple[str, bool]] = []

    links = {"2": ["api/af_api/", "common/", "framework/include/"]}
    cases.append(
        ("App 能 link fw_app.h 且闭包只在其集合内",
         check(links, ["framework/include/fw_app.h"], quiet=True) == 0)
    )

    # 必红：把平台契约擦掉，fw_app.h 的闭包（只有 cm_types.h）仍在集合内 —— 这条是**绿**的，
    # 所以真正的红证要反过来：假装 fw_app.h 依赖 pf_ops.h。
    links_bad = {"2": ["api/af_api/", "framework/include/"]}  # 少了 common/
    cases.append(
        ("漏掉 common/ 时 fw_app.h 必红",
         check(links_bad, ["framework/include/fw_app.h"], quiet=True) == 1)
    )

    cases.append(
        ("tag 4 能 link pf_ops.h，闭包只有它自己",
         check({"4": ["platform/port/"]}, ["platform/port/pf_ops.h"], quiet=True) == 0)
    )

    cases.append(
        ("没有 link 行的 tag 不会被误判",
         check({"5": ["docs/"]}, ["framework/include/fw_app.h"], quiet=True) == 0)
    )

    failed = 0
    for name, ok in cases:
        if ok:
            print(f"PASS {name}")
        else:
            print(f"FAIL {name}")
            failed += 1
    print(f"SUMMARY pass={len(cases) - failed} fail={failed} skip=0 link_selftest")
    return 0 if failed == 0 else 1


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description="link 面门禁（依赖闭包）")
    parser.add_argument("--selftest", action="store_true", help="跑判定器自检")
    args = parser.parse_args(argv)

    if args.selftest:
        try:
            return selftest()
        except RuntimeError as exc:
            print(f"ERROR: {exc}", file=sys.stderr)
            return 2

    try:
        links = parse_link_lines(POLICY)
    except PolicyError as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 2

    try:
        return check(links, repo_headers())
    except RuntimeError as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
