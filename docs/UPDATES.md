# Automatic updates / 自动更新

Starting with version 0.2, DXL checks the latest stable GitHub release and its local update cache in the
background on startup. Nothing is shown when no newer version is available.
If a new version exists, DXL shows its release notes and a Download button.
After verification, choose Install and restart or Not now. A complete deferred
download is offered again on the next launch, including when offline.

DXL 从 0.2 起，每次启动都会在后台检查 GitHub 最新正式版及本地更新缓存。
没有更新时不提示；有更新时显示版本、更新说明和下载按钮。
下载校验完成后，可选择安装并重启或暂不安装。暂不安装的完整包会保留，
下次启动时再次提示，离线也可安装。

Cache / 缓存：`%LOCALAPPDATA%\DXL\updates`

Close games and other DXL windows before installation. The standalone updater
first verifies and extracts the package, then asks DXL to exit, replaces the files,
and restarts DXL. If preflight fails, DXL stays open and displays the error.
If file replacement fails, it restores the previous files and retains the download.
Successfully installed and older DXL update archives are removed automatically.
Game profiles, preferences and unrelated installation-folder files are preserved.

安装前请退出游戏和其他 DXL 窗口。独立更新程序会先校验并解包，成功后才通知
DXL 退出，覆盖程序文件并自动重启。预检失败时保留工具窗口并显示错误。
文件替换失败时恢复原文件，并保留下载包。
安装成功后自动删除已安装及更旧的 DXL 更新压缩包。游戏配置、偏好设置及
安装目录内与 DXL 更新无关的文件会保留。

## Release packaging / 发布打包

The updated 1.0 client reads `update.json` from the latest stable GitHub Release. Upload the complete
versioned Windows ZIP and its matching `update.json` as separate release assets. Players already running
the same version see no update prompt, including when its assets are replaced.

新版 1.0 从 GitHub 最新正式版的附件读取 `update.json`。将带版本号的 Windows 完整包和配套的
`update.json` 作为两个独立附件上传。同版本替换附件时，已安装该版本的玩家不会收到更新提示。

After creating the ZIP, run this command with the UTF-8 release description. 打好 ZIP 后，用 UTF-8
发布说明文件运行以下命令。

```powershell
python scripts/make_update_manifest.py DXL-v1.0-win64.zip release-notes.md
```

The ZIP must contain `DXL-v1.0/DXL.exe` and the package manifest from `release_dxl.ps1`, with the
folder and manifest version matching the filename. The script writes `update.json` beside the ZIP.
Upload the matching ZIP first and `update.json` last when replacing live assets. The client checks the
ZIP size and SHA-256 from the release asset, then verifies package file hashes and the executable version.

ZIP 内应包含 `DXL-v1.0/DXL.exe` 和 `release_dxl.ps1` 生成的包内清单，文件夹与清单的版本须与
ZIP 文件名一致。脚本在 ZIP 旁生成 `update.json`。替换已发布的附件时，先传 ZIP，最后传
`update.json`。客户端校验 ZIP 的大小和 SHA-256，解包后再核对包内文件哈希及程序版本。
