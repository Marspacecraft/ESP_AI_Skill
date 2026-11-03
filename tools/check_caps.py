#!/usr/bin/env python3
"""check_caps.py —— 能力位与能力目录的双向比对。

这一道门禁盯的是一个**只会安静漂移**的地方：`platform/port/pf_ops.h` 里定义了
`PF_CAP_*`，`docs/generated/catalog.md` 里登记了它们，`api/af_api/` 里可能有对应的
接口头。三处少写一处、位号写错、或者"目录里声称有一个头而那个头不存在"，
编译器都不会说话——只有这里会。

判据（两个方向都要成立）：

  A. 宏 → 目录：`pf_ops.h` 里每个 `PF_CAP_*` 宏，目录里必须有一行，且位号一致。
  B. 目录 → 宏：目录里每一行引用的宏必须在 `pf_ops.h` 里存在，且位号一致。
  C. 目录 → 文件：目录"接口头"一列如果不是 `—`，那个文件必须真的存在。
  D. 头 → 目录：`api/af_api/` 下除基础设施头以外，每个头都必须在目录里被登记。

位 9–31 保留（`PF_CAP_RESERVED_MASK`），任何 `PF_CAP_*` 落到这些位上都是错误。

退出码：0 通过 / 1 有问题 / 2 环境或用法问题（2 不是通过）。
"""
from __future__ import annotations

import re
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
OPS_H = Path("platform/port/pf_ops.h")
CATALOG = Path("docs/generated/catalog.md")
API_DIR = Path("api/af_api")

# 基础设施头：不是"能力"，因此不需要能力位。
NONCAP_HEADERS = {
    "app.h",   # 生命周期 + 使用面
    "res.h",   # 资源与配额
    "evt.h",   # 事件
    "svc.h",   # 服务注册表
}

RESERVED_FIRST_BIT = 9
DASHES = {"—", "-", "--", ""}


def parse_macros(root: Path) -> dict[str, int]:
    """从 pf_ops.h 读出 {宏后缀: 位号}。`PF_CAP_RESERVED_MASK` 不是能力，天然不匹配。"""
    path = root / OPS_H
    if not path.is_file():
        raise SystemExit(f"check_caps: 找不到 {OPS_H}")
    text = path.read_text(encoding="utf-8")
    out: dict[str, int] = {}
    for name, bit in re.findall(r"^\s*#define\s+PF_CAP_([A-Z0-9_]+)\s+\(1u\s*<<\s*(\d+)u?\)",
                                text, re.MULTILINE):
        out[name] = int(bit)
    return out


def parse_catalog(root: Path) -> list[dict[str, object]]:
    """读出目录表的行：[{bit, name, macro, header}]。"""
    path = root / CATALOG
    if not path.is_file():
        raise SystemExit(f"check_caps: 找不到 {CATALOG}")
    rows: list[dict[str, object]] = []
    for line in path.read_text(encoding="utf-8").splitlines():
        line = line.strip()
        if not line.startswith("|"):
            continue
        cells = [c.strip() for c in line.strip("|").split("|")]
        if len(cells) < 4 or not re.fullmatch(r"\d+", cells[0]):
            continue
        bit = int(cells[0])
        macro = cells[2].strip("`")
        if not macro.startswith("PF_CAP_"):
            continue
        rows.append({
            "bit": bit,
            "name": cells[1],
            "macro": macro[len("PF_CAP_"):],
            "header": cells[3].strip("`"),
        })
    return rows


def check(root: Path) -> list[str]:
    """返回违规描述列表；空表示通过。"""
    macros = parse_macros(root)
    rows = parse_catalog(root)
    problems: list[str] = []

    by_name: dict[str, int] = {}
    for r in rows:
        name = str(r["macro"])
        if name in by_name:
            problems.append(f"目录里 `PF_CAP_{name}` 出现了两次（位 {by_name[name]} 与 {r['bit']}）")
        by_name[name] = int(r["bit"])  # type: ignore[arg-type]
        if int(r["bit"]) >= RESERVED_FIRST_BIT:  # type: ignore[arg-type]
            problems.append(f"目录里 `PF_CAP_{name}` 占用保留位 {r['bit']}（9–31 必须为 0）")

    # A. 宏 → 目录
    for name, bit in sorted(macros.items(), key=lambda kv: kv[1]):
        if name not in by_name:
            problems.append(f"`PF_CAP_{name}`（位 {bit}）在 pf_ops.h 里定义了，但目录里没有这一行")
        elif by_name[name] != bit:
            problems.append(f"`PF_CAP_{name}` 位号不一致：pf_ops.h 是 {bit}，目录写 {by_name[name]}")

    # B. 目录 → 宏
    for r in rows:
        name = str(r["macro"])
        if name not in macros:
            problems.append(f"目录登记了 `PF_CAP_{name}`（位 {r['bit']}），但 pf_ops.h 里没有这个宏")

    # C. 目录 → 文件
    for r in rows:
        hdr = str(r["header"])
        if hdr in DASHES:
            continue
        if not (root / hdr).is_file():
            problems.append(f"目录说 `PF_CAP_{r['macro']}` 的接口头是 `{hdr}`，但文件不存在")

    # D. 头 → 目录
    api = root / API_DIR
    if api.is_dir():
        claimed = {str(r["header"]) for r in rows}
        for f in sorted(api.glob("*.h")):
            rel = f"{API_DIR}/{f.name}"
            if f.name in NONCAP_HEADERS:
                continue
            if rel not in claimed:
                problems.append(f"`{rel}` 是一个能力头，但目录里没有任何一行登记它")

    return problems


def selftest() -> int:
    """六条：一条该绿、五条该红。每条都在临时目录里搭最小现场。"""
    base_ops = "#define PF_CAP_DISPLAY (1u << 0)\n#define PF_CAP_KEY (1u << 3)\n"
    base_cat = (
        "| bit | 名字 | 宏 | 接口头 | 说明 |\n"
        "|----:|------|----|--------|------|\n"
        "| 0 | DISPLAY | `PF_CAP_DISPLAY` | — | x |\n"
        "| 3 | KEY | `PF_CAP_KEY` | — | x |\n"
    )
    cases = [
        ("consistent_catalog_passes", base_ops, base_cat, None, True),
        ("macro_missing_from_catalog_is_caught",
         base_ops + "#define PF_CAP_NET (1u << 5)\n", base_cat, None, False),
        # 目录里登记了 KEY，但 pf_ops.h 没有这个宏 —— 反过来也要拦。
        ("catalog_row_without_macro_is_caught",
         "#define PF_CAP_DISPLAY (1u << 0)\n", base_cat, None, False),
        ("bit_mismatch_is_caught",
         base_ops, base_cat.replace("| 3 | KEY", "| 4 | KEY"), None, False),
        ("catalog_claiming_a_missing_header_is_caught",
         base_ops, base_cat.replace("| 3 | KEY | `PF_CAP_KEY` | — |",
                                    "| 3 | KEY | `PF_CAP_KEY` | `api/af_api/disp.h` |"), None, False),
        ("unregistered_capability_header_is_caught",
         base_ops, base_cat, "api/af_api/disp.h", False),
    ]

    ok = 0
    bad = 0
    for name, ops, cat, extra, should_pass in cases:
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            (root / OPS_H).parent.mkdir(parents=True, exist_ok=True)
            (root / OPS_H).write_text(ops, encoding="utf-8")
            (root / CATALOG).parent.mkdir(parents=True, exist_ok=True)
            (root / CATALOG).write_text(cat, encoding="utf-8")
            (root / API_DIR).mkdir(parents=True, exist_ok=True)
            (root / API_DIR / "app.h").write_text("/* infra */\n", encoding="utf-8")
            if extra:
                (root / extra).write_text("/* cap */\n", encoding="utf-8")
            got = not check(root)
        if got == should_pass:
            print(f"PASS {name}")
            ok += 1
        else:
            print(f"FAIL {name} -- 期望{'通过' if should_pass else '变红'}，实际相反")
            bad += 1
    print(f"SUMMARY pass={ok} fail={bad} skip=0 caps_selftest")
    return 0 if bad == 0 else 1


def main(argv: list[str]) -> int:
    if "--selftest" in argv:
        return selftest()

    problems = check(ROOT)
    if problems:
        print(f"能力目录: FAIL（{len(problems)} 处不一致）")
        for p in problems:
            print(f"  - {p}")
        return 1
    macros = parse_macros(ROOT)
    print(f"能力目录: PASS（{len(macros)} 个能力位，与 docs/generated/catalog.md 双向一致）")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
