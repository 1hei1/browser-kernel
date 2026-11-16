# Windows x64 分段编译（GitHub Actions 免费 runner）

工作流：`.github/workflows/mz-win-build.yml`（由 `ci/win/gen_workflow.py` 生成，改段数/时间预算改生成器再重跑）。

流程：
1. stage-1：depot_tools + `gclient sync --no-history` 拉 Chromium `CHROMIUM_TAG` → `gclient runhooks`
   → 写 `out/Default/args.gn`（`ci/win/args.gn`）→ `uv run python -m driver apply-and-prepare`
   （branding 的 sync_gn_args 追加 `chrome_executable_name="mz"`、`is_mz_branded=true`）→ `gn gen` → ninja。
2. ninja 到时间预算（默认距 job 开始 285 分钟）被 CTRL_BREAK 中断，整棵工作树（去掉 .git）打成 7z
   上传为 artifact `mz-tree`；下一段在同一路径解压后继续 ninja。
3. 编完的那一段从 `out/Default/chrome.7z`（mini_installer 产物）取 `Chrome-bin`，补上 msvcp140*.dll，
   打成 `mz-browser-kernel-<ver>-win-x64.zip`（artifact `mz-kernel`），并和
   `reference-157.0.8103.51-mz3.txt`（上游 release 文件清单）对比，结果写在 job summary。

触发：推送到 `ci/**` 分支且改动了工作流或 `ci/win/**`，或在 Actions 页手动 Run workflow。

ninja 编译出错（不是超时）时，这一段照样把工作树上传为 `mz-tree`（保留 3 天）然后标红。
修好后把改动放进 `ci/win/fixups/`（`files/<chromium 路径>` 整文件覆盖，或 `*.patch` 只打一次），
同时改好 overlay 本身，再用 `resume_run_id=<失败的 run id>` 手动重跑，只重编受影响的部分。

当前分支 `ci/m158-win`：Chromium 158.0.8127.37。上游没有 158 的 release，`reference-158.0.8127.37-mz3.txt`
是把 157 清单的版本目录改名得到的近似对比。
