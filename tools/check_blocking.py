"""check_blocking.py —— 任务体内必须有伪阻塞调用（防死循环）。

依据 docs/architecture.md §3.1.5：伪阻塞只在"调用框架接口"时生效。
一个 `for(;;) { do_work(); }` 里没有阻塞调用，取消位永远没人看，
这个 App 就停不下来。本检查是那道防线的**静态门禁**；
另一道（看门狗）属于管理器，不在本仓库。

判断标准（刻意保守，宁可漏报少一点、误报更少）：
  - 只看 apps/ 下的 .c；
  - 只看"任务体"：注册到 `app_desc_t.task` 的函数，或名字以 `_task` 结尾的函数；
  - 任务体里必须出现至少一个阻塞调用：`fw_*_block(...)` / `*_delay*(...)` / `pf_*sleep*(...)`；
  - 若含无限循环而整个函数一个阻塞调用都没有 → 报错。

用法：
    python tools/check_blocking.py
    python tools/check_blocking.py --list

退出码：0 通过 / 1 有可疑的死循环 / 2 环境问题。
"""

from __future__ import annotations

import argparse
import re
import sys

from common import EXIT_CODE, EXIT_ENV, EXIT_OK, Report, rel, strip_c_comments, walk_files

BLOCKING_CALL_RE = re.compile(
    r"\b(?:fw_\w*_block|fw_delay\w*|app_\w*_block|pf_\w*(?:delay|sleep)\w*|\w+_delay_block)\s*\("
)
INFINITE_LOOP_RE = re.compile(r"\b(?:while\s*\(\s*(?:1|true)\s*\)|for\s*\(\s*;\s*;\s*\))")
TASK_ASSIGN_RE = re.compile(r"\.task\s*=\s*([A-Za-z_]\w*)")
TASK_NAME_RE = re.compile(r"\b(?:static\s+)?(?:void|int|cm_err_t)\s+([A-Za-z_]\w*_task)\s*\(")


def function_bodies(text: str) -> list[tuple[str, int, str]]:
    """粗提取 (函数名, 起始行, 函数体)。不追求完整 C 语法，够用即可。"""
    out: list[tuple[str, int, str]] = []
    for m in re.finditer(r"\b([A-Za-z_]\w*)\s*\([^;{}]*\)\s*\{", text):
        name = m.group(1)
        if name in {"if", "for", "while", "switch", "return", "sizeof"}:
            continue
        i = text.index("{", m.end() - 1)
        depth, j = 0, i
        while j < len(text):
            if text[j] == "{":
                depth += 1
            elif text[j] == "}":
                depth -= 1
                if depth == 0:
                    break
            j += 1
        body = text[i + 1 : j]
        line = text[: m.start()].count("\n") + 1
        out.append((name, line, body))
    return out


def check(listing: bool) -> Report:
    files = walk_files((".c",), ("apps/",))
    rep = Report("check_blocking", checked=False)
    if not files:
        return rep.skip("apps/ 下还没有 .c 文件")

    rep.checked = True
    found_task = False
    for f in files:
        raw = f.read_text(encoding="utf-8", errors="replace")
        text = strip_c_comments(raw)
        path = rel(f)

        registered = set(TASK_ASSIGN_RE.findall(text)) | set(TASK_NAME_RE.findall(text))
        bodies = {name: (line, body) for name, line, body in function_bodies(text)}

        for name in sorted(registered):
            if name not in bodies:
                rep.error(
                    "BLK-NOBODY",
                    f"注册为任务回调的 {name} 在这个文件里找不到函数体",
                    path=path,
                    hint="回调可能在别的文件里；如果确实不存在，这个 App 起不来",
                )
                continue
            found_task = True
            line, body = bodies[name]
            has_block = bool(BLOCKING_CALL_RE.search(body))
            has_loop = bool(INFINITE_LOOP_RE.search(body))
            if listing:
                print(f"  {path}:{line} {name}  阻塞调用={'有' if has_block else '没有'}  无限循环={'有' if has_loop else '没有'}")
            if has_loop and not has_block:
                rep.error(
                    "BLK-LOOP",
                    f"{name} 里有无限循环但一次阻塞调用都没有",
                    path=path,
                    line=line,
                    hint="取消位只在这些调用里被检查。补一个 fw_*_block(...)（如 fw_delay_block(100)），"
                         "或者这个任务本来就不该常驻",
                )
        if registered and not bodies:
            rep.warn("BLK-EMPTY", f"{path} 里有任务回调的引用，但没解析出函数体")
    if not found_task:
        return Report("check_blocking", checked=False).skip(
            "没有任何注册到 app_desc_t.task 或名为 *_task 的函数"
        )
    return rep


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(prog="check_blocking.py")
    ap.add_argument("--list", action="store_true", help="列出每个任务体与它的判定依据")
    args = ap.parse_args(argv)
    try:
        rep = check(args.list)
    except RuntimeError as exc:
        print(f"check_blocking: 环境问题 —— {exc}", file=sys.stderr)
        return EXIT_ENV
    rep.render()
    return EXIT_OK if rep.ok else EXIT_CODE


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
