"""check_catalog.py —— 能力目录与代码的双向比对。

为什么需要它：能力目录一旦手写，就会和代码静默漂移——头文件删了、
服务不注册了，而目录还写着"支持"。这个检查把两边对齐：
  头部有 api/af_api/<cap>.h  → 必须有 fw_svc_register(SVC_<CAP>, ...)
  有 fw_svc_register(SVC_<X>) → 必须有 api/af_api/<x>.h（或明确豁免）

v1 明确不含屏幕，所以 `disp.h` 不发布时"没发布"**不算漂移**——
这不是缺口，是设计（docs/display.md §2）。

用法：
    python tools/check_catalog.py
    python tools/check_catalog.py --explain

退出码：0 通过 / 1 有漂移 / 2 环境问题。
"""

from __future__ import annotations

import argparse
import sys

from common import EXIT_CODE, EXIT_ENV, EXIT_OK, Report

from gen_catalog import BASE_HEADERS, capability_headers, registrations

# 有服务但故意没有对应能力头的：列在这里并给出理由，比默默放行诚实。
SERVICE_EXEMPT: dict[str, str] = {
    "SVC_DISP": "屏幕 v1 不发布 disp.h（docs/display.md §2），能力头随屏幕立项一起做",
}


def check(explain: bool) -> Report:
    caps = capability_headers()
    regs = registrations()
    rep = Report("check_catalog", checked=False)
    if not caps and not regs:
        return rep.skip("还没有能力头，也没有服务注册（v1 里屏幕不在内，属正常）")

    rep.checked = True
    cap_svcs = {f"SVC_{c.upper()}" for c in caps}

    for cap in caps:
        svc = f"SVC_{cap.upper()}"
        if svc not in regs:
            rep.error(
                "CAT-UNREG",
                f"有 {cap} 的能力头，但没有任何地方 fw_svc_register({svc}, ...)",
                hint="头文件承诺了能力，却没人实现它——调用方会拿到 CM_ERR_NOT_SUPPORTED 而不是编译错误",
            )
        elif explain:
            print(f"  ok   {cap} ← {svc} ({', '.join(sorted(regs[svc]))})")

    for svc in sorted(set(regs) - cap_svcs):
        if svc in SERVICE_EXEMPT:
            if explain:
                print(f"  skip {svc}：{SERVICE_EXEMPT[svc]}")
            continue
        if svc.startswith("SVC_") and svc[4:].lower() in {h.lower() for h in BASE_HEADERS}:
            continue  # log/res/cap 这类基础服务没有独立的能力头，属正常
        rep.warn(
            "CAT-NOHEAD",
            f"注册了 {svc} 但没有 api/af_api/ 下对应的能力头",
            hint="要么写头，要么在 SERVICE_EXEMPT 里写明为什么不需要",
        )

    return rep


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(prog="check_catalog.py")
    ap.add_argument("--explain", action="store_true")
    args = ap.parse_args(argv)
    try:
        rep = check(args.explain)
    except RuntimeError as exc:
        print(f"check_catalog: 环境问题 —— {exc}", file=sys.stderr)
        return EXIT_ENV
    rep.render()
    return EXIT_OK if rep.ok else EXIT_CODE


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
