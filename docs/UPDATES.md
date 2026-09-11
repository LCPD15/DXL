# Automatic updates / 自动更新

DXL 0.2 checks the latest stable GitHub release and its local update cache in the
background on startup. Nothing is shown when no newer version is available.
If a new version exists, DXL shows its release notes and a Download button.
After verification, choose Install and restart or Not now. A complete deferred
download is offered again on the next launch, including when offline.

DXL 0.2 每次启动都会在后台检查 GitHub 最新正式版及本地更新缓存。
没有更新时不提示；有更新时显示版本、更新说明和下载按钮。
下载校验完成后，可选择安装并重启或暂不安装。暂不安装的完整包会保留，
下次启动时再次提示，离线也可安装。

Cache / 缓存：`%LOCALAPPDATA%\DXL\updates`

Close games and other DXL windows before installation. The standalone updater
waits for DXL to exit, verifies and replaces package files, then restarts DXL.
If file replacement fails, it restores the previous files and retains the download.
Successfully installed and older DXL update archives are removed automatically.
Game profiles, preferences and unrelated installation-folder files are preserved.

安装前请退出游戏和其他 DXL 窗口。独立更新程序会等待 DXL 退出，校验并覆盖
程序文件，再自动重启。文件替换失败时恢复原文件，并保留下载包。
安装成功后自动删除已安装及更旧的 DXL 更新压缩包。游戏配置、偏好设置及
安装目录内与 DXL 更新无关的文件会保留。

## Release packaging

Publish stable tags such as `v0.3` with a `DXL-v0.3-win64.zip` asset. The ZIP must
contain the `DXL-v0.3/` folder and the manifest produced by `release_dxl.ps1`.
The updater verifies GitHub's asset SHA256 digest, package manifest hashes and
the executable's version. Pre-releases, source/model archives and older/equal
versions are not installation candidates. Keep the updater executable and scripts
in every subsequent complete package. Do not re-use a published version number.
