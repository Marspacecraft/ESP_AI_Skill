#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""工作区策略检查器 —— 步骤 0.5。

只判 **write 面**：某个身份（tag）能不能把实现写进某个路径。
link 面（能不能 include）要等 `clang -M` 的依赖闭包，排在 0.5 之后。

用法：
  python3 tools/check_workspace_policy.py --as 2 apps/demo/app_demo.c
  python3 tools/check_workspace_policy.py --as 2 --quiet apps/demo/app_demo.c
  python3 tools/check_workspace_policy.py --check      # 只检查 policy.txt 自身是否自洽
  python3 tools/check_workspace_policy.py --selftest   # 负向用例：证明这个检查器能红

退出码：0 = 允许 / 通过；1 = 拒绝 / 有问题；2 = 用法或环境错误（**2 不是通过**）。

判定顺序（policy.txt 里也写了）：
  1. 命中 `nobody` → 拒绝（只有人能改，任何身份都不行）
  2. 命中某条 `role` 的路径前缀 → 该身份在里面才允许，否则拒绝
  3. 谁都没覆盖 → 按 `default`（本仓库是 allow，即"没配置的目录不约束"）

注意：`nobody` 优先于 `role`。理由很简单——如果有人能把 `policy.txt`
写进某个 `role` 的前缀里，它就必须仍然写不了，否则门禁可以自己解除自己。
"""
from __future__ import annotations

import posixpath
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
POLICY = ROOT / "policy.txt"

# 身份编号沿用旧仓库：1 sysapp / 2 aiapp / 3 framework / 4 platform / 5 api
VALID_TAGS = {1, 2, 3, 4, 5}
KNOWN_DIRECTIVES = {"default", "role", "nobody", "link"}


class PolicyError(Exception):
    """policy.txt 写错了。属于"环境有问题"，退出码 2。"""


class Policy:
    def __init__(self) -> None:
        self.default_allow = True
        self.roles: dict[int, list[str]] = {}
        self.nobody: list[str] = []
        self.links: list[tuple[list[int], list[str]]] = []

    def owners(self, path: str) -> list[tuple[str, int]]:
        """返回覆盖该路径的全部 (entry, tag)。"""
        hits: list[tuple[str, int]] = []
        for tag, entries in self.roles.items():
            for entry in entries:
                if _covers(entry, path):
                    hits.append((entry, tag))
        return hits


def _covers(entry: str, path: str) -> bool:
    """entry 是目录（以 / 结尾）时做前缀匹配，否则做文件精确匹配。"""
    if entry.endswith("/"):
        return path == entry.rstrip("/") or path.startswith(entry)
    return path == entry


def _norm(p: str) -> str:
    if not p or p.startswith("/") or p.startswith("../") or "/../" in p:
        raise PolicyError(f"路径必须是相对仓库根的：{p!r}")
    trailing = p.endswith("/")
    q = posixpath.normpath(p).replace("\\", "/")
    if q in (".", ""):
        raise PolicyError(f"路径不能是仓库根：{p!r}")
    # normpath 会把 "common/" 收成 "common"，但"是不是目录"是策略里的一等信息
    #（目录=前缀匹配，文件=精确匹配），所以结尾的 / 必须留着。
    if trailing:
        q += "/"
    return q


def parse(text: str) -> Policy:
    pol = Policy()
    pending_role: int | None = None
    seen_default = 0

    for lineno, raw in enumerate(text.splitlines(), 1):
        where = f"policy.txt:{lineno}"

        # 注释与空行：整行丢弃。注意**先判续行**再判注释，
        # 因为续行是以空白开头的，而 `#` 开头的续行没有意义。
        stripped = raw.split("#", 1)[0].rstrip()
        if not stripped.strip():
            continue

        is_continuation = raw[:1].isspace()
        tokens = stripped.split()

        if is_continuation:
            if pending_role is None:
                raise PolicyError(f"{where}: 续行前面没有 role 行")
            for tok in tokens:
                pol.roles[pending_role].append(_norm(tok))
            continue

        # `default: allow` 与 `default:allow` 都认：冒号可能粘在指令上。
        head = tokens[0].rstrip(":")

        if head == "default":
            if ":" not in stripped:
                raise PolicyError(f"{where}: default 要写成 `default: allow` 或 `default: deny`")
            value = stripped.split(":", 1)[1].strip()
            if value not in ("allow", "deny"):
                raise PolicyError(f"{where}: default 只认 allow / deny，实际 {value!r}")
            pol.default_allow = value == "allow"
            seen_default += 1
            pending_role = None
            continue

        if head == "role":
            if len(tokens) < 3:
                raise PolicyError(f"{where}: role 行至少要 `role <编号> <名字> <路径...>`")
            try:
                tag = int(tokens[1])
            except ValueError:
                raise PolicyError(f"{where}: role 编号必须是整数，实际 {tokens[1]!r}") from None
            if tag not in VALID_TAGS:
                raise PolicyError(f"{where}: role 编号只认 1..5，实际 {tag}")
            if tag in pol.roles:
                raise PolicyError(f"{where}: role {tag} 重复定义（并集即可，不要写两行）")
            pol.roles[tag] = [_norm(tok) for tok in tokens[3:]]
            pending_role = tag
            continue

        if head == "nobody":
            if len(tokens) < 2:
                raise PolicyError(f"{where}: nobody 行至少要跟一个路径")
            pol.nobody.extend(_norm(tok) for tok in tokens[1:])
            pending_role = None
            continue

        if head == "link":
            if len(tokens) < 3:
                raise PolicyError(f"{where}: link 行至少要 `link <tag...> <路径...>`")
            # link 行第一个非 tag 的位置就是路径起点
            idx = 1
            tags: list[int] = []
            while idx < len(tokens) and tokens[idx].isdigit():
                t = int(tokens[idx])
                if t not in VALID_TAGS:
                    raise PolicyError(f"{where}: link 里的 tag 只认 1..5，实际 {t}")
                tags.append(t)
                idx += 1
            if not tags or idx >= len(tokens):
                raise PolicyError(f"{where}: link 行格式是 `link <tag...> <路径...>`")
            pol.links.append((tags, [_norm(tok) for tok in tokens[idx:]]))
            pending_role = None
            continue

        raise PolicyError(
            f"{where}: 不认识的指令 {head!r}（只认 {', '.join(sorted(KNOWN_DIRECTIVES))}）"
        )

    if seen_default == 0:
        raise PolicyError("policy.txt: 缺少 `default:` 行（没写就等于没说清未配置路径怎么办）")
    if seen_default > 1:
        raise PolicyError("policy.txt: `default:` 只能出现一次")
    return pol


def decide(pol: Policy, tag: int, path: str) -> tuple[bool, str]:
    """返回 (是否允许, 理由)。"""
    for entry in pol.nobody:
        if _covers(entry, path):
            return False, f"nobody：{entry} 只有人能改"

    hits = pol.owners(path)
    if hits:
        tags = sorted({t for _, t in hits})
        if tag in tags:
            mine = sorted({e for e, t in hits if t == tag})
            return True, f"role {tag}：" + " ".join(mine)
        who = " / ".join(f"role {t}" for t in tags)
        return False, f"属于 {who}；--as {tag} 不在其中"

    if pol.default_allow:
        return True, "未被任何 role 覆盖 → default allow"
    return False, "未被任何 role 覆盖 → default deny"


def check_structure(pol: Policy) -> list[str]:
    """policy.txt 自身是否自洽。返回问题列表（空 = 通过）。"""
    problems: list[str] = []

    # nobody 与 role 重叠 = 语义打架：一边说只有人能改，一边说某身份能写。
    for entry in pol.nobody:
        hits = pol.owners(entry)
        if hits:
            owners = " / ".join(f"role {t}" for _, t in hits)
            problems.append(f"{entry} 同时出现在 nobody 与 {owners} 里")

    for tag, entries in pol.roles.items():
        if not entries:
            problems.append(f"role {tag} 没有任何路径（要么补，要么删掉这行）")
        for entry in entries:
            if entry.startswith("/"):
                problems.append(f"role {tag}: 路径不能是绝对路径 {entry}")
            if ".." in entry.split("/"):
                problems.append(f"role {tag}: 路径不能含 .. ({entry})")

    for tags, paths in pol.links:
        for t in tags:
            if t not in pol.roles:
                problems.append(f"link 里出现未定义的 role {t}")
        for p in paths:
            if not p.endswith("/"):
                problems.append(f"link 的路径应写成目录（以 / 结尾）：{p}")

    return problems


def render(pol: Policy) -> None:
    print("=== policy.txt 解析结果 ===")
    print(f"default: {'allow' if pol.default_allow else 'deny'}")
    for tag in sorted(pol.roles):
        print(f"role {tag}: " + " ".join(pol.roles[tag]))
    print("nobody: " + (" ".join(pol.nobody) if pol.nobody else "(空)"))
    print("link:")
    for tags, paths in pol.links:
        print(f"  {','.join(map(str, tags))} -> " + " ".join(paths))
    print("（0.5 只判 write 面；link 面等 clang -M 的依赖闭包）")


def selftest() -> int:
    """负向用例：证明检查器真的会红。用子进程跑 CLI，顺带验证退出码。"""
    cases = [
        ("nobody_不能写_policy.txt", ["--as", "3", "policy.txt"], 1),
        ("tag2_不能写_tests_framework", ["--as", "2", "tests/framework/test_x.c"], 1),
        ("tag2_不能写_framework_src", ["--as", "2", "framework/src/fw_main.c"], 1),
        ("tag2_能写_apps_demo", ["--as", "2", "apps/demo/app_demo.c"], 0),
        ("tag4_能写_platform_host_fake", ["--as", "4", "platform/host-fake/pf_host_fake.c"], 0),
        ("未配置目录默认放行", ["--as", "2", "brand_new_dir/x.c"], 0),
    ]
    ok = bad = 0
    for name, args, want in cases:
        proc = subprocess.run(
            [sys.executable, str(Path(__file__).resolve()), *args],
            capture_output=True, text=True, cwd=str(ROOT),
        )
        if proc.returncode == want:
            ok += 1
            print(f"PASS {name}")
        else:
            bad += 1
            detail = (proc.stdout + proc.stderr).strip().replace("\n", " / ")
            print(f"FAIL {name} -- 期望 rc={want}, 实际 rc={proc.returncode} | {detail}")
    print(f"SUMMARY pass={ok} fail={bad} skip=0 policy_selftest")
    return 0 if bad == 0 else 1


def main(argv: list[str]) -> int:
    if "--selftest" in argv:
        return selftest()
    if "--check" in argv:
        try:
            pol = parse(POLICY.read_text(encoding="utf-8"))
        except (OSError, PolicyError) as e:
            print(f"policy 自检: FAIL -- {e}")
            return 2
        problems = check_structure(pol)
        if problems:
            for p in problems:
                print(f"  - {p}")
            print("policy 自检: FAIL")
            return 1
        print("policy 自检: PASS")
        return 0

    quiet = "--quiet" in argv
    tag: int | None = None
    paths: list[str] = []
    i = 1
    while i < len(argv):
        a = argv[i]
        if a == "--as":
            if i + 1 >= len(argv):
                print("用法错误：--as 后面要跟身份编号（1..5）", file=sys.stderr)
                return 2
            try:
                tag = int(argv[i + 1])
            except ValueError:
                print(f"用法错误：--as 需要一个整数，实际 {argv[i + 1]!r}", file=sys.stderr)
                return 2
            i += 2
            continue
        if a in ("--quiet", "--check", "--selftest"):
            i += 1
            continue
        if a.startswith("-"):
            print(f"用法错误：不认识的选项 {a!r}", file=sys.stderr)
            return 2
        paths.append(a)
        i += 1

    if tag is None:
        print("用法错误：缺 --as <1..5>（不知道你是谁，就判不了你能不能写）", file=sys.stderr)
        return 2
    if tag not in VALID_TAGS:
        print(f"用法错误：身份只认 1..5，实际 {tag}", file=sys.stderr)
        return 2
    if not paths:
        print("用法错误：至少给一个路径", file=sys.stderr)
        return 2

    try:
        pol = parse(POLICY.read_text(encoding="utf-8"))
    except (OSError, PolicyError) as e:
        print(f"check_workspace_policy: FAIL -- {e}", file=sys.stderr)
        return 2

    problems = check_structure(pol)
    if problems:
        # 策略文件自身有矛盾时**不许**下判断：那等于用一份自相矛盾的规则去判定别人。
        for p in problems:
            print(f"  - {p}", file=sys.stderr)
        print("check_workspace_policy: FAIL -- policy.txt 自身不自洽", file=sys.stderr)
        return 2

    denied = 0
    for raw in paths:
        try:
            path = _norm(raw)
        except PolicyError as e:
            print(f"用法错误：{e}", file=sys.stderr)
            return 2
        allowed, why = decide(pol, tag, path)
        if not allowed:
            denied += 1
        if not quiet:
            print(f"{'ALLOW' if allowed else 'DENY '}  {path:<34}  {why}")

    if not quiet:
        print(f"check_workspace_policy: --as {tag} → {len(paths)} 个路径，{denied} 个被拒")
    return 0 if denied == 0 else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
