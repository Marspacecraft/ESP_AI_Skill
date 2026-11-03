#!/usr/bin/env python3
"""check_purity.py —— 纯 C 边界：这些目录不认识任何目标 SDK。

规则来自 `tools/purity_rules.txt`（数据与判定分开，改规则是一次独立的小 diff）。
要回答的问题只有一条：

    `api/`、`common/`、`framework/`、`apps/` 里，有没有出现 SDK / RTOS 的痕迹？

为什么必须有它（而不是靠 `check_link.py`）：`link 面` 只看**仓库内**的 include 闭包，
`#include "freertos/FreeRTOS.h"` 是系统头，闭包里根本不会出现 → 一条越层安静通过。
这一份补的正是那个洞。

用法：
    python3 tools/check_purity.py
    python3 tools/check_purity.py --rules tools/purity_rules.txt
    python3 tools/check_purity.py --selftest      # 用临时目录里的假文件验判定本身

退出码：0 通过 / 1 有违规 / 2 环境或用法有问题（**2 不是通过**）。
"""

from __future__ import annotations

import argparse
import re
import sys
import tempfile
from dataclasses import dataclass, field
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
DEFAULT_RULES = REPO_ROOT / "tools" / "purity_rules.txt"
SOURCE_SUFFIX = (".c", ".h", ".cpp", ".hpp")

INCLUDE_RE = re.compile(r'^[ \t]*#[ \t]*include[ \t]*[<"]([^>"]+)[>"]', re.M)


@dataclass
class Rules:
    scopes: list = field(default_factory=list)
    allow_include: list = field(default_factory=list)
    forbid_include: list = field(default_factory=list)
    forbid_token: set = field(default_factory=set)

    def load(self, path: Path) -> None:
        for lineno, raw in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
            line = raw.strip()
            if not line or line.startswith("#"):
                continue
            parts = line.split()
            kind = parts[0]
            if kind == "scope":
                self.scopes.extend(parts[1:])
            elif kind == "allow_include":
                self.allow_include.extend(parts[1:])
            elif kind == "forbid_include":
                self.forbid_include.extend(parts[1:])
            elif kind == "forbid_token":
                self.forbid_token.update(parts[1:])
            else:
                raise RuntimeError(f"{path.name}:{lineno} 不认识的指令 {kind!r}")

    def allows(self, header: str) -> bool:
        return any(header.startswith(prefix) for prefix in self.allow_include)

    def forbidden_header(self, header: str):
        """返回命中的禁止前缀（没有则 None）。"""
        if self.allows(header):
            return None
        for prefix in self.forbid_include:
            if header.startswith(prefix):
                return prefix
        return None


def strip_c(text: str, drop_strings: bool = True) -> str:
    """把注释与字符串/字符字面量的内容换成空格，保留换行与偏移。

    保留偏移是为了违规行号还能对上——`line_of(offset)` 直接数换行即可。
    这一步不能省：注释里写 `// 不要用 vTaskDelay` 不该算违规，而注释掉的
    `// #include "freertos/FreeRTOS.h"` 同样不该算。
    """
    out = []
    i, n = 0, len(text)
    while i < n:
        ch = text[i]
        if ch == "/" and i + 1 < n and text[i + 1] == "/":
            while i < n and text[i] != "\n":
                out.append(" ")
                i += 1
            continue
        if ch == "/" and i + 1 < n and text[i + 1] == "*":
            out.append(" ")
            out.append(" ")
            i += 2
            while i + 1 < n and not (text[i] == "*" and text[i + 1] == "/"):
                out.append("\n" if text[i] == "\n" else " ")
                i += 1
            if i + 1 < n:
                out.append(" ")
                out.append(" ")
                i += 2
            continue
        if ch in ("'", '"'):
            if not drop_strings:
                out.append(ch)
                i += 1
                while i < n:
                    if text[i] == "\\" and i + 1 < n:
                        out.append(text[i])
                        out.append(text[i + 1])
                        i += 2
                        continue
                    out.append(text[i])
                    if text[i] == ch:
                        i += 1
                        break
                    i += 1
                continue
            quote = ch
            out.append(" ")
            i += 1
            while i < n:
                if text[i] == "\\" and i + 1 < n:
                    out.append(" \n" if text[i + 1] == "\n" else "  ")
                    i += 2
                    continue
                if text[i] == quote:
                    out.append(" ")
                    i += 1
                    break
                out.append("\n" if text[i] == "\n" else " ")
                i += 1
            continue
        out.append(ch)
        i += 1
    return "".join(out)


def line_of(text: str, offset: int) -> int:
    return text.count("\n", 0, offset) + 1


def check_file(path: Path, rules: Rules, root: Path):
    """返回 [(相对路径, 行号, 说明)]。"""
    raw = path.read_text(encoding="utf-8", errors="replace")
    code = strip_c(raw)
    rel = path.relative_to(root).as_posix()
    problems = []

    # `#include "x.h"` 里的路径本身就是字符串字面量——所以找 include 时**不能**剥离字符串，
    # 只剥注释（注释掉的 include 不该算）。找标识符时才剥字符串（`"vTaskDelay"` 不算违规）。
    code_no_comments = strip_c(raw, drop_strings=False)
    for m in INCLUDE_RE.finditer(code_no_comments):
        header = m.group(1)
        bad = rules.forbidden_header(header)
        if bad is not None:
            problems.append((rel, line_of(code_no_comments, m.start()), f"禁止的头 {header!r}（命中 {bad!r}）"))

    for token in sorted(rules.forbid_token):
        for m in re.finditer(r"\b" + re.escape(token) + r"\b", code):
            problems.append((rel, line_of(code, m.start()), f"禁止的标识符 {token!r}"))

    return problems


def scan(root: Path, rules: Rules):
    problems = []
    for scope in rules.scopes:
        base = root / scope
        if not base.is_dir():
            continue
        for path in sorted(base.rglob("*")):
            if path.is_file() and path.suffix in SOURCE_SUFFIX:
                problems.extend(check_file(path, rules, root))
    return problems


def selftest() -> int:
    """判定本身的正例/负例：这一步证明"检查器会红"，而不是"它总是绿"。"""
    cases = [
        ("bad_include.c", '#include "freertos/FreeRTOS.h"\n', 1, "禁止的头"),
        ("bad_token.c", "static void f(void) { vTaskDelay(1); }\n", 1, "禁止的标识符"),
        ("commented.c", '/* vTaskDelay */\n// #include "esp_log.h"\nstatic int x;\n', 0, ""),
        ("string.c", 'static const char *s = "freertos/FreeRTOS.h";\n', 0, ""),
        ("allowed.c", '#include "platform/port/pf_ops.h"\n#include "common/include/cm_types.h"\n', 0, ""),
    ]
    rules = Rules()
    rules.load(DEFAULT_RULES)
    passed = failed = 0
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        for name, body, want, want_kind in cases:
            (root / name).write_text(body, encoding="utf-8")
            got = check_file(root / name, rules, root)
            ok = len(got) == want and (not want or want_kind in got[0][2])
            if ok:
                print(f"PASS {name}")
                passed += 1
            else:
                print(f"FAIL {name} -- 期望 {want} 处（{want_kind or '无'}），实际 {len(got)} 处：{got}")
                failed += 1
    print(f"SUMMARY pass={passed} fail={failed} skip=0 purity_selftest")
    return 0 if failed == 0 else 1


def main(argv) -> int:
    ap = argparse.ArgumentParser(description="纯 C 边界检查")
    ap.add_argument("--rules", default=str(DEFAULT_RULES))
    ap.add_argument("--selftest", action="store_true", help="用临时假文件验判定本身")
    args = ap.parse_args(argv)

    if args.selftest:
        return selftest()

    rules_path = Path(args.rules)
    if not rules_path.is_file():
        print(f"check_purity: 找不到规则文件 {rules_path}", file=sys.stderr)
        return 2
    rules = Rules()
    try:
        rules.load(rules_path)
    except (RuntimeError, OSError) as exc:
        print(f"check_purity: {exc}", file=sys.stderr)
        return 2
    if not rules.scopes:
        print("check_purity: 规则里没有 scope", file=sys.stderr)
        return 2

    problems = scan(REPO_ROOT, rules)
    if not problems:
        print(f"纯 C 边界: PASS（{len(rules.scopes)} 个目录，{len(rules.forbid_include)} 条禁止头，"
              f"{len(rules.forbid_token)} 个禁止标识符）")
        return 0

    for rel, lineno, msg in problems:
        print(f"FAIL {rel}:{lineno} -- {msg}")
    print(f"纯 C 边界: FAIL（{len(problems)} 处）")
    return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
