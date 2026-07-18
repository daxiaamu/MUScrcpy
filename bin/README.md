# 更新 scrcpy 组件

MU投屏通过本目录中的 `scrcpy.exe`、`adb.exe`、`scrcpy-server` 及相关 DLL 完成投屏和设备连接。

官方发布地址：<https://github.com/Genymobile/scrcpy/releases>

## 更新方法

1. 关闭 MU投屏及所有正在运行的 scrcpy 窗口。
2. 前往上面的官方发布页面，下载最新的 Windows 压缩包：
   - 32 位系统或 Win32 版 MU投屏：`scrcpy-win32-v版本号.zip`
   - 64 位系统或 Win64 版 MU投屏：`scrcpy-win64-v版本号.zip`
3. 解压下载的压缩包。
4. 删除本 `bin` 目录中除 `README.md` 以外的旧文件。
5. 将官方压缩包内文件夹中的**全部文件**复制到本 `bin` 目录。
6. 确认以下文件直接位于 `bin` 下，而不是多套了一层目录：
   - `bin\scrcpy.exe`
   - `bin\scrcpy-server`
   - `bin\adb.exe`
7. 重新启动 MU投屏，在“帮助 → 关于”中检查当前 scrcpy 版本。

## 注意事项

- 必须整体替换官方压缩包中的全部文件，不要只替换 `scrcpy.exe` 或 `scrcpy-server`。
- 不要混用不同 scrcpy 版本的 EXE、DLL、ADB 和 server 文件。
- 不要在 Win32 与 Win64 组件之间混合复制文件。
- MU投屏只从程序目录下的 `bin` 读取组件，不会回退到其他 scrcpy 目录。
