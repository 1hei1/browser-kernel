# TOTP 模块（2FA 验证码）

LaunchConfig 的 `accounts[].totp_secret`（Base32 或 otpauth:// URI）只保存在浏览器进程内存（`mz/totp`），不落盘。
新标签页通过 `mz_ip.mojom` 的 `GetTotpCodes()` 读取计算好的验证码（RFC 6238，默认 SHA-1 / 6 位 / 30 秒），
左下角面板显示站点、账号、验证码和剩余秒数，点击复制。

- `sources/mz/totp/`：deploy 到 Chromium `mz/totp/`
- `patches/001-eventbus-totp.patch`：eventbus 解析 totp_secret 并写入存储，eventbus 依赖 `//mz/totp`
- 新标签页部分在 `overlay/ntp/mz_ip/` 与 `overlay/ntp/resources/mz_ntp.ts`
