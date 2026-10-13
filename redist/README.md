# `redist/` — 第三方运行库（构建依赖，不纳入版本库）

本目录下的二进制文件**已被仓库根目录的 `.gitignore` 忽略**（`redist/*`），
因此克隆仓库后需要自行补齐以下文件，才能成功运行 `build_all.ps1` 与 `build_installer.iss`。

## 需要的文件

| 路径 | 说明 | 获取方式 |
| --- | --- | --- |
| `redist/vc143_x64/` | VC++ 2022 (v14) x64 运行库：`vcruntime140*.dll`、`msvcp140*.dll`、`concrt140.dll`、`vccorlib140.dll` 等 | 安装 [Microsoft Visual C++ 2015–2022 Redistributable (x64)](https://learn.microsoft.com/cpp/windows/latest-supported-vc-redist)，从其安装目录（如 `C:\Windows\System32`）复制，或直接解包 `vc_redist.x64.exe` |

> ⚠️ 历史上这里还放过 **Windows App Runtime 1.6**（`WindowsAppRuntimeInstall-x64.exe` 与 `rt16/`，约 124 MB）。
> 那只服务于已被取代的 **WinUI 3** 前端；现前端是 Qt 6（`EGtools.Frontend`），
> `build_all.ps1` 与 `build_installer.iss` 已 **0 处引用**这些文件，故 2026-10-13 已删除，**不要**再补回来。

## 目录示例

```
redist/
├── README.md
└── vc143_x64/
    ├── vcruntime140.dll
    ├── vcruntime140_1.dll
    ├── msvcp140.dll
    └── ...（其余 VC++ 运行库 DLL）
```

> 注意：这些文件均为第三方分发包，受各自许可协议约束，故不随源码提交。
> 请在合规前提下获取并用于本地构建。
