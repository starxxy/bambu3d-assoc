// redeem_gen — 每日兑换码生成器（开发者本机用，不随安装包分发）。
//
// 用法:
//   redeem_gen                     输出今天的兑换码
//   redeem_gen 20261001            输出指定日期的兑换码
//   redeem_gen 20261001 20261002   批量输出
//
// 算法与 src/license.cpp 共用同一份代码（链接 bs3d_lib），不存在两套实现;
// tools/redeem_check.py 是独立参考实现，用于交叉校验算法一致性。
#include "bs3d/license.h"

#include <windows.h>
#include <cstdio>

int wmain(int argc, wchar_t** argv) {
    if (argc <= 1) {
        const std::wstring code = bs3d::redeemCodeForDate(nullptr);
        if (code.empty()) { fwprintf(stderr, L"生成失败\n"); return 1; }
        wprintf(L"%s\n", code.c_str());
        return 0;
    }
    for (int i = 1; i < argc; ++i) {
        const std::wstring code = bs3d::redeemCodeForDate(argv[i]);
        if (code.empty()) { fwprintf(stderr, L"日期无效: %s\n", argv[i]); return 1; }
        wprintf(L"%s  %s\n", argv[i], code.c_str());
    }
    return 0;
}
