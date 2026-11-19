// MZ 每环境独立通行密钥（passkey）。
//
// 每个 MZ 环境在浏览器进程里持有一个持久化的软件平台验证器，替代 Windows Hello：
// 复用 content/browser/webauth 的虚拟验证器（VirtualAuthenticator / VirtualCtap2Device），
// 对所有 frame 生效（transport internal、CTAP2、resident key、内置用户验证自动通过）。
// 凭据（含 PKCS#8 私钥）只保存在内存里；创建 / 计数变化 / 删除时通过回调交给 MZ EventBus，
// 由 MZ 应用加密落盘，下次启动随 LaunchConfig 下发再注入。
//
// 本头文件只依赖 base，供 //mz 和 //chrome 层调用；实现在
// content/browser/webauth/authenticator_environment.cc。

#ifndef CONTENT_PUBLIC_BROWSER_MZ_PASSKEY_H_
#define CONTENT_PUBLIC_BROWSER_MZ_PASSKEY_H_

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "base/functional/callback.h"
#include "content/common/content_export.h"

namespace content::mz_passkey {

// 一条通行密钥凭据。
struct CONTENT_EXPORT Credential {
  Credential();
  Credential(const Credential&);
  Credential(Credential&&);
  Credential& operator=(const Credential&);
  Credential& operator=(Credential&&);
  ~Credential();

  std::vector<uint8_t> credential_id;
  std::string rp_id;
  std::vector<uint8_t> user_handle;
  std::string user_name;
  std::string user_display_name;
  // PKCS#8 DER 私钥（ES256 / EdDSA / RS256 等，由站点要求的算法决定）
  std::vector<uint8_t> private_key_pkcs8;
  uint32_t sign_count = 0;
  // 创建时间，Unix 毫秒
  int64_t created_at_ms = 0;
  // 是否为可发现凭据（resident key）
  bool discoverable = true;
};

enum class Change {
  kCreated,  // 新建
  kUpdated,  // 签名计数或用户名变化
  kDeleted,  // 被站点替换或通过 signal API 删除（只有 credential_id 有效）
};

using ChangeCallback =
    base::RepeatingCallback<void(Change change, const Credential& credential)>;

struct CONTENT_EXPORT Options {
  Options();
  Options(const Options&);
  Options& operator=(const Options&);
  ~Options();

  // true：使用 Chrome 自带的通行密钥对话框（账号选择、条件式 UI 自动填充）；
  // false：完全静默，自动选第一个账号（适合自动化）。
  bool show_ui = true;
  // 证明里的 AAGUID；不设置时使用 Windows Hello 的 AAGUID。
  std::optional<std::array<uint8_t, 16>> aaguid;
};

// 启用（或重新加载）本进程的软件平台验证器并注入凭据。必须在 UI 线程调用。
// 再次调用会清空旧凭据后重新注入，回调替换为新的。
CONTENT_EXPORT void EnablePlatformAuthenticator(
    std::vector<Credential> credentials,
    const Options& options,
    ChangeCallback on_change);

// 是否已启用。UI 线程。
CONTENT_EXPORT bool IsPlatformAuthenticatorEnabled();

}  // namespace content::mz_passkey

#endif  // CONTENT_PUBLIC_BROWSER_MZ_PASSKEY_H_
