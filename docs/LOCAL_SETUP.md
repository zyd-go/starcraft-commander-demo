# 本地运行

## 已安装完整版本的电脑

当前完整程序位于 `E:\StarCraft_llmteam\commander_desktop\星际指挥台.exe`。直接打开它即可按 README 的步骤演示。本仓库是轻量源码交接，不会替换电脑上已安装的程序。

## 从仓库运行源码

需要准备：

- Windows，带 Tkinter 的 Python 3（本机使用 Anaconda）。
- StarCraft: Brood War 1.16.1、BWAPI 及本地注入启动工具 `injectory.x86.exe`。
- 三张固定版本地图，放在游戏实验目录的 `benchmark/maps/`。
- 对应版本的 `baseline/Bot.cpp` 和 `baseline/Baseline.dll`，必须匹配 `baseline/manifest.json`。
- 十个固定版本对手包：`opponents/<名称>/bwapi-data/AI/`，必须匹配清单；对手包内的配置、数据文件也要保留。
- cnc-ddraw 7.1 的 `ddraw.dll` 与 `Shaders/xbr/xbr-lv2-noblend.glsl`。
- PurpleWave 所需 Java；Skynet 所需旧版 `BWAPI.dll`，默认位于实验目录的 `opponents/C/Skynet/BWAPI.dll`。

默认实验目录为 `E:\StarCraftAI`，游戏位于其 `StarCraft1161` 子目录。可以在 PowerShell 设置以下环境变量；路径要改为真实位置：

```powershell
$env:STARCRAFT_LAB = 'E:\StarCraftAI'
$env:STARCRAFT_DISPLAY = 'E:\StarCraft_llmteam\human_commander_local\vendor\cnc-ddraw-v7.1'
# 如需 PurpleWave，可指定 java.exe；也兼容实验目录中原有 local.settings.json 的 java 配置。
$env:STARCRAFT_JAVA = 'C:\path\to\java.exe'
```

将本地已验证的 `CommanderLayer.dll`、`ReplayControl.dll`、`ResultsWatcher.dll`、`LegacyStorm.dll` 放入仓库根目录的 `build/`，或按下文编译。请使用同一版本的源码和 DLL。

在仓库目录执行：

```powershell
python -B app/matches.py --validate
python -B app/desktop.pyw
```

校验会检查基线、三张地图及对手入口文件哈希；游戏启动还依赖完整的配置与运行文件。对局写入 `matches/`，回放临时实例写入 `playback/`，都在 Git 忽略列表中。

## 编译原生适配层

需要 CMake、MSVC C++ 工具链、BWAPI 头文件与 **x86** 导入库、BWEM 源码。打开 **x86 Native Tools Command Prompt for VS**，在仓库根目录执行：

```bat
cmake -S app -B build -G "NMake Makefiles" -DCMAKE_BUILD_TYPE=Release -DSTARCRAFT_LAB=E:/StarCraftAI
cmake --build build
```

默认查找实验目录下 `dependencies/bwapi` 和 `dependencies/strategy-libs/bwem/BWEM`，可以通过 CMake 的 `BWAPI_ROOT`、`BWEM_ROOT` 覆盖。

仓库中的 CMake 已移除废弃的 MP4 录制目标，使用仓库内的 `Candidate.def`，不需要原电脑上的外部导出文件。只构建人工指挥与回放等适配层，不编译或修改冻结基线。

## 演示检查

选择一个已验证的组合（例如 Heartbreak Ridge / PylonPuller），进入对局后检查八键和手动镜头。结束后应能查看战绩，点击 OK 返回指挥台，历史中出现 `.rep`；播放时显示地图、双方颜色与对手名称。该过程会创建真实对局，需手动开始。
