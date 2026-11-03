#!/usr/bin/env python3
"""check_svc_ids.py —— 服务号与登记表的双向比对。

这一道门禁盯的是**一个只有运行期才会说话的地方**：`fw_svc_id_t` 是 `uint8_t`，
两个服务挑同一个号时，编译器不响、链接器不响，只有第二个服务启动时
`fw_svc_register()` 返回 `CM_ERR_BUSY`——而现场看起来像"这个服务偶尔起不来"。

能力位有 `check_caps.py` 盯着，服务号必须有同样的东西。

判据（四个方向）：

  A. 代码 → 表：`api/af_api/*.h` 里每个 `#define <X>_SVC_ID n`，表里必须有一行，
     且号一致；同一个号不许被两个宏占用。
  B. 表 → 代码：表里"服务接口头"一列写的文件必须存在，且该头里定义了表里那个号。
  C. 唯一性：表的 id 列本身不许重复。
  D. 范围：`0` 保留（`FW_SVC_NONE`）；本仓库只许用 `1–127`；`128–255` 属产品仓库，
     出现在这张表里就是错；`>255` 装不进 `uint8_t`。

退出码：0 通过 / 1 有问题 / 2 环境或用法问题（2 不是通过）。
"""
from __future__ import annotations

import re
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
API_DIR = Path("api/af_api")
TABLE = Path("docs/generated/svc-ids.md")

IN_REPO_MIN = 1
IN_REPO_MAX = 127
PRODUCT_MAX = 255
DASHES = {"—", "-", "--", ""}

# `#define DISP_SVC_ID 2u`（允许不带 u 后缀）
MACRO_RE = re.compile(r"^\s*#define\s+([A-Z][A-Z0-9_]*_SVC_ID)\s+(\d+)[uU]?\s*$", re.MULTILINE)


def parse_macros(root: Path) -> list[tuple[str, int, str]]:
    """扫 api/af_api/*.h，返回 [(宏名, 号, 相对路径)]。"""
    api = root / API_DIR
    if not api.is_dir():
        raise SystemExit(f"check_svc_ids: 找不到 {API_DIR}")
    out: list[tuple[str, int, str]] = []
    for header in sorted(api.glob("*.h")):
        text = header.read_text(encoding="utf-8")
        for name, num in MACRO_RE.findall(text):
            out.append((name, int(num), str(header.relative_to(root))))
    return out


def parse_table(root: Path) -> list[dict[str, object]]:
    """读登记表：id / 名字 / 接口头 / 行号。只认第一列是数字的行。"""
    path = root / TABLE
    if not path.is_file():
        raise SystemExit(f"check_svc_ids: 找不到 {TABLE}")
    rows: list[dict[str, object]] = []
    for lineno, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        if not line.startswith("|"):
            continue
        cells = [c.strip() for c in line.strip().strip("|").split("|")]
        if len(cells) < 3:
            continue
        if not re.fullmatch(r"\d+", cells[0]):
            continue
        rows.append({
            "id": int(cells[0]),
            "name": cells[1],
            "header": cells[2].strip("`") if len(cells) > 2 else "",
            "line": lineno,
        })
    return rows


def check(root: Path) -> list[str]:
    macros = parse_macros(root)
    rows = parse_table(root)
    problems: list[str] = []

    by_header = {h: [] for h in {r["header"] for r in rows if r["header"] not in DASHES}}

    # A + D：代码一侧
    seen: dict[int, str] = {}
    for name, num, where in macros:
        if num == 0:
            problems.append(f"`{where}` 里 `{name}` 用了 0——0 是 `FW_SVC_NONE`，永远不是有效服务")
        if num > PRODUCT_MAX:
            problems.append(f"`{where}` 里 `{name}` = {num}，装不进 `uint8_t`（上限 {PRODUCT_MAX}）")
        elif num > IN_REPO_MAX:
            problems.append(f"`{where}` 里 `{name}` = {num}，`{IN_REPO_MAX + 1}-{PRODUCT_MAX}` "
                            f"属产品仓库，本仓库只用 {IN_REPO_MIN}-{IN_REPO_MAX}")
        if num in seen and seen[num] != name:
            problems.append(f"服务号 {num} 被两个宏占用：`{seen[num]}` 与 `{name}`（`{where}`）")
        seen.setdefault(num, name)

    # B + C：表一侧
    table_ids: dict[int, int] = {}
    for row in rows:
        rid = int(row["id"])
        if rid in table_ids:
            problems.append(f"`{TABLE}` 第 {row['line']} 行：服务号 {rid} 重复登记"
                            f"（上一次在第 {table_ids[rid]} 行）")
        table_ids.setdefault(rid, int(row["line"]))
        if rid != 0 and not any(num == rid for _, num, _ in macros):
            problems.append(f"`{TABLE}` 第 {row['line']} 行登记了服务号 {rid}，"
                            f"但没有任何头定义它")

    # A（另一半）：代码里定义的号，表里必须有对应的一行
    for name, num, where in macros:
        if num not in table_ids:
            problems.append(f"`{where}` 里 `{name}` = {num}，但 `{TABLE}` 里没有登记这个号")

    # 表里声称有接口头的，必须真有那个头、且真有那个号
    for header in by_header:
        path = root / API_DIR / header
        if not path.is_file():
            problems.append(f"`{TABLE}` 声称接口头是 `{header}`，但 `{API_DIR / header}` 不存在")
            continue
        ids = [num for _, num, where in macros if Path(where).name == header]
        table_ids_for_header = {int(r["id"]) for r in rows if r["header"] == header}
        for tid in sorted(table_ids_for_header):
            if tid not in ids:
                problems.append(f"`{TABLE}` 说 `{header}` 是服务 {tid} 的接口头，"
                                f"但该头里没有号 {tid} 的 `*_SVC_ID` 宏")

    return problems


def selftest() -> int:
    """五条：一条该绿、四条该红。每条都在临时目录里搭最小现场。"""
    good_table = (
        "| id | 名字 | 服务接口头 | 实现者 | 备注 |\n"
        "|---:|------|-----------|--------|------|\n"
        "| 0 | （保留） | — | — | `FW_SVC_NONE` |\n"
        "| 2 | 显示 | `disp.h` | 显示服务 | x |\n"
    )
    good_header = "#define DISP_SVC_ID 2u\n"
    cases = [
        ("consistent_registry_passes", good_table, {"disp.h": good_header}, True),
        ("same_id_in_two_headers_is_caught",
         good_table, {"disp.h": good_header, "stor.h": "#define STOR_SVC_ID 2u\n"}, False),
        ("macro_missing_from_table_is_caught",
         "| id | 名字 | 服务接口头 | 实现者 | 备注 |\n|---:|------|-----------|--------|------|\n",
         {"disp.h": good_header}, False),
        ("table_claiming_a_missing_header_is_caught",
         good_table.replace("`disp.h`", "`disp_missing.h`"), {"disp.h": good_header}, False),
        ("product_range_id_is_caught",
         good_table, {"disp.h": "#define DISP_SVC_ID 200u\n"}, False),
    ]

    ok = 0
    bad = 0
    for name, table, headers, should_pass in cases:
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            (root / API_DIR).mkdir(parents=True, exist_ok=True)
            for fname, body in headers.items():
                (root / API_DIR / fname).write_text(body, encoding="utf-8")
            (root / TABLE).parent.mkdir(parents=True, exist_ok=True)
            (root / TABLE).write_text(table, encoding="utf-8")
            got = not check(root)
        if got == should_pass:
            print(f"PASS {name}")
            ok += 1
        else:
            print(f"FAIL {name} -- 期望{'通过' if should_pass else '变红'}，实际相反")
            bad += 1
    print(f"SUMMARY pass={ok} fail={bad} skip=0 svc_ids_selftest")
    return 0 if bad == 0 else 1


def main(argv: list[str]) -> int:
    if "--selftest" in argv:
        return selftest()

    try:
        problems = check(ROOT)
        count = len(parse_macros(ROOT))
    except SystemExit as exc:
        print(exc)
        return 2

    if problems:
        print(f"服务号: FAIL（{len(problems)} 处不一致）")
        for p in problems:
            print(f"  - {p}")
        return 1
    print(f"服务号: PASS（{count} 个服务，与 {TABLE} 双向一致）")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
