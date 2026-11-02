放公共层头：cm_types.h（cm_err_t 与基础类型）、cm_atomic.h、cm_list.h、cm_ring.h。谁写：tag 5。
规则：公共层不许 include api/ 或 framework/（那会造成反向依赖）。
