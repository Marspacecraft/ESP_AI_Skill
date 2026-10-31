"""check_policy.py —— policy.txt 的三条判定 + 两条附加检查。

三条判定（docs/access-control.md §4）：
  ① 读权：真实 include 到的仓库内头文件，必须落在该 tag 的 read 集合里
  ② 符号：nm -u 的每个未定义符号，两问——
        Q1 它的定义在调用者**可写区**里吗？是 → 自己人的代码，放行
        Q2 否则，它必须在**调用者不可写**的头里被声明，且那个头被实际 include 过
  ③ 写入：改动涉及的每个路径，必须被该身份的 role 前缀覆盖

两条附加检查：
  A 注册清单只许追加（apps/app_manifest.c：diff 只增，新增行只引用自己目录下的对象）
  B 每个已存在的顶层目录，必须被某个 role 前缀覆盖

用法：
    python tools/check_policy.py --all --as 2
    python tools/check_policy.py --writes --as 2
    python tools/check_policy.py --reads  --as 3 --paths framework/src/fw_app.c
    python tools/check_policy.py --all --as human
    python tools/check_policy.py --all            # 未声明身份：打印 SKIP 并返回 0（CI 里返回 1）

退出码：0 通过 / 1 有违规 / 2 环境问题（缺 git / 编译器 / nm）。
"""

from __future__ import annotations

import argparse
import os
import re
import sys

from common import (
    EXIT_CODE,
    EXIT_ENV,
    EXIT_OK,
    REPO_ROOT,
    Report,
    env_or_none,
    load_policy,
    need,
    rel,
    run,
    strip_c_comments,
    top_level_dirs,
)

CTAG_HUMAN = "human"

# 头文件搜索后缀
C_SUFFIX = (".c",)
H_SUFFIX = (".h", ".hpp", ".hh")


# ---------------------------------------------------------------- git

def has_commits() -> bool:
    """仓库里有没有 HEAD。没有提交时 git status 会把整棵树都算成"改动"。"""
    git = need("git", "需要一个 git 仓库")
    cp = run([git, "rev-parse", "--verify", "--quiet", "HEAD"])
    return cp.returncode == 0


def changed_entries() -> list[tuple[str, str]]:
    """git status 里所有非干净的条目：(状态, 路径)。未跟踪文件也算。"""
    git = need("git", "需要一个 git 仓库")
    cp = run([git, "status", "--porcelain=v1", "-uall"])
    if cp.returncode != 0:
        raise RuntimeError(f"git status 失败：{cp.stderr.strip()}")
    out: list[tuple[str, str]] = []
    for raw in cp.stdout.splitlines():
        if not raw.strip():
            continue
        status = raw[:2].strip() or "?"
        path = raw[3:].strip()
        if " -> " in path:  # rename：只取新名
            path = path.split(" -> ", 1)[1]
        out.append((status, path.replace("\\", "/")))
    return out


# ---------------------------------------------------------------- ③ 写入

def check_writes(policy, ident: str, paths: list[str]) -> Report:
    rep = Report("check_policy: 写入", checked=True)
    if ident == CTAG_HUMAN:
        rep.warn("W-HUMAN", "身份是 human：人不受 role 行约束，写入判定跳过")
        return rep
    tag = int(ident)
    if tag not in policy.tags:
        rep.error("W-BADTAG", f"--as {identifier_str(ident)} 不是一个已定义的 tag（{sorted(policy.tags)}）")
        return rep
    for p in paths:
        writers = policy.writers_of(p)
        if tag in writers:
            continue
        if not writers:
            rep.error(
                "W-NOBODY",
                f"{p}：没有任何 role 前缀覆盖它，所以谁都写不了",
                hint="新建顶层目录要人在 policy.txt 加一行；写在这里说明这次改动越界了",
            )
        else:
            rep.error(
                "W-OTHER",
                f"{p}：能写它的是 {', '.join(policy.role_names(writers))}，不是 tag {tag}",
                hint="改别人的目录属于越层。要么报告，要么请对方改",
            )
    return rep


def identifier_str(ident: str) -> str:
    return f"{ident}（{CTAG_HUMAN}）" if ident == CTAG_HUMAN else f"tag {ident}"


# ---------------------------------------------------------------- ① 读权

def include_dirs_for(policy, tag: int) -> list[str]:
    """给 clang 的 -I 列表：所有 tag 能读的目录 + 仓库内所有头所在目录。"""
    dirs: list[str] = []
    seen: set[str] = set()

    def add(d: str) -> None:
        if d and d not in seen and (REPO_ROOT / d).is_dir():
            seen.add(d)
            dirs.append(str(REPO_ROOT / d))

    for rule in policy.reads:
        if tag in rule.tags:
            add(rule.prefix)
    # 再补上所有含头文件的子目录（跨目录 include 时 clang 需要精确目录）
    for top in {p.split("/")[0] for p in seen}:
        base = REPO_ROOT / top
        if base.is_dir():
            for sub in base.rglob("*"):
                if sub.is_dir():
                    add(rel(sub))
    return dirs


def clang_deps(clang: str, src: Path, incdirs: list[str]) -> list[str]:
    # -M：真实展开（含系统头）；判定时只保留仓库内的头，所以不需要 -MM 的"只列用户头"
    cmd = [clang, "-M", *[f"-I{d}" for d in incdirs], str(src)]
    cp = run(cmd)
    if cp.returncode != 0:
        raise RuntimeError(f"clang -M 失败（{rel(src)}）：\n{cp.stderr.strip()[:800]}")
    deps: list[str] = []
    for line in cp.stdout.replace("\\\n", " ").split():
        if line.endswith(":") or line.endswith(".c"):
            continue
        deps.append(line)
    return deps


def check_reads(policy, ident: str, paths: list[str]) -> Report:
    rep = Report("check_policy: 读权", checked=False)
    if ident == CTAG_HUMAN:
        return rep.skip("身份是 human：读权判定只对 tag 有意义")
    tag = int(ident)
    sources = [p for p in paths if p.endswith(C_SUFFIX)]
    if not sources:
        return rep.skip("改动里没有 .c 文件，没有可分析的编译单元")
    try:
        clang = need("clang", "读权判定要真实展开 #include")
    except Exception as exc:  # noqa: BLE001
        raise RuntimeError(str(exc)) from exc

    rep.checked = True
    incdirs = include_dirs_for(policy, tag)
    for src in sources:
        full = REPO_ROOT / src
        if not full.is_file():
            rep.warn("R-MISSING", f"{src} 在改动列表里但文件不存在（删除了？）")
            continue
        deps = clang_deps(clang, full, incdirs)
        for dep in deps:
            try:
                dpath = rel(Path(dep).resolve())
            except OSError:
                continue
            if dpath.startswith("..") or not (REPO_ROOT / dpath).is_file():
                continue  # 仓库外/系统头不判
            if tag in policy.readers_of(dpath):
                continue
            if not policy.readers_of(dpath):
                rep.error(
                    "R-NOBODY",
                    f"{src} include 了 {dpath}，而它没有任何 read 行",
                    hint="这个头谁都不能读，include 它会在判定里报错",
                )
            else:
                rep.error(
                    "R-DENIED",
                    f"{src} include 了 {dpath}，但 tag {tag} 读不到它"
                    f"（能读的是 {', '.join(policy.role_names(policy.readers_of(dpath)))}）",
                    hint="跨层只能经 api/。看 docs/code-structure.md §2 的 include 矩阵",
                )
    return rep


# ---------------------------------------------------------------- ② 符号

DECL_RE_CACHE: dict[str, dict[str, dict[str, set[str]]]] = {}


def headers_index() -> dict[str, set[str]]:
    """{符号: {声明它的头路径, ...}}。粗但够用：头里出现的标识符都算。"""
    if "idx" in DECL_RE_CACHE:
        return DECL_RE_CACHE["idx"]  # type: ignore[return-value]
    idx: dict[str, set[str]] = {}
    for p in REPO_ROOT.rglob("*"):
        if not p.is_file() or p.suffix not in H_SUFFIX:
            continue
        if ".git" in p.parts:
            continue
        try:
            text = strip_c_comments(p.read_text(encoding="utf-8", errors="replace"))
        except OSError:
            continue
        path = rel(p)
        for m in re.finditer(r"\b([A-Za-z_]\w*)\b", text):
            idx.setdefault(m.group(1), set()).add(path)
    DECL_RE_CACHE["idx"] = idx  # type: ignore[index]
    return idx


def defined_symbols(policy, tag: int) -> dict[str, set[str]]:
    """{符号: {定义它的 .c 路径}}，只看 tag 可写区里的 .c。"""
    key = f"def{tag}"
    if key in DECL_RE_CACHE:
        return DECL_RE_CACHE[key]  # type: ignore[return-value]
    nm = need_nm()
    out: dict[str, set[str]] = {}
    for src in writable_sources(policy, tag):
        obj = compile_object(src)
        if obj is None:
            continue
        cp = run([nm, "--defined-only", str(obj)])
        for line in cp.stdout.splitlines():
            parts = line.split()
            if not parts:
                continue
            sym = parts[-1]
            out.setdefault(sym, set()).add(rel(src))
    DECL_RE_CACHE[key] = out  # type: ignore[index]
    return out


def writable_sources(policy, tag: int) -> list[Path]:
    out: list[Path] = []
    for p in REPO_ROOT.rglob("*"):
        if not p.is_file() or p.suffix not in C_SUFFIX:
            continue
        if ".git" in p.parts:
            continue
        if tag in policy.writers_of(rel(p)):
            out.append(p)
    return sorted(out)


def compile_object(src: Path) -> Path | None:
    cc = env_or_none("CC") or "cc"
    exe = None
    for cand in (cc, "clang", "gcc"):
        try:
            exe = need(cand)
            break
        except Exception:  # noqa: BLE001
            continue
    if not exe:
        return None
    outdir = REPO_ROOT / "build" / "tools"
    outdir.mkdir(parents=True, exist_ok=True)
    obj = outdir / (src.stem + f"_{abs(hash(str(src))) % 100000}" + ".o")
    incs = [f"-I{d}" for d in sorted({str(p.parent) for p in REPO_ROOT.rglob("*.h") if ".git" not in p.parts})]
    cp = run([exe, "-c", "-fsyntax-only", *incs, str(src)])
    if cp.returncode != 0:
        return None
    cp = run([exe, "-c", *incs, "-o", str(obj), str(src)])
    return obj if cp.returncode == 0 else None


def need_nm() -> str:
    for cand in ("llvm-nm", "nm"):
        try:
            return need(cand, "符号判定要读目标文件的未定义符号")
        except Exception:  # noqa: BLE001
            continue
    raise RuntimeError("找不到 llvm-nm / nm")


def check_symbols(policy, ident: str, paths: list[str]) -> Report:
    rep = Report("check_policy: 符号", checked=False)
    if ident == CTAG_HUMAN:
        return rep.skip("身份是 human：符号判定只对 tag 有意义")
    tag = int(ident)
    sources = [p for p in paths if p.endswith(C_SUFFIX)]
    if not sources:
        return rep.skip("改动里没有 .c 文件，没有符号要判")
    nm = need_nm()
    rep.checked = True
    idx = headers_index()
    defs = defined_symbols(policy, tag)
    incdirs = include_dirs_for(policy, tag)

    for src in sources:
        full = REPO_ROOT / src
        if not full.is_file():
            continue
        obj = compile_object(full)
        if obj is None:
            rep.error("S-COMPILE", f"{src} 编译不过，符号判定做不了", hint="先把它编过再跑门禁")
            continue
        # 这个 TU 实际 include 了哪些头
        try:
            inc = {rel(Path(d).resolve()) for d in clang_deps(need("clang"), full, incdirs)}
        except RuntimeError:
            inc = set()
        cp = run([nm, "-u", str(obj)])
        for line in cp.stdout.splitlines():
            parts = line.split()
            if not parts:
                continue
            sym = parts[-1]
            # Q1：定义在可写区 → 自己人的代码
            if sym in defs:
                continue
            # Q2：声明必须落在不可写的头里，且被实际 include
            decls = idx.get(sym, set())
            if not decls:
                rep.error(
                    "S-UNDECLARED",
                    f"{src} 用了未定义符号 {sym}，仓库里没有任何头声明它",
                    hint="要么 include 那个头，要么这是漏编译/拼错的名字",
                )
                continue
            readable = {h for h in decls if tag in policy.readers_of(h)}
            included = {h for h in readable if h in inc}
            if not included:
                rep.error(
                    "S-NOTINCLUDED",
                    f"{src} 用了 {sym}，它只声明在 {', '.join(sorted(readable) or sorted(decls))}，"
                    f"而该头没有被实际 include",
                    hint="不要靠 extern 取特权：include 声明它的头（这就是读权判定那条线）",
                )
                continue
            writable = {h for h in included if tag in policy.writers_of(h)}
            if writable:
                rep.error(
                    "S-SELFDECL",
                    f"{src} 用的 {sym} 只声明在你自己可写的头里：{', '.join(sorted(writable))}",
                    hint="在可写头里造一份声明就能绕过符号判定——所以 Q1 只认'定义也在可写区'",
                )
    return rep


# ---------------------------------------------------------------- 附加检查

MANIFEST = "apps/app_manifest.c"
OBJ_RE = re.compile(r"&\s*([A-Za-z_]\w*)")


def check_manifest_append_only() -> Report:
    rep = Report("check_policy: 清单只追加", checked=False)
    if not (REPO_ROOT / MANIFEST).is_file():
        return rep.skip(f"{MANIFEST} 还不存在")
    git = need("git")
    entries = [p for _, p in changed_entries()]
    if MANIFEST not in entries:
        return rep.skip(f"{MANIFEST} 这次没有改动")
    rep.checked = True
    cp = run([git, "diff", "-U0", "--", MANIFEST])
    if cp.returncode != 0:
        cp = run([git, "diff", "--no-index", "-U0", "/dev/null", MANIFEST])
    added: list[str] = []
    for line in cp.stdout.splitlines():
        if line.startswith("+++") or line.startswith("---") or line.startswith("@@"):
            continue
        if line.startswith("-"):
            rep.error(
                "M-DELETED",
                f"{MANIFEST} 有删除行：{line[1:].strip()[:70]}",
                hint="清单是 tag 1 与 tag 2 共写的，删除等于改坏别人的条目",
            )
        elif line.startswith("+"):
            added.append(line[1:])
    demo = REPO_ROOT / "apps" / "demo"
    for line in added:
        for sym in OBJ_RE.findall(line):
            if not demo.is_dir():
                break
            if not any(sym in f.read_text(encoding="utf-8", errors="replace")
                       for f in demo.rglob("*.c")):
                rep.error(
                    "M-FOREIGN",
                    f"{MANIFEST} 新增行引用了 {sym}，它在 apps/demo/ 里找不到定义",
                    hint="只许追加自己目录下的 App",
                )
    return rep


def check_toplevel_coverage(policy) -> Report:
    rep = Report("check_policy: 顶层目录", checked=True)
    for d, nfiles in top_level_dirs():
        if policy.writers_of(d):
            continue
        # 有 read 行但没有 role 行 = 只读给人看、只有人能写（`skills/` 就是这种：考卷）。
        if policy.readers_of(d) and nfiles > 0:
            continue
        if nfiles == 0:
            rep.warn("T-ORPHAN-EMPTY", f"顶层空目录 {d} 没有任何 role 前缀覆盖（多半是建错的残骸）")
        else:
            rep.error(
                "T-ORPHAN",
                f"顶层目录 {d}（{nfiles} 个文件）没有任何 role 前缀覆盖，也没有 read 行",
                hint="人在 policy.txt 加一行 role（写者）或 read（只读、人维护）；或者这个目录不该存在（不是代码就移出仓库）",
            )
    return rep


# ---------------------------------------------------------------- 入口

def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(prog="check_policy.py", description="policy.txt 的三条判定")
    ap.add_argument("--all", action="store_true", help="跑三条判定 + 两条附加检查")
    ap.add_argument("--reads", action="store_true", help="只跑读权判定")
    ap.add_argument("--symbols", action="store_true", help="只跑符号判定")
    ap.add_argument("--writes", action="store_true", help="只跑写入判定")
    ap.add_argument("--as", dest="ident", metavar="<tag|human>", help="以谁的身份判定")
    ap.add_argument("--paths", nargs="*", default=None, help="只看这些路径（默认取 git status）")
    ap.add_argument("--strict", action="store_true", help="未声明身份视为失败（CI 必须加）")
    args = ap.parse_args(argv)

    policy = load_policy()
    if not (args.all or args.reads or args.symbols or args.writes):
        args.all = True

    ident = args.ident
    if not ident:
        ci = os.environ.get("CI", "").strip().lower() in {"1", "true", "yes", "on"}
        if ci or args.strict:
            print("check_policy: FAIL  未声明 --as，CI/--strict 下这不是通过")
            return EXIT_CODE
        print("check_policy: SKIP  未声明 --as —— 这不是通过（本地允许，CI 会判红）")
        return EXIT_OK

    if ident != CTAG_HUMAN:
        try:
            int(ident)
        except ValueError:
            print(f"check_policy: FAIL  --as {ident} 既不是 tag 也不是 human", file=sys.stderr)
            return EXIT_CODE

    if args.paths:
        paths = [p.replace("\\", "/") for p in args.paths]
    elif not has_commits():
        # 仓库一个提交都还没有时，git status 把整棵树都算成"改动"，
        # 按它判写入只会得到一堆噪音。这种情况明确说"判不了"，不要假装跑过。
        print("check_policy: SKIP  仓库还没有任何提交，无法确定'这次改了什么'")
        print("                      先用 --paths <文件...> 指定要判的文件；这不是通过")
        return EXIT_OK
    else:
        paths = [p for _, p in changed_entries()]

    reports: list[Report] = []
    if args.writes or args.all:
        reports.append(check_writes(policy, ident, paths))
    if args.reads or args.all:
        reports.append(check_reads(policy, ident, paths))
    if args.symbols or args.all:
        reports.append(check_symbols(policy, ident, paths))
    if args.all:
        reports.append(check_manifest_append_only())
        reports.append(check_toplevel_coverage(policy))

    failed = False
    for rep in reports:
        rep.render()
        failed = failed or not rep.ok

    print()
    if failed:
        print("check_policy: FAIL")
        return EXIT_CODE
    print(f"check_policy: PASS  （身份 {identifier_str(ident)}，{len(paths)} 个路径）")
    return EXIT_OK


if __name__ == "__main__":
    try:
        sys.exit(main(sys.argv[1:]))
    except RuntimeError as exc:
        print(f"check_policy: 环境问题 —— {exc}", file=sys.stderr)
        sys.exit(EXIT_ENV)
