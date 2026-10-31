"""policy.py —— 查"这个文件属于谁"，以及 policy.txt 的数据自检。

用法：
    python tools/policy.py --check                 # 数据自检（格式、tag、重复前缀、写⊆读）
    python tools/policy.py role <路径>              # 谁能写 / 谁能读这个路径
    python tools/policy.py role <路径> --json       # 同上，机器可读
    python tools/policy.py dump                    # 打印解析结果

退出码：0 通过 / 1 数据或查询有问题 / 2 环境有问题。
"""

from __future__ import annotations

import argparse
import json
import sys

from common import EXIT_CODE, EXIT_ENV, EXIT_OK, REPO_ROOT, load_policy, PolicyError, rel


def cmd_check(policy) -> int:
    rep = policy.self_check()
    rep.render()
    if not rep.ok:
        return EXIT_CODE
    print(f"\npolicy.txt 数据自检通过：{len(policy.roles)} 条 role、{len(policy.reads)} 条 read。")
    return EXIT_OK


def cmd_role(policy, path: str, as_json: bool) -> int:
    target = rel(REPO_ROOT / path) if not path.startswith("/") else path
    writers = policy.writers_of(target)
    readers = policy.readers_of(target)

    if as_json:
        print(json.dumps(
            {
                "path": target,
                "writers": sorted(writers),
                "readers": sorted(readers),
                "writer_names": policy.role_names(writers),
                "reader_names": policy.role_names(readers),
            },
            ensure_ascii=False,
            indent=2,
        ))
        return EXIT_OK

    print(f"路径：{target}")
    if writers:
        print(f"  能写：{', '.join(policy.role_names(writers))}")
    else:
        print("  能写：（无人可写——只有人能改）")
    if readers:
        print(f"  能读：{', '.join(policy.role_names(readers))}")
    else:
        print("  能读：（无人可读——include 它会失败）")

    if not writers:
        print("\n注意：没有任何 role 前缀覆盖这个路径，所以任何 tag 都写不了。")
        print("      如果是新建的顶层目录，需要人在 policy.txt 里加一行。")
    if writers and not readers:
        print("\n警告：能写却读不到。这个角色改完自己也无法编译验证。")
        return EXIT_CODE
    return EXIT_OK


def cmd_dump(policy) -> int:
    for r in policy.roles:
        print(f"role {r.tag:2d} {r.name:10s} " + " ".join(r.prefixes))
    print()
    for rule in policy.reads:
        print(f"read {rule.prefix:34s} " + " ".join(str(t) for t in sorted(rule.tags)))
    return EXIT_OK


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(prog="policy.py", description="policy.txt 的查询与自检")
    sub = ap.add_subparsers(dest="cmd", required=True)

    p_check = sub.add_parser("check", help="数据自检")
    p_check.set_defaults(func=lambda a: cmd_check(a.policy))

    p_role = sub.add_parser("role", help="查一个路径的读/写角色")
    p_role.add_argument("path", help="仓库内相对路径或绝对路径")
    p_role.add_argument("--json", action="store_true")
    p_role.set_defaults(func=lambda a: cmd_role(a.policy, a.path, a.json))

    p_dump = sub.add_parser("dump", help="打印解析结果")
    p_dump.set_defaults(func=lambda a: cmd_dump(a.policy))

    # 兼容文档里的写法：python tools/policy.py --check
    if argv and argv[0] == "--check":
        argv = ["check", *argv[1:]]

    args = ap.parse_args(argv)
    try:
        args.policy = load_policy()
    except (PolicyError, RuntimeError) as exc:
        print(f"[ERROR] 无法解析 policy.txt：{exc}", file=sys.stderr)
        return EXIT_CODE
    return args.func(args)


if __name__ == "__main__":
    try:
        sys.exit(main(sys.argv[1:]))
    except KeyboardInterrupt:
        sys.exit(EXIT_ENV)
