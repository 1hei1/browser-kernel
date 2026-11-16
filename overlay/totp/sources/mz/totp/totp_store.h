// MZ 2FA（TOTP）验证码存储。
// LaunchConfig 下发的账号 totp_secret 只保存在浏览器进程内存里，不落盘；
// 新标签页通过 mz_ip Mojo 接口按需读取当前验证码（RFC 6238，SHA-1/6 位/30 秒）。

#ifndef MZ_TOTP_TOTP_STORE_H_
#define MZ_TOTP_TOTP_STORE_H_

#include <cstdint>
#include <string>
#include <vector>

#include "base/time/time.h"

namespace mz::totp {

struct Account {
  std::string url;
  std::string username;
  std::string secret;  // Base32 或 otpauth:// URI
};

struct Code {
  std::string site;      // 显示用的域名
  std::string username;
  std::string code;      // 6 位验证码
  int32_t remaining = 0;  // 当前验证码剩余秒数
  int32_t period = 30;
};

// 覆盖保存当前环境的账号密钥（空列表表示清空）。线程安全。
void SetAccounts(std::vector<Account> accounts);

// 计算当前时刻全部账号的验证码。密钥无效的账号会被跳过。线程安全。
std::vector<Code> GetCodes(base::Time now);

// 以下函数供测试使用。
bool DecodeBase32(const std::string& input, std::vector<uint8_t>* out);
std::string ComputeCode(const std::vector<uint8_t>& key,
                        uint64_t counter,
                        int digits);

}  // namespace mz::totp

#endif  // MZ_TOTP_TOTP_STORE_H_
