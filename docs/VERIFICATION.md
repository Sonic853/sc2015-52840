# 本次实现验证记录

## 已执行

```powershell
python tools/test.py --sanitize
python tools/build.py
npm test
npm run test:browser
python tools/verify.py
```

- Windows MSVC 14.51，`/W4 /WX`；AddressSanitizer已启用并通过。
- 本轮使用既有锁定依赖；Windows主suite **111,160条断言**，独立协议兼容suite **10,741条**、RX保留suite **1,512条**，另有USB/桌面suite通过结果；108组独立PyCryptodome CCM向量、128组冻结跳频向量，以及4个Python镜像测试。
- 生产协议和USB桥代码直接参加测试；AES block oracle为Windows BCrypt，独立完整CCM oracle为PyCryptodome。
- 模拟手柄覆盖新配对、初始counter=1密钥更新、普通输入、认证重复、双lane ACK、Feature 27+27+10切片/回复、存储失败后提交、断开及恢复绑定。
- ASan曾发现watchdog轮转到phase6/7时会越过四个slot，现已限制到phase2..5；完整测试随后通过。
- ARM GCC 9.2.1、Seeed nRF52 1.1.13、TinyUSB 3.6.0实际编译通过，无编译警告。
- 新增F0 v2槽身份、冻结绑定快照、分阶段整库导入、取消/超时、旧token、重复ID、缺失记录和Flash失败回滚测试通过。
- 新增原固件S1请求ACK低位1/3/5/7、跳频不打断RX/ACK、批量join/rekey、bootstrap ACK重发交界测试通过。
- 修订号4新增连接矩阵通过：无H=0缓存的持久H=2恢复、最终key延后提交、本地无主机自动保存、断电/快速重启、四槽满及最后join重发、所有在线槽03广播、历史RX/watchdog顺序、错key/id/地址/挑战拒绝，以及20条绑定满库/已有id更新。
- 修订号5新增活动无线回复与本地B4/A1/F0/F7/F8/F1缓存隔离、占位GET请求去重、同命令不同参数、B2关闭后回显、离线B3状态，以及F9 magic/import/过期版本/一次性请求覆盖。
- 生产USB桥覆盖F9确认、短GET、GET SETUP后中止、EP0状态完成后延期消费。构建另核实状态完成钩子为强符号，且核心UF2入口实际链接。
- 修订号6覆盖传统HID属性→字符串→设置事务、50次待就绪SET去重、就绪SET保留回复、本地未发布GET、GET完成→publish→同命令SET的消费竞态、零/短/失败OUT及FA无参数正文诊断。
- 修订号7的触觉回归先在旧生产代码上失败，修复后通过：两个板选择值的重复脉冲、无应用回复/可选GET、已有写回复缓存、重复64字节设置分片、12片FIFO及第13次拒绝/排空；读取查询去重仍通过。说明见 [HAPTIC_FIX.md](HAPTIC_FIX.md)。
- 修订号8新增94族GET回退、本地分派、AD/B1 no-op、完整命令容量探测、四槽输入reset、bootstrap AES失败/重新接入回归；其中96拒绝测试的错误预期已在修订号9纠正。
- 新增USB启动GET、Output无Feature副作用、SET/GET环记录预留、DATA/ACK中止、RF背压下27+27+10完整交付、尽力写满丢弃、读取忙去重及断开排空。复位清理前新SET测试先失败，加入复位门控后通过。
- RX保留模型复现旧溢出ACK导致下行卡住，验证31项容量、环回卷、重复/旧快照、三个连续下行及ACK admission条件；不执行MMIO。键鼠strict mock覆盖单in-flight公平性、B1积压清理、标准Boot/Report和生命周期集成。详见 [COMMUNICATION_AUDIT.md](COMMUNICATION_AUDIT.md)。
- 修订号9的96熵设置回归先在修订号8代码上失败，修复后通过：四离线槽SET/GET、40轮四接口熵设置与80次左右触觉/连续加密输入交错、满RF FIFO、本地无Flash/换钥/挑战变化、短/失败OUT，以及FA五类拒绝原因。详见 [ENTROPY_FIX.md](ENTROPY_FIX.md)。
- 原生USB shim驱动生产wrapper，未运行真实TinyUSB控制引擎/FreeRTOS。nRF5x DCD在安排status时合成完成事件，未验证所有物理中止边界。
- 应用使用 **85,312字节Flash，27,328字节全局RAM**。
- 编译后的RADIO/TIMER3向量确实绑定到强符号handler；两份HID report descriptor与原接收器字节一致。
- UF2起点 `0x27000`，当前末端 `0x3be00`；334块，171,008字节。绑定页、SoftDevice、bootloader及UICR不在UF2内。
- 当前UF2 SHA-256：`54dc773daaad1ac622d5429a15a4578782f78f4b3cde6f1c1eca6067ac4fafad`，固件修订号9、F0诊断协议v3。
- ARM二进制审计确认TinyUSB类表实际指向SET就绪wrapper，控制传输wrapper与UF2入口已链接；生命周期hook及BOS/vendor包装为强符号，vendor class/BOS能力数据已移除。
- **28个Node网页协议/状态测试通过**：包括无线读取SET忙/未发布GET有界重试、写错误不自动重试、权限错误不吞掉、FA旧版/修订号9拒绝元数据及F0/F8无B4常规刷新。
- **12个真实Edge headless浏览器流程通过**：当前覆盖修订号9的FA主机计数、最近触觉/拒绝命令元数据展示及JSON导出、空闲槽本地诊断和无B4重复通知，保留既有UF2和迁移场景；HID peer均为模拟。

可机读结果：`build/tests/manifest.json`、`build/tests/protocol_test.log`、`build/xiao/manifest.json`、`build/xiao/compile.log`。

编译器`.su`也保留在`build/cache/sketch`：RADIO处理体56字节stack、跳频处理体32字节；Flash load/save和setup路径有较大的局部对象，未将数据库或密码学工作移到ISR。`.su`是静态单函数用量，不是实测整条调用链的水位。

## 未执行

用户已反馈首次配对、恢复绑定及修订号6的Steam识别/输入成功。最新修订号8现场日志出现熵设置约1秒超时、70661次无效SET、292次协议队列满、触觉任务积压与反复断连；已定位并修复96误拒绝回归，但没有USB抓包将全部拒绝及断线逐一归因。本代理没有刷写XIAO、打开物理HID接口或采集RF/逻辑分析仪轨迹；浏览器测试注入模拟HID peer。因此尚不能宣称：

- 修订号9的实际持续触觉/连接稳定性、USB复位/恢复效果及网页UF2入口的实际磁盘枚举已由本代理验证。
- 130µs ACK参数、所有切频/恢复边界已实测匹配。
- SDL输入兼容性、四手柄并发、触觉/LED及完整掉电矩阵均已验证。

主机fixture使用合成材料；测试日志不应当作设备抓包证据。物理调试入口是`receiverctl.py status/capture`及其CRC、MIC、ACK逾期和队列计数。
