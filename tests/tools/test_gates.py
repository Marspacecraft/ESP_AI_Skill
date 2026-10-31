"""门禁脚本自测 —— 门禁自己也是代码，也要有测试。

跑：python tests/test.py tools          （整层，只统计）
    python tests/test.py tools test     （逐用例）

这里断言的是**命令行的可观察行为**，不是内部实现——门禁脚本允许重构，
但"哪些路径归谁、退出码是什么"不能悄悄变。改坏一条，这里就红。
"""

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from tinytest import case, contains, eq, run, sh  # noqa: E402


def role_of(path):
    return sh("tools/policy.py", "role", path).stdout


# ---------------------------------------------------------------- 写权归属
@case
def app_source_owned_by_tag2():
    contains("能写：2:aiapp", role_of("apps/demo/x/app.c"))


@case
def system_app_owned_by_tag1():
    contains("能写：1:sysapp", role_of("apps/system/main.c"))


@case
def platform_impl_owned_by_tag4():
    contains("能写：4:platform", role_of("platform/host-fake/pf_ops_impl.c"))


@case
def api_header_owned_by_tag5():
    contains("能写：5:api", role_of("api/af_api/app.h"))


@case
def docs_owned_by_tag5():
    contains("能写：5:api", role_of("docs/roles.md"))


@case
def app_manifest_shared_by_tag1_and_tag2():
    out = role_of("apps/app_manifest.c")
    contains("1:sysapp", out)
    contains("2:aiapp", out)


# ---------------------------------------------------------------- 谁都不能写
@case
def build_files_have_no_writer():
    contains("无人可写", role_of("CMakeLists.txt"))


@case
def skills_are_readonly_but_readable():
    """v1.5 起 skills/ 从 role 5 的写区摘掉：能读、不能写。"""
    out = role_of("skills/role-app/SKILL.md")
    contains("无人可写", out)
    contains("5:api", out)


@case
def policy_itself_has_no_writer():
    contains("无人可写", role_of("policy.txt"))


# ---------------------------------------------------------------- 门禁退出码
@case
def policy_self_check_passes():
    proc = sh("tools/policy.py", "--check")
    eq(0, proc.returncode, "policy.py --check 退出码")
    contains("PASS", proc.stdout.upper())


@case
def reads_gate_passes_for_tag2():
    proc = sh("tools/check_policy.py", "--reads", "--as", "2")
    eq(0, proc.returncode, "check_policy.py --reads --as 2 退出码")


@case
def writes_gate_skips_when_repo_has_no_commit():
    """没有 git 提交时写入判定必须明说 SKIP，而不是假装通过。"""
    proc = sh("tools/check_policy.py", "--writes", "--as", "2")
    eq(0, proc.returncode, "check_policy.py --writes 无提交时退出码")
    contains("SKIP", proc.stdout)


if __name__ == "__main__":
    raise SystemExit(run("test_gates"))
