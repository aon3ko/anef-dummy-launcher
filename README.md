# Arknights: Endfield dummy launcher

English [中文](./README.zh.md)

A simple launcher, launch the game directly without go through the fat official launcher.

## Build

Build `anef_dummy_launcher.sln` with Visual Studio 2022/2026, or run from command line:

```
msbuild anef_dummy_launcher.sln /m /p:Configuration=Release /p:Platform=x64
```

## Usage

You must have MSVC v14 Redistributable installed to run this program.

Place `build\x64\Release\anef_dummy_launcher.exe` beside `Endfield.exe`, then you can run the game by executing `anef_dummy_launcher.exe`.

### Command line arguments

Usually you don't have to adjust the default behavior, but in case you have to:

```
anef_dummy_launcher.exe [--game PATH] [--parent PATH] [-- ARGS...]
```

- game: Default `Endfield.exe` under the same folder.
- parent: Default `PlatformProcess.exe` under the same folder.
- ARGS: argument(s) after `--` will be passthrough to game directly.
