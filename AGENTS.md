# 工程级变更记录与代理指引

## 修改 AFE 电芯拓扑前必须检查

凡修改 AFE 电压扫描、逻辑串数、单体电压映射/校准、均衡位图或相关通信/日志逻辑，必须先阅读并保留下方变更记录 **CHG-AFE-TOPCELL-MAP-20261010-01**。若改动该方案，请同步更新此记录和 `tool/tests/app_cell_topology_test.c`，不可只改运行代码。

## CHG-AFE-TOPCELL-MAP-20261010-01 — 24 路扫描与逻辑末节映射

- **目的**：适配当前板级接线：最顶节接在 DVC1124 第 24 路（物理数组下标 23），但电池包实际串数按逻辑配置，不一定是 24S。
- **当前配置**：逻辑串数 `APP_BMS_CELL_COUNT = 20`；AFE 每轮扫描 `APP_BMS_AFE_SCAN_CELL_COUNT = 24` 路；末节物理通道 `APP_BMS_AFE_TOP_CELL_INDEX = 23`。
- **电压映射规则**：逻辑索引 `0..N-2` 映射到同下标物理通道；逻辑末节 `N-1` 映射到物理下标 23。例：20S 的 `cell_mv[19]` 取 `gt_hsi_afe.vol[23]`；21S 的 `cell_mv[20]` 取 `gt_hsi_afe.vol[23]`；24S 时映射自然等同连续排列。
- **数组容量规则**：业务快照 `cell_mv` 按逻辑串数配置 `APP_BMS_CELL_COUNT` 分配，当前 Watch 展开20项；AFE原始 `vol[]` 和扫描缓冲仍按 `APP_BMS_MAX_CELL_NUM`/24路分配。未来改21S时只改逻辑串数配置，快照数组随之变为21项，AFE仍扫描24路。
- **均衡映射规则**：业务层使用逻辑位图，写入 AFE 前反向映射；逻辑末节位必须落到物理 bit23。电压校准偏差按逻辑槽位索引应用，不能把物理下标 23 当成逻辑电芯编号。
- **配置变更**：改为 21S 时只将 `APP_BMS_CELL_COUNT` 改为 21；24 路扫描数和物理末节索引保持不变。不要把 24 路扫描数误当成电池包串数。
- **实现位置**：`app/N32G45x_KEIL/User_Src/App/app_param.h`、`app_cell_topology.h`、`app_bms_snapshot.c`、`app_balance_manage.c`、`app/N32G45x_KEIL/User_Src/HSI_afe/hsi_afe.c` 与 `hsi_afe.h`。串数相关日志/通信计算也以逻辑串数为准。
- **回退提示**：若要取消“末节固定在物理通道 24”的接线映射，应先确认硬件接线已改变；随后将 `app_cell_topology.h` 的逻辑到物理映射恢复为连续下标，并移除 `app_balance_manage.c` 的反向位图映射，同时更新拓扑单元测试。仅改配置串数或扫描数并不能单独撤销该映射。
- **验证要求**：运行 `tool/tests/app_cell_topology_test.c` 对快照数组容量及 20S/21S/24S 的电压和均衡映射进行验证，并运行 `tool/tests/hsi_afe_dvc1124_contract_test.ps1`；改动 AFE 读数/任务时再用目标 Keil APP 工程完整编译。

## Keil 工程命令行编译约定

- APP 和 BOOT 工程一律通过命令行编译，使用 `E:\Keil_v5\UV4\uVision.com`；不要启动 `UV4.exe`，也不要切换或操作用户桌面。
- APP 工程目标为 `N32G45x`，命令格式：
  `& 'E:\Keil_v5\UV4\uVision.com' -b '<工程根目录>\app\N32G45x_KEIL\MDK-ARM\N32G45x.uvprojx' -t N32G45x -o '<工程根目录>\app\N32G45x_KEIL\MDK-ARM\Objects\build.log' -j0`
- BOOT 工程目标为 `boot`，命令格式：
  `& 'E:\Keil_v5\UV4\uVision.com' -b '<工程根目录>\boot\Projects\MDK\boot.uvprojx' -t boot -o '<工程根目录>\boot\Projects\MDK\Objects\build.log' -j0`
- 编译后检查退出码和日志中的错误数；确认产物生成后再同步到 `tool`。APP 工程的 Post-Build 会调用 `tool.exe`，需检查相关产物和工作区变更。
- 若受限环境中 `armlink.exe` 因“无效的系统 DLL 重定位”无法启动，在获准的命令行进程环境中重试同一 `uVision.com` 命令；不要改用桌面窗口来绕过。
