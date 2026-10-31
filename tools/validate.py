"""validate.py —— 一次跑完全部门禁。

设计要点：**不因前一项失败而中断**。一次跑完拿到完整清单，
比"修一个再跑一次"省得多；而且有些失败是同一处引起的，
分开看浪费时间。

顺序（先便宜后贵的）：
    1. policy.py --check        数据自检
    2. build_check              目录/构建入口存在性（不阻塞）
    3. check_structure          目录树 ↔ policy
    4. check_purity             纯 C 边界
    5. check_layers             include 矩阵
    6. check_policy --all --as  三条判定 + 附加检查（需要身份）
    7. check_catalog            能力目录 ↔ 注册
    8. check_blocking           任务体伪阻塞
    9. check_headers            头文件自包含（要编译，最慢）
   10. tests/test.py            host 单测（唯一入口，最慢）

用法：
    python tools/validate.py                 # 本地：未声明身份时写入判定打印 SKIP
    python tools/validate.py --as 2          # 以 tag 2 的身份跑
    python tools/validate.py --as 2 --strict # SKIP 也算失败（CI 用）
    python tools/validate.py --only purity,layers

退出码：0 全部通过 / 1 有问题 / 2 环境坏了（两者都有时优先报 1）。
"""

from __future__ import annotations

import argparse
import subprocess
import sys
import time
from dataclasses import dataclass
from pathlib import Path

from common import EXIT_CODE, EXIT_ENV, EXIT_OK, REPO_ROOT, run

TOOLS = REPO_ROOT / "tools"


@dataclass
class Step:
    name: str
    cmd: list[str]
    skippable: bool = False


def steps(ident: str | None, strict: bool) -> list[Step]:
    py = sys.executable
    out = [
        Step("policy 数据自检", [py, str(TOOLS / "policy.py"), "check"]),
        Step("目录树 ↔ policy", [py, str(TOOLS / "check_structure.py")]),
        Step("纯 C 边界", [py, str(TOOLS / "check_purity.py")]),
        Step("include 矩阵", [py, str(TOOLS / "check_layers.py")]),
    ]
    pol_cmd = [py, str(TOOLS / "check_policy.py"), "--all"]
    if ident:
        pol_cmd += ["--as", ident]
    if strict:
        pol_cmd.append("--strict")
    out.append(Step("三条权限判定", pol_cmd))
    out += [
        Step("能力目录 ↔ 注册", [py, str(TOOLS / "check_catalog.py")]),
        Step("任务体伪阻塞", [py, str(TOOLS / "check_blocking.py")]),
        Step("头文件自包含", [py, str(TOOLS / "check_headers.py")]),
        Step("host 单测", [py, str(REPO_ROOT / "tests" / "test.py")]),
    ]
    return out


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(prog="validate.py", description="全部 host 门禁")
    ap.add_argument("--as", dest="ident", metavar="<tag|human>", help="以谁的身份判定写入")
    ap.add_argument("--strict", action="store_true", help="把 SKIP 也当失败（CI 用）")
    ap.add_argument("--only", default=None, help="只跑这些（逗号分隔的子串匹配）")
    ap.add_argument("--list", action="store_true", help="列出步骤名后退出")
    args = ap.parse_args(argv)

    plan = steps(args.ident, args.strict)
    if args.only:
        keys = [k.strip() for k in args.only.split(",") if k.strip()]
        plan = [s for s in plan if any(k in s.name for k in keys)]
    if args.list:
        for s in plan:
            print(s.name)
        return EXIT_OK

    print(f"仓库：{REPO_ROOT}")
    print(f"身份：{args.ident or '（未声明）'}{'  strict' if args.strict else ''}")
    print("-" * 68)

    results: list[tuple[str, int, float, str]] = []
    for s in plan:
        t0 = time.time()
        try:
            cp = run(s.cmd, cwd=REPO_ROOT)
            rc, body = cp.returncode, (cp.stdout or "") + (cp.stderr or "")
        except OSError as exc:  # 解释器都起不来，算环境问题
            rc, body = EXIT_ENV, f"无法启动 {s.cmd[0]}：{exc}"
        dt = time.time() - t0
        results.append((s.name, rc, dt, body.strip()))
        print(f"\n### {s.name}   rc={rc}  {dt:.1f}s")
        if body.strip():
            print(body.rstrip())

    print("\n" + "=" * 68)
    print(f"{'步骤':28s} {'结果':6s} {'耗时':>7s}")
    print("-" * 68)
    worst = EXIT_OK
    for name, rc, dt, _ in results:
        label = {0: "PASS", 1: "FAIL", 2: "ENV"}.get(rc, f"rc{rc}")
        print(f"{name:28s} {label:6s} {dt:6.1f}s")
        if rc == EXIT_CODE:
            worst = EXIT_CODE
        elif rc == EXIT_ENV and worst == EXIT_OK:
            worst = EXIT_ENV

    print("-" * 68)
    if worst == EXIT_OK:
        skipped = sum(1 for _, _, _, b in results if "SKIP" in b)
        print("validate: PASS" + (f"（有 {skipped} 步 SKIP —— 那是无东西可跑，不是通过）" if skipped else ""))
    elif worst == EXIT_CODE:
        print("validate: FAIL  有检查没过；见上面的明细")
    else:
        print("validate: ENV   环境坏了（缺编译器/git/nm 之类）。2 不是通过")
    return worst


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
