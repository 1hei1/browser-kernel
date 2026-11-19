# 每环境独立通行密钥（passkey）

每个 MZ 环境在浏览器进程里有一个**持久化的软件平台验证器**，替代 Windows Hello：
`navigator.credentials.create/get` 创建和使用的通行密钥属于这个环境，换环境即换一套，
不经过 Windows Hello，也不写进 Windows 凭据库。

## 工作方式

- 复用 `content/browser/webauth` 的虚拟验证器（DevTools WebAuthn 面板 / WebDriver 用的同一套）。
  `AuthenticatorEnvironment::MaybeGetVirtualAuthenticatorManager()` 在没有 DevTools 测试环境时
  返回一个全局的 `VirtualAuthenticatorManagerImpl`，所以**所有 frame（含跨域 iframe）**都用它。
  DevTools/WebDriver 对某个 frame 打开 WebAuthn 测试环境时仍然优先用它们自己的。
- 验证器参数：CTAP2.0、transport `internal`、attachment `platform`、resident key、
  内置用户验证（`is_user_verified=true`，用户在场自动通过），凭据容量不设上限。
- 证明：始终返回 `none` 证明；AAGUID 默认用 Windows Hello 的
  `08987058-cadc-4b81-b6e1-30de50dcbe96`（Chrome 对平台验证器即使 attestation=none 也保留 AAGUID），
  可用 LaunchConfig 的 `passkey_aaguid` 覆盖。BE/BS 标志为 0（与 Windows Hello 一致）。
- `PublicKeyCredential.isUserVerifyingPlatformAuthenticatorAvailable()` → true（content 的
  WebAuthenticationDelegate 对虚拟环境返回“有 UV 平台验证器”）。
  `isConditionalMediationAvailable()` / `getClientCapabilities().conditionalGet` → true。
- 界面：默认 **保留 Chrome 自己的通行密钥对话框**（`show_ui=true`）。这样多账号时能选账号，
  `mediation: "conditional"` 走密码框自动填充（Chrome 的虚拟验证器实现了平台凭据枚举），
  条件式创建（passkey upgrade）也不会在后台静默生成密钥。虚拟环境下 Chrome 不提供
  Google 密码管理器通行密钥，Windows Hello 也不会出现（虚拟 discovery 不创建 Windows API 验证器）。
  LaunchConfig 里 `passkey_silent: true` 时完全静默（UI 关闭，自动选第一个账号，条件式请求会立即完成），
  只建议自动化场景使用。
- 超时和 TLS：普通虚拟环境会跳过超时限制和证书检查；这里通过 `MzIsPasskeyAuthenticatorManager()`
  区分，MZ 验证器照常限制超时并要求正常证书。

## 持久化

- 应用 → 内核：LaunchConfig 的 `passkeys` 数组（**带这个字段才启用**，空数组也启用；旧版应用不发，行为不变）：
  ```json
  {"credential_id":"<b64url>","rp_id":"webauthn.io","user_handle":"<b64url>",
   "user_name":"a","user_display_name":"a","private_key":"<b64url PKCS#8 DER>",
   "sign_count":3,"created_at":1763539200000,"discoverable":true}
  ```
  启动时注入 `State::registrations`；私钥只在浏览器进程内存里。
- 内核 → 应用：EventBus 事件 `Topic::kPasskeyChanged = 0x0901`，JSON：
  `{"action":"created|updated|deleted","credential_id":...}`，created/updated 带上面全部字段
  （每次断言后签名计数 +1 也会发 updated）。deleted 来自站点用同一用户重新注册（旧凭据被替换）
  或 `PublicKeyCredential.signalUnknownCredential / signalAllAcceptedCredentials`。
  EventBus 未连接时事件丢失（凭据只活到这次浏览器关闭），日志会记录“上报失败”。

## 文件

- `sources/content/public/browser/mz_passkey.h`：给 //mz 用的公开接口（只依赖 base），实现在
  `authenticator_environment.cc`（没有改 content 的 BUILD.gn）。
- `001`：`VirtualCtap2Device::Config::mz_aaguid`。
- `002`：`VirtualAuthenticator::SetMzPlatformAuthenticator()`（none 证明、AAGUID、容量、getInfo 只报 internal）。
- `003`：全局验证器 + 观察者 + `mz_passkey` 实现；`authenticator_common_impl.cc` 的超时/TLS 区分。
- `004`：EventBus 解析 `passkeys`、上报 `kPasskeyChanged`。

## 已知限制 / 取舍

- 一个浏览器进程一个验证器：同一环境的隐身窗口和普通窗口共用这些通行密钥。
- 不支持 PRF / hmac-secret / largeBlob / credBlob（需要 PIN 协议，虚拟设备实现复杂度高；Windows Hello 本身也不全支持）。
- 不支持混合传输（手机扫码）和 USB 安全密钥：虚拟环境下 Chrome 不会创建真实的 HID/caBLE discovery。
  对话框里可能仍列出“USB 安全密钥”等入口，选了只会等待超时。
- 签名计数从 1 开始递增（Windows Hello 通常为 0）。
- 没有在本地编译/运行，只在 CI 编译；行为需在 https://webauthn.io 实测。
