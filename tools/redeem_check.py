#!/usr/bin/env python3
# 独立参考实现：与 src/license.cpp / tools/redeem_gen.cpp 做逐字节交叉校验。
# 仅开发者本机使用，不随安装包分发。纯标准库。
#
# ⚠️ 已退役 + 已脱敏：本脚本服务于 v1.2.4 及更早版本的「每日兑换码」付费机制，
#    该机制自 v1.3.0 起退役。公开仓库中此处不再包含真实密钥 —— 若需复核算法，
#    请自行填入原密钥（仅在本机使用，切勿提交）。
#
# 用法:
#   python redeem_check.py                 # 今天的码
#   python redeem_check.py 20261001 ...    # 指定日期
import hmac
import hashlib
import datetime
import sys

# 脱敏：原密钥为 32 字节 ASCII 串，已从此处移除。
# 格式为 b"b3d-..." ，对应 src/license.cpp 的 (kSecretXor ^ kSecretMask)。
SECRET = b"<RETIRED-SECRET-WITHHELD>"   # 占位符；真实值不公开
B32 = "0123456789ABCDEFGHJKMNPQRSTVWXYZ"       # Crockford，无 I/L/O/U


def code_for(date_str: str) -> str:
    """date_str: yyyymmdd"""
    digest = hmac.new(SECRET, ("B3D1|" + date_str).encode("ascii"),
                      hashlib.sha256).digest()
    acc = 0
    bits = 0
    out = []
    for b in digest[:10]:
        acc = (acc << 8) | b
        bits += 8
        while bits >= 5:
            out.append(B32[(acc >> (bits - 5)) & 31])
            bits -= 5
    s = "".join(out)
    return "-".join(s[i:i + 4] for i in range(0, 16, 4))


if __name__ == "__main__":
    if len(sys.argv) > 1:
        for d in sys.argv[1:]:
            print(d, code_for(d))
    else:
        print(code_for(datetime.date.today().strftime("%Y%m%d")))
