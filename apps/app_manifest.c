/* apps/app_manifest.c —— 本仓库自带的清单实现。
 *
 * 只做一件事：把描述体地址排成一个数组。这里**不判断**、不排序、不筛
 * `APP_FLAG_SYSTEM`——那些是 `fw_init()` / `fw_boot()` 的事。清单越蠢越好，
 * 因为它是唯一由多个身份（tag 1 与 tag 2）共同追加的文件。
 */
#include "apps/app_manifest.h"

#include "apps/demo/app_demo.h"

static const app_desc_t *const s_apps[] = {
    &app_demo_desc,
};

const fw_manifest_t apps_manifest = {
    s_apps,
    (uint32_t)(sizeof s_apps / sizeof s_apps[0]),
};
