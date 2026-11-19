# Linux x64 分段编译（无桌面服务器 / agent 用）

工作流：`.github/workflows/mz-linux-build.yml`（由 `ci/linux/gen_workflow.py` 生成）。流程同 `ci/win`：
stage-1 拉 Chromium、`install-build-deps.sh`、runhooks → `ci/linux/args.gn` + driver apply-and-prepare → gn gen → ninja；
超时就把工作树（去掉 .git）打成 tar.zst 传给下一段；编完打包 `mz-browser-kernel-<ver>-linux-x64.tar.xz`。
编译出错同样保留工作树，修好后放进 `ci/linux/fixups/`，用 `resume_run_id` 续编。

运行时（服务器上）：
- 内核通过 Unix socket 和控制端通信：`$MZ_EVENTBUS_DIR/mz_<env_id>.sock`（默认 `/tmp`），协议与 Windows 命名管道一致。
- 无 GPU：`--headless=new --use-angle=swiftshader --enable-unsafe-swiftshader`，否则 WebGL 不可用。
- 依赖：`libnss3 libatk-bridge2.0-0 libgbm1 libxkbcommon0 libasound2t64 libcups2 libxcomposite1 libxdamage1 libxrandr2 libpango-1.0-0` 等。

当前分支 `ci/m158-linux`：Chromium 158.0.8127.37。
