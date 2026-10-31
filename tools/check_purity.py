"""check_purity.py —— 纯 C 边界：三个目录不认识任何目标 SDK。

规则来自 tools/purity_rules.txt（数据与逻辑分开，规则可逐条 review）。
依据 docs/code-structure.md §2.2：`common/` `api/` `framework/` 是标准 C11，
不 include 平台专有头、不出现 SDK / RTOS 的标识符；要 RTOS 原语只能
include `platform/port/pf_rtos.h`（我们自己的纯 C 头）。`apps/` 不在此列。

用法：
    python tools/check_purity.py
    python tools/check_purity.py --rules tools/purity_rules.txt

退出码：0 通过 / 1 有违规 / 2 环境有问题。
"""

from __future__ import annotations

import argparse
import re
import sys

from common import (
    EXIT_CODE,
    EXIT_ENV,
    EXIT_OK,
    REPO_ROOT,
    Report,
    rel,
    strip_c_comments,
    walk_files,
)

SOURCE_SUFFIX = (".c", ".h", ".cpp", ".hpp")

INCLUDE_RE = re.compile(r'^\s*#\s*include\s*[<"]([^>"]+)[>"]', re.M)


class Rules:
    def __init__(self) -> None:
        self.scopes: list[str] = []
        self.allow_include: list[str] = []
        self.forbid_include: list[str] = []
        self.forbid_token: set[str] = set()

    def load(self, path) -> None:
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
                raise RuntimeError(f"{rel(path)}:{lineno} 不认识的指令 {kind!r}")

    def allowed(self, header: str) -> bool:
        h = header.replace("\\", "/")
        return any(h == a or h.startswith(a) for a in self.allow_include)


def check() -> Report:
    rules_path = REPO_ROOT / "tools" / "purity_rules.txt"
    if not rules_path.is_file():
        raise RuntimeError("找不到 tools/purity_rules.txt")
    rules = Rules()
    rules.load(rules_path)

    files = walk_files(SOURCE_SUFFIX, tuple(rules.scopes))
    rep = Report("check_purity", checked=False)
    if not files:
        return rep.skip(f"{', '.join(rules.scopes)} 里还没有源文件")

    rep.checked = True
    for f in files:
        text = f.read_text(encoding="utf-8", errors="replace")
        path = rel(f)
        stripped = strip_c_comments(text)

        for m in INCLUDE_RE.finditer(text):  # include 要在原文里找（注释剥离会吃掉）
            header = m.group(1).replace("\\", "/")
            if rules.allowed(header):
                continue
            for bad in rules.forbid_include:
                if header.startswith(bad):
                    line = text[: m.start()].count("\n") + 1
                    rep.error(
                        "PUR-INC",
                        f"include 了平台专有头 {header}",
                        path=path,
                        line=line,
                        hint="三个纯 C 目录不认识目标 SDK。要 RTOS 原语走 platform/port/pf_rtos.h",
                    )

        if not rules.forbid_token:
            continue
        for m in re.finditer(r"\b[A-Za-z_]\w*\b", stripped):
            tok = m.group(0)
            if tok in rules.forbid_token:
                line = stripped[: m.start()].count("\n") + 1
                rep.error(
                    "PUR-TOK",
                    f"出现 SDK / RTOS 标识符 {tok}",
                    path=path,
                    line=line,
                    hint="命名与类型必须是我们自己的（cm_* / fw_* / pf_*）",
                )
    return rep


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(prog="check_purity.py")
    ap.add_argument("--rules", default=None, help="规则文件（默认 tools/purity_rules.txt）")
    ap.parse_args(argv)
    try:
        rep = check()
    except RuntimeError as exc:
        print(f"check_purity: 环境问题 —— {exc}", file=sys.stderr)
        return EXIT_ENV
    rep.render()
    return EXIT_OK if rep.ok else EXIT_CODE


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
