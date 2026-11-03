# Arknights: Endfield dummy launcher

[English](./README.md) 中文

一個簡單的啓動器，直接啓動遊戲而不經官方臃腫的啓動器。

## 構建

使用 Visual Studio 2022/2026 構建 `anef_dummy_launcher.sln` ，或執行如下命令行命令：

```
msbuild anef_dummy_launcher.sln /m /p:Configuration=Release /p:Platform=x64
```

## 使用

你需要安裝 MSVC v14 Redistributable 來運行此程序。

將 `build\x64\Release\anef_dummy_launcher.exe` 放置於 `Endfield.exe` 的統一文件夾下，而後使用 `anef_dummy_launcher.exe` 來運行遊戲。

### 命令行選項

通常情況下無需調整默認軟件行爲，但若你有需要：

```
anef_dummy_launcher.exe [--game PATH] [--parent PATH] [-- ARGS...]
```

- game: 默認值爲同一文件夾下的 `Endfield.exe` 。
- parent: 默認值爲同一文件夾下的 `PlatformProcess.exe` 。
- ARGS: 於 `--` 後填寫的參數將被直接傳遞給遊戲。
