#!/bin/sh
# tests/tools/check_entry_unique.sh —— 全仓只许有一处 app_main 定义（步骤 4.2）。
#
# 为什么需要它：`app_main` 是整机唯一入口（决定 32）。允许出现第二处，就等于允许
# 出现第二套启动链——而"第二套启动链"在旧仓库里真的发生过（仓库根的 app_main.cc
# 直接 pf_init + while(1) pf_loop，绕过 fw_init/fw_boot/清单）。
#
# 它只认"定义"，不认"声明/调用/注释"：
#   - 定义 = 该行以 `app_main` 开头、且行内有 `(`、后续文本里有 `{`
#   - 被允许的那一处：framework/src/fw_main.c
#   - `tests/` 下不算：那里的固件是**自检工程**（如 tests/port/idf 的 ESP-IDF
#     工程），它有自己的入口是正常的；被检查的是产品固件的入口。
#
# 用法：tests/tools/check_entry_unique.sh [仓库根]
# 退出码：0 唯一；1 有多处；2 用法/环境问题（**2 不是通过**）。
set -u

ROOT=${1:-$(cd "$(dirname "$0")/../.." && pwd)}
[ -d "$ROOT" ] || { echo "check_entry_unique: 不是目录：$ROOT" >&2; exit 2; }
cd "$ROOT" || exit 2

ALLOWED="framework/src/fw_main.c"
hits=$(grep -rlnE '^[[:space:]]*(extern[[:space:]]+"C"[[:space:]]+)?void[[:space:]]+app_main[[:space:]]*\(' \
        --include='*.c' --include='*.cc' --include='*.cpp' \
        --exclude-dir=build . 2>/dev/null \
      | sed 's|^\./||' | grep -v '^tests/' | sort)

if [ -z "$hits" ]; then
    echo "check_entry_unique: FAIL -- 没有找到 app_main 的定义（入口没了）"
    exit 1
fi

bad=$(printf '%s\n' "$hits" | grep -vxF -- "$ALLOWED" || true)
if [ -n "$bad" ]; then
    echo "check_entry_unique: FAIL -- app_main 只能定义在 ${ALLOWED}，另有："
    printf '  %s\n' "$bad"
    exit 1
fi

echo "check_entry_unique: PASS (${ALLOWED})"
exit 0
