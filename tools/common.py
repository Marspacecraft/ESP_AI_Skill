"""tools/ 公共库：仓库根定位、policy.txt 解析、结果模型、外部工具探测。

门禁只有一份实现（Python），本文件是它们共用的最小底座。
约定（docs/code-structure.md §10）：退出码 0 通过 / 1 代码问题 / 2 环境问题。
"""

from __future__ import annotations

import os
import re
import shutil
import subprocess
import sys
import tempfile
from dataclasses import dataclass, field
from pathlib import Path

# ---------------------------------------------------------------- 退出码

EXIT_OK = 0
EXIT_CODE = 1
EXIT_ENV = 2

# ---------------------------------------------------------------- 仓库根

_THIS = Path(__file__).resolve()


def find_repo_root(start: Path | None = None) -> Path:
    """从 tools/ 往上找带 policy.txt 的目录。"""
    cur = (start or _THIS).resolve()
    for cand in [cur, *cur.parents]:
        if (cand / "policy.txt").is_file():
            return cand
    raise RuntimeError("找不到仓库根（往上没有 policy.txt）")


REPO_ROOT = find_repo_root()


def rel(p: Path | str) -> str:
    """仓库内路径 → posix 相对路径。"""
    pp = Path(p)
    if not pp.is_absolute():
        return pp.as_posix()
    try:
        return pp.resolve().relative_to(REPO_ROOT).as_posix()
    except ValueError:
        return pp.as_posix()


# ---------------------------------------------------------------- 结果模型

LEVEL_ERROR = "error"
LEVEL_WARN = "warn"


@dataclass
class Finding:
    level: str
    code: str
    message: str
    path: str = ""
    line: int = 0
    hint: str = ""

    def format(self) -> str:
        loc = self.path
        if loc and self.line:
            loc = f"{loc}:{self.line}"
        head = f"[{self.level.upper()}] {self.code}"
        if loc:
            head = f"{head} {loc}"
        out = f"{head}  {self.message}"
        if self.hint:
            out += f"\n        提示：{self.hint}"
        return out


@dataclass
class Report:
    """一个检查器一次运行的结论。"""

    name: str
    findings: list[Finding] = field(default_factory=list)
    checked: bool = False
    reason: str = ""  # checked=False 时说明为什么没跑

    def error(self, code: str, message: str, **kw) -> None:
        self.findings.append(Finding(LEVEL_ERROR, code, message, **kw))

    def warn(self, code: str, message: str, **kw) -> None:
        self.findings.append(Finding(LEVEL_WARN, code, message, **kw))

    @property
    def errors(self) -> list[Finding]:
        return [f for f in self.findings if f.level == LEVEL_ERROR]

    @property
    def ok(self) -> bool:
        return not self.errors

    def skip(self, reason: str) -> "Report":
        self.checked = False
        self.reason = reason
        return self

    def render(self) -> None:
        if not self.checked:
            print(f"{self.name:22s} SKIP  {self.reason}")
            return
        errs = self.errors
        status = "PASS" if not errs else f"FAIL  ({len(errs)} 项)"
        print(f"{self.name:22s} {status}")
        for f in self.findings:
            print("  " + f.format())


# ---------------------------------------------------------------- policy.txt

@dataclass(frozen=True)
class Role:
    tag: int
    name: str
    prefixes: tuple[str, ...]


@dataclass(frozen=True)
class ReadRule:
    prefix: str
    tags: frozenset[int]


class PolicyError(Exception):
    pass


class Policy:
    """policy.txt 的解析结果。

    role 行：  role <tag> <名字> <可写路径前缀>...
    read 行：  read <路径前缀> <允许读它的 tag>...

    语义（policy.txt 文首）：
      - 一个文件的写角色集合 = 匹配它的**最长前缀**；命中同样长度的多条前缀时全部生效（并集）。
      - 同样规则适用于读权。
      - 未列出的路径 = 谁都不能读、不能写。
    """

    def __init__(self, text: str, origin: str = "policy.txt") -> None:
        self.origin = origin
        self.roles: list[Role] = []
        self.reads: list[ReadRule] = []
        self._parse(text)

    # -- 解析 ----------------------------------------------------------

    def _parse(self, text: str) -> None:
        for lineno, raw in enumerate(text.splitlines(), start=1):
            line = raw.strip()
            if not line or line.startswith("#"):
                continue
            parts = line.split()
            kind = parts[0]
            if kind == "role":
                if len(parts) < 3:
                    raise PolicyError(f"{self.origin}:{lineno} role 行至少要有 tag 和名字")
                try:
                    tag = int(parts[1])
                except ValueError:
                    raise PolicyError(f"{self.origin}:{lineno} tag 不是整数：{parts[1]!r}")
                prefixes = tuple(_norm_prefix(p) for p in parts[3:])
                self.roles.append(Role(tag, parts[2], prefixes))
            elif kind == "read":
                if len(parts) < 3:
                    raise PolicyError(f"{self.origin}:{lineno} read 行至少要有前缀和一个 tag")
                tags = set()
                for tok in parts[2:]:
                    try:
                        tags.add(int(tok))
                    except ValueError:
                        raise PolicyError(f"{self.origin}:{lineno} read 的 tag 不是整数：{tok!r}")
                self.reads.append(ReadRule(_norm_prefix(parts[1]), frozenset(tags)))
            else:
                raise PolicyError(f"{self.origin}:{lineno} 不认识的指令 {kind!r}（只允许 role / read）")

    # -- 查询 ----------------------------------------------------------

    @property
    def tags(self) -> set[int]:
        return {r.tag for r in self.roles}

    def _best(self, path: str, table: list[tuple[str, frozenset[int]]]) -> frozenset[int]:
        """最长前缀优先；同长度取并集。"""
        target = _norm_path(path)
        best_len = -1
        hits: set[int] = set()
        for prefix, tags in table:
            if not _prefix_matches(prefix, target):
                continue
            n = len(prefix)
            if n > best_len:
                best_len = n
                hits = set(tags)
            elif n == best_len:
                hits |= set(tags)
        return frozenset(hits)

    def writers_of(self, path: str) -> frozenset[int]:
        table = [(p, frozenset({r.tag})) for r in self.roles for p in r.prefixes]
        return self._best(path, table)

    def readers_of(self, path: str) -> frozenset[int]:
        table = [(r.prefix, r.tags) for r in self.reads]
        return self._best(path, table)

    def role_names(self, tags) -> list[str]:
        by_tag = {r.tag: r.name for r in self.roles}
        return [f"{t}:{by_tag.get(t, '?')}" for t in sorted(tags)]

    # -- 自检 ----------------------------------------------------------

    def self_check(self) -> Report:
        rep = Report("policy.py --check", checked=True)

        # 1. tag 编号：必须是 1..5（新增层要改文档与三份 skill，所以这里写死并报错提醒）
        expected = {1, 2, 3, 4, 5}
        actual = self.tags
        if actual != expected:
            rep.error(
                "P-TAGSET",
                f"tag 集合是 {sorted(actual)}，文档约定是 {sorted(expected)}",
                hint="新增/删除一个架构层要同时改 docs/roles.md、docs/access-control.md 与三份 skill",
            )

        # 2. 同一**行**里重复列同一个前缀是手抖；但**跨行共写是合法的**
        #    （`apps/app_manifest.c` 由 tag 1 与 tag 2 共写——命中同样长度的
        #    最长前缀时并集放行，见 docs/roles.md §1）。
        for r in self.roles:
            if not r.prefixes:
                rep.warn("P-NOPREFIX", f"role {r.tag} {r.name} 没有任何可写前缀")
            seen: set[str] = set()
            for p in r.prefixes:
                if p in seen:
                    rep.error(
                        "P-DUPPREFIX",
                        f"role {r.tag} 同一行里重复列了 {p!r}",
                        hint="删掉重复的那个。跨行共写是合法的，这里只抓行内手抖",
                    )
                seen.add(p)
        read_prefixes = [r.prefix for r in self.reads]
        dup = {p for p in read_prefixes if read_prefixes.count(p) > 1}
        for p in sorted(dup):
            rep.error("P-DUPREAD", f"read 前缀 {p!r} 出现了不止一次")

        # 3. 每个给出的 tag 必须存在
        known = self.tags
        for r in self.reads:
            for t in r.tags:
                if t not in known:
                    rep.error("P-UNKNOWNTAG", f"read {r.prefix} 里引用了不存在的 tag {t}")

        # 4. 写 ⊆ 读：能改一个文件，通常也要能 include 它才能验证。
        #    但有些目录是**故意**不给读权（`tools/`：AI 只能按退出码用它，
        #    读不到门禁源码是设计），所以这里只报警告。
        for r in self.roles:
            for p in r.prefixes:
                if not self.readers_of(p) >= {r.tag}:
                    rep.warn(
                        "P-WRITENOTREAD",
                        f"tag {r.tag} 能写 {p} 但读不到它",
                        hint="如果是故意的（如 tools/：按退出码用、不给读源码），忽略即可；"
                             "否则这个角色改完自己也无法编译验证",
                    )

        # 5. 每个 read 前缀都该有写者，否则是一条永久读不到内容的规则（仅警告）
        for rule in self.reads:
            if not self.writers_of(rule.prefix):
                rep.warn(
                    "P-READNOWRITER",
                    f"read {rule.prefix} 有读者但没有任何 tag 能写",
                    hint="要么是预留位（写成注释更诚实），要么是漏了 role 前缀",
                )

        # 6. 路径形式
        for r in self.roles:
            for p in r.prefixes:
                _check_prefix_shape(rep, p, f"role {r.tag}")
        for rule in self.reads:
            _check_prefix_shape(rep, rule.prefix, "read 行")
        return rep


def _norm_path(path: str) -> str:
    p = path.replace("\\", "/")
    while p.startswith("./"):
        p = p[2:]
    return p


def _norm_prefix(prefix: str) -> str:
    return _norm_path(prefix)


def _prefix_matches(prefix: str, path: str) -> bool:
    if prefix.endswith("/"):
        return path == prefix[:-1] or path.startswith(prefix)
    return path == prefix


_PATH_RE = re.compile(r"^[A-Za-z0-9_.\-/]+$")


def _check_prefix_shape(rep: Report, prefix: str, where: str) -> None:
    if not _PATH_RE.match(prefix):
        rep.error("P-BADPATH", f"{where} 的路径含有非法字符：{prefix!r}", hint="路径只允许字母数字._-/，不含空格")
    if prefix.startswith("/") or ".." in prefix.split("/"):
        rep.error("P-BADPATH", f"{where} 的路径必须相对仓库根、且不含 '..'：{prefix!r}")
    if prefix.endswith("/") and prefix.endswith("//"):
        rep.error("P-BADPATH", f"{where} 的路径出现重复斜杠：{prefix!r}")


def load_policy(path: Path | None = None) -> Policy:
    p = path or (REPO_ROOT / "policy.txt")
    if not p.is_file():
        raise RuntimeError(f"找不到 {rel(p)}")
    return Policy(p.read_text(encoding="utf-8"), rel(p))


# ---------------------------------------------------------------- 外部工具

class EnvProblem(Exception):
    """环境缺失（编译器、git 等）——退出码 2，不是失败。"""


def need(tool: str, hint: str = "") -> str:
    exe = shutil.which(tool)
    if not exe:
        raise EnvProblem(f"找不到 {tool}" + (f"（{hint}）" if hint else ""))
    return exe


def env_or_none(name: str) -> str | None:
    val = os.environ.get(name, "").strip()
    return val or None


def run(cmd: list[str], cwd: Path | None = None, check: bool = False) -> subprocess.CompletedProcess:
    """跑一个外部命令并把输出收回来。

    **不用管道**：某些沙箱（Windows 上的 CreatePipe）会直接拒绝 `capture_output`，
    子进程连启动都失败。改成「写临时文件再读回」——慢一点，但哪都能跑，
    而且输出长度不受缓冲区限制。这是 docs/code-structure.md 决定 10 的同一条理由。
    """
    with tempfile.TemporaryFile("w+", encoding="utf-8", errors="replace") as fo, \
            tempfile.TemporaryFile("w+", encoding="utf-8", errors="replace") as fe:
        proc = subprocess.run(
            cmd,
            cwd=str(cwd or REPO_ROOT),
            stdout=fo,
            stderr=fe,
            stdin=subprocess.DEVNULL,
            text=True,
            encoding="utf-8",
            errors="replace",
            check=check,
        )
        fo.seek(0)
        fe.seek(0)
        out, err = fo.read(), fe.read()
    return subprocess.CompletedProcess(cmd, proc.returncode, out, err)


# ---------------------------------------------------------------- 文件枚举

# 仓库根下不参与"必须有人能写"判定的目录：版本库、构建产物、解释器缓存，
# 以及编辑器/笔记工具自己建的隐藏目录（`.obsidian/`、`.vscode/`）。
IGNORED_TOP = {".git", "__pycache__", "build", "node_modules", ".obsidian", ".vscode"}


def top_level_dirs() -> list[tuple[str, int]]:
    """仓库根下的顶层目录 → (名字带斜杠, 里面有多少个文件)。

    文件数用来区分两种情况：空目录只是没人清理的残骸（警告），
    有文件的目录才是"这块地方没有归属"（错误）。
    """
    out: list[tuple[str, int]] = []
    for p in sorted(REPO_ROOT.iterdir()):
        if not p.is_dir():
            continue
        if p.name in IGNORED_TOP or p.name.startswith("."):
            continue
        n = sum(1 for q in p.rglob("*") if q.is_file() and ".git" not in q.parts)
        out.append((p.name + "/", n))
    return out


def walk_files(suffixes: tuple[str, ...], roots: tuple[str, ...] = ()) -> list[Path]:
    """按 policy 的目录前缀枚举源文件；root 不存在就跳过。"""
    out: list[Path] = []
    for root in roots:
        base = REPO_ROOT / root
        if not base.is_dir():
            continue
        for p in sorted(base.rglob("*")):
            if p.is_file() and p.suffix in suffixes:
                if ".git" in p.parts:
                    continue
                out.append(p)
    return out


def strip_c_comments(text: str) -> str:
    """粗剥注释与字符串，供纯文本标识符扫描用（不追求完整词法）。"""
    out = []
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        if c == "/" and i + 1 < n and text[i + 1] == "/":
            j = text.find("\n", i)
            i = n if j < 0 else j
        elif c == "/" and i + 1 < n and text[i + 1] == "*":
            j = text.find("*/", i + 2)
            i = n if j < 0 else j + 2
            out.append(" ")
        elif c in "\"'":
            quote = c
            i += 1
            while i < n:
                if text[i] == "\\":
                    i += 2
                    continue
                if text[i] == quote:
                    i += 1
                    break
                i += 1
            out.append(" ")
        else:
            out.append(c)
            i += 1
    return "".join(out)


if __name__ == "__main__":
    sys.stderr.write("common.py 是库，不直接运行；用 tools/policy.py 等入口\n")
    sys.exit(EXIT_ENV)
