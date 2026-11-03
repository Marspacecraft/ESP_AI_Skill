放公共层头：`cm_types.h`（`cm_err_t` 与基础类型）、`cm_atomic.h`（跨任务原子访问）。
谁写：tag 5。

规则：公共层不许 include `api/` 或 `framework/`（那会造成反向依赖）——
`common/` 是最底层，谁都能 link 它，所以它不能反过来认识任何人。

`cm_atomic.h` 只包五个宏（load / store / exchange / add_fetch / sub_fetch）+ 一个内存序枚举，
底下是 `__atomic_*`。**为什么要有这一层、为什么不用 `volatile` 与 `<stdatomic.h>`、
哪一档序该用在哪里**，全部写在那个头的注释里——跨任务读写的地方只经它，不要直接调内建。
