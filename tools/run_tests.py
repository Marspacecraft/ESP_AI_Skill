"""run_tests.py —— 已迁走，这里只是转发。

测试的唯一入口是 **`tests/test.py`**（`docs/test-plan.md`）：
测试脚本和用例都放在 `tests/` 下，按源码分层镜像（`tests/<层>/<子路径>`），
`tools/` 只说"门禁查什么"，不再管"测试怎么跑"。

保留这个文件只为两件事：
  1. 老命令 `python tools/run_tests.py` 还能用（免得有人照着旧文档敲）；
  2. `validate.py` 之外如果有人直接调它，行为与 `tests/test.py` 完全一致。

用法与 `tests/test.py` 相同：

    python tools/run_tests.py                  # 全部层，只统计
    python tools/run_tests.py tools            # 整层只统计
    python tools/run_tests.py tools gates      # 收窄 → 逐用例
    python tools/run_tests.py test_gates       # 跨层按程序名过滤

退出码：0 通过 / 1 有失败 / 2 环境问题。
"""

from __future__ import annotations

import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
ENTRY = REPO / "tests" / "test.py"


def main(argv: list[str]) -> int:
    if not ENTRY.exists():
        print(f"环境问题: 找不到 {ENTRY}（测试入口被删了？见 docs/test-plan.md）")
        return 2

    # 旧参数里唯一没有等价物的：平台一致性套件换板。
    # 现在它由 `tests/test.py port` 跑，板由套件自己在编译期决定。
    if "--src" in argv:
        i = argv.index("--src")
        board = argv[i + 1] if i + 1 < len(argv) else "<板>"
        print("--src 不再传参：一致性套件只有一份（tests/port/pf_conformance.c），")
        print(f"板级差异靠把 {board} 的 pf_ops_t 编进来，不是靠命令行换目录。")
        print("改成：python tests/test.py port")
        return 2

    # 直接继承 stdio 的 call（不用管道——这个环境禁止给子进程建管道）
    return subprocess.call([sys.executable, str(ENTRY), *argv])


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
