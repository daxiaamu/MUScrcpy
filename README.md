# MU投屏

MU投屏是一款面向 Windows 的轻量 Android 投屏启动器，基于 [scrcpy](https://github.com/Genymobile/scrcpy)，使用原生 Win32 C 编写。

程序会自动检测 USB 调试设备、读取设备信息并循环启动 scrcpy。设备断开后保持等待，重新连接时自动恢复投屏。

## 功能

- 自动读取型号、序列号、Android 版本、系统版本、A/B 槽位与内核版本
- 自动、流畅、均衡、高清、极致五档画质
- 保持亮屏、投屏置顶与软件渲染兼容模式
- 检测手机屏幕状态并一键点亮
- scrcpy 窗口自动贴靠与前台联动
- 断线等待和自动重新投屏
- 实时运行日志
- “关于”界面显示当前 scrcpy 组件版本

完整的功能介绍见 [optool.daxiaamu.com/muscrcpy](https://optool.daxiaamu.com/muscrcpy)。

## 运行

可直接从 [Releases](https://github.com/daxiaamu/MUScrcpy/releases) 下载包含 scrcpy 的完整包：

- `MUScrcpy-v2.3.1-win64-scrcpy-v4.1.zip`：适用于 64 位 Windows
- `MUScrcpy-v2.3.1-win32-scrcpy-v4.1.zip`：适用于 32 位 Windows

从源码自行准备运行目录时：

1. 下载官方 [scrcpy Windows 版本](https://github.com/Genymobile/scrcpy/releases)。
2. 在程序目录创建 `bin` 文件夹，将 scrcpy 发布包中的全部文件放入其中。
3. 确认 `bin\adb.exe` 与 `bin\scrcpy.exe` 存在。
4. 在 Android 设备上开启 USB 调试，连接电脑后运行 `MUScrcpy.exe`。

## 编译

使用 MinGW-w64：

```bat
编译c.bat
```

等效命令：

```bat
windres start_scrcpy_c.rc -O coff -o start_scrcpy_c.res
gcc start_scrcpy_c.c start_scrcpy_c.res -municode -mwindows -O2 -s -o MUScrcpy.exe -lcomctl32 -lgdi32 -ladvapi32 -lshell32
```

## 第三方项目

投屏、设备控制和音视频传输由 [Genymobile/scrcpy](https://github.com/Genymobile/scrcpy) 提供。请同时遵守 scrcpy 及其发布包内第三方组件的许可证。
