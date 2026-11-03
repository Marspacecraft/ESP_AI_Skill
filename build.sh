#!/bin/sh
# ESP AI Skill —— 唯一的构建入口（步骤 0.2）
#
# 用法：
#   ./build.sh        只编译
#   ./build.sh run    编译，然后逐个执行
#
# 约定（改这里 = 改全仓构建，先看 实施步骤.md §2）：
#   1. 每个 tests/**/test_*.c 各自链成一个可执行文件——它们各自有 int main。
#   2. 非测试实现：common/src framework/src platform/*/ apps/*.c（存在才编）。
#   3. 一个文件编不过只影响它自己，其余继续；最终退出码非 0。
#   4. `run` 双重判定：退出码非 0 **或** 输出里有 `^FAIL ` 都算红——
#      防止某个 main 忘了 return tt_summary() 把失败吞掉。
#   5. 没有测试文件时打印 `no tests` 且退出码 0：空集不是失败，
#      但也**不许说"通过"**（"no tests" 不是 "pass"）。
set -u

ROOT=$(cd "$(dirname "$0")" && pwd)
cd "$ROOT"

CC=${CC:-cc}
WARN="-std=c11 -Wall -Wextra -Werror"
# -std=c11 会关掉 glibc 的 POSIX 默认暴露（__STRICT_ANSI__），
# clock_gettime / nanosleep / pthread_* 就都不声明了；显式要 POSIX.1-2008。
DEFS="-D_POSIX_C_SOURCE=200809L"
# macOS 在 __STRICT_ANSI__ 下只暴露 POSIX 子集，pthread_setname_np 这类
# 非 POSIX 扩展会消失；_DARWIN_C_SOURCE 把完整 Darwin API 要回来（Linux 无影响）。
case "$(uname -s)" in
    Darwin) DEFS="$DEFS -D_DARWIN_C_SOURCE" ;;
esac
THREADS="-pthread"          # platform/host-fake 用 pthread 实现端口九项
INC="-I. -Icommon/include -Itests"
OUT=build

mkdir -p "$OUT"

list_lib_srcs() {
    # tests/support 里的夹具可以带实现（.c），但 `test_*.c` 是**用例**，
    # 每个都有自己的 main——混进库源会让每个二进制撞上重复符号 `_main`。
    {
        # platform/ai-passport-c3/ 是 ESP-IDF 端口（include freertos/ 与 esp_timer.h），
        # host 上编不过，必须排除；它的验收走 tests/port/idf 的独立 IDF 工程。
        find common/src framework/src platform apps -type f -name '*.c' \
            ! -path 'platform/ai-passport-c3/*' 2>/dev/null
        find tests/support -type f -name '*.c' ! -name 'test_*.c' 2>/dev/null
    } | sort
}

list_tests() {
    find tests -type f -name 'test_*.c' 2>/dev/null | sort
}

build_one() {
    src=$1
    name=$(basename "$src" .c)
    log="$OUT/$name.log"
    # $WARN $INC $LIBSRC 要按空格拆分，故意不加引号。
    # shellcheck disable=SC2086
    if $CC $WARN $DEFS $THREADS $INC -o "$OUT/$name" "$src" $LIBSRC 2>"$log"; then
        echo "BUILD ok   $name"
        return 0
    fi
    echo "BUILD FAIL $name  (编译器输出见 $log)"
    sed -n '1,20p' "$log" | sed 's/^/    /'
    return 1
}

TESTS=$(list_tests)
if [ -z "$TESTS" ]; then
    echo "no tests"
    exit 0
fi

LIBSRC=$(list_lib_srcs)

rc=0
built=""
for t in $TESTS; do
    if build_one "$t"; then
        built="$built $t"
    else
        rc=1
    fi
done

if [ "${1:-}" != "run" ]; then
    [ "$rc" -eq 0 ] || exit 1
    exit 0
fi

for t in $built; do
    name=$(basename "$t" .c)
    out="$OUT/$name.out"
    "./$OUT/$name" >"$out" 2>&1 || rc=1
    cat "$out"
    if grep -q '^FAIL ' "$out"; then
        rc=1
    fi
done

# 静态自检：不是测试，靠退出码判定。任何一条红了都算整轮失败。
#   check_entry_unique.sh —— 全仓只许有一处 app_main（决定 32）。
if [ -x tests/tools/check_entry_unique.sh ]; then
    if ! tests/tools/check_entry_unique.sh; then
        rc=1
    fi
fi
#   check_link.py —— 公开头的依赖闭包必须落在读者自己的 link 集合里。
#   它判的是"App 的编译单元里有没有平台表"，这是 write 面管不到的事。
if [ -f tools/check_link.py ]; then
    if ! python3 tools/check_link.py; then
        rc=1
    fi
fi

exit "$rc"
