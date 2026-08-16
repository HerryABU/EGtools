# `redist/` — 第三方运行库（构建依赖，不纳入版本库）

本目录下的二进制文件**已被仓库根目录的 `.gitignore` 忽略**（`redist/*`），
因此克隆仓库后需要自行补齐以下文件，才能成功运行 `build_all.ps1` 与 `build_installer.iss`。

## 需要的文件

| 路径 | 说明 | 获取方式 |
| --- | --- | --- |
| `redist/vc143_x64/` | VC++ 2022 (v14) x64 运行库：`vcruntime140*.dll`、`msvcp140*.dll`、`concrt140.dll`、`vccorlib140.dll` 等 | 安装 [Microsoft Visual C++ 2015–2022 Redistributable (x64)](https://learn.microsoft.com/cpp/windows/latest-supported-vc-redist)，从其安装目录（如 `C:\Windows\System32`）复制，或直接解包 `vc_redist.x64.exe` |
| `redist/WindowsAppRuntimeInstall-x64.exe` | Windows App Runtime 1.6 安装包（WinUI 3 运行依赖，`build_installer.iss` 会静默安装） | `winget download -e --id Microsoft.WindowsAppRuntime.1.6` 得到 `WindowsAppRuntimeInstall-x64.exe`；或从 [Windows App SDK 下载页](https://learn.microsoft.com/windows/apps/windows-app-sdk/downloads) 获取 |
| `redist/rt16/` | Windows App Runtime 1.6.9 zh-CN 安装包与元数据（备用，安装包当前使用上一项根目录的 exe） | 同 Windows App Runtime 1.6 官方渠道 |

## 目录示例

```
redist/
├── vc143_x64/
│   ├── vcruntime140.dll
│   ├── vcruntime140_1.dll
│   ├── msvcp140.dll
│   └── ...（其余 VC++ 运行库 DLL）
├── WindowsAppRuntimeInstall-x64.exe
└── rt16/
    ├── Windows App Runtime 1.6_1.6.9_X64_exe_zh-CN.exe
    └── Windows App Runtime 1.6_1.6.9_X64_exe_zh-CN.yaml
```

> 注意：这些文件均为第三方分发包，体积较大（约 130 MB），且受各自许可协议约束，
> 故不随源码提交。请在合规前提下获取并用于本地构建。
