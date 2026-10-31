"""gen_catalog.py —— 从 api/ + 服务注册生成能力目录。

能力目录回答一个问题："这块板能做什么、对应哪个头、由谁实现。"
它必须从代码生成，不能手写——手写的目录会和代码静默漂移，
而那正是 check_catalog.py 要防的事。

生成物形态（markdown 表格）：
    | 能力 | 头文件 | 注册的服务 | 状态 |

用法：
    python tools/gen_catalog.py                 # 打到 stdout
    python tools/gen_catalog.py --out docs/generated/catalog.md

退出码：0 成功 / 1 数据有问题 / 2 环境问题。
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

from common import EXIT_CODE, EXIT_ENV, EXIT_OK, REPO_ROOT, rel, walk_files

# 基础头：不是能力，是 app 面向的基础设施
BASE_HEADERS = {"api.h", "app.h", "log.h", "res.h", "cap.h"}

SVC_RE = re.compile(r"fw_svc_register\s*\(\s*([A-Za-z_]\w*)")


def capability_headers() -> list[str]:
    d = REPO_ROOT / "api" / "af_api"
    if not d.is_dir():
        return []
    return sorted(p.stem for p in d.glob("*.h") if p.name not in BASE_HEADERS)


def registrations() -> dict[str, set[str]]:
    """{服务 id: {注册它的 .c 路径}}。"""
    out: dict[str, set[str]] = {}
    for f in walk_files((".c",), ("apps/", "framework/", "platform/")):
        text = f.read_text(encoding="utf-8", errors="replace")
        for m in SVC_RE.finditer(text):
            out.setdefault(m.group(1), set()).add(rel(f))
    return out


def render_md() -> str:
    caps = capability_headers()
    regs = registrations()
    lines = [
        "# 能力目录（生成物，勿手改）",
        "",
        "> 由 `python tools/gen_catalog.py` 生成。手改会在下次生成时被覆盖，",
        "> 而且 `check_catalog.py` 会把「生成物 ↔ 实际注册」的漂移判成失败。",
        "",
        "| 能力 | 头文件 | 注册的服务 | 注册位置 |",
        "|---|---|---|---|",
    ]
    if not caps and not regs:
        lines.append("| （无） | —— | —— | 还没有任何能力头，也没有服务注册 |")
    for cap in caps:
        svc = f"SVC_{cap.upper()}"
        where = ", ".join(sorted(regs.get(svc, set()))) or "**未注册**"
        lines.append(f"| {cap} | `api/af_api/{cap}.h` | `{svc}` | {where} |")
    extra = sorted(set(regs) - {f"SVC_{c.upper()}" for c in caps})
    for svc in extra:
        lines.append(f"| （无对应头） | —— | `{svc}` | {', '.join(sorted(regs[svc]))} |")
    lines.append("")
    lines.append(f"共 {len(caps)} 个能力头、{len(regs)} 个服务注册。")
    return "\n".join(lines) + "\n"


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(prog="gen_catalog.py")
    ap.add_argument("--out", default=None, help="写到这个路径（默认 stdout）")
    args = ap.parse_args(argv)
    text = render_md()
    if args.out:
        out = REPO_ROOT / args.out
        out.parent.mkdir(parents=True, exist_ok=True)
        out.write_text(text, encoding="utf-8")
        print(f"已写入 {rel(out)}（{len(text.splitlines())} 行）")
        print("注意：docs/ 归 tag 5，生成物一般放 build/ 或由人决定落点")
    else:
        sys.stdout.write(text)
    return EXIT_OK


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
