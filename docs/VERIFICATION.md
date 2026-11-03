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
- 本轮使用既有锁定依赖；Windows主suite **113,659条断言**，独立协议兼容suite **15,840条**、无线策略suite **247,618条**、队列指标suite **10,860条**、USB队列/回放suite **964,313条**，另有USB/桌面suite通过结果；108组独立PyCryptodome CCM向量、128组冻结跳频向量，以及10个Python测试（4个镜像、6个CLI快照/身份测试）。
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
- 修订号10复现“已保留但未认证RX先推进下行→旧lane滞留→重复上行→看门狗断开”，相位门控修复后3000hop保持推进；4000轮丢ACK/旧上行/延后发布与70条触觉负载通过。另覆盖旧B2/B3/设置跨会话、DATA→ACK期间换会话、五类实际断开原因与FB断开前快照。见 [RECONNECT_LOOP_FIX.md](RECONNECT_LOOP_FIX.md)。
- 修订号11覆盖分类失败/内部重试、整命令拒绝、FIFO/lane年龄和清理、事件环覆盖、8ms前释放/到期仍满、时钟回卷/重配连续性、会话/reset取消守恒、FC页0..17冻结及失效/畸形参数。278/371条实测USB到达序列在两种假设RF轮询周期下回放；16条瞬时负载拒绝4→2，持续高负载仅小幅改善并增加部分尾延迟。见 [QUEUE_IMPROVEMENTS.md](QUEUE_IMPROVEMENTS.md)。
- 原生USB shim驱动生产wrapper，未运行真实TinyUSB控制引擎/FreeRTOS。nRF5x DCD在安排status时合成完成事件，未验证所有物理中止边界。
- 应用使用 **95,192字节Flash，30,124字节全局RAM**。
- 编译后的RADIO/TIMER3向量确实绑定到强符号handler；两份HID report descriptor与原接收器字节一致。
- UF2起点 `0x27000`，当前末端（不含）`0x3e400`；372块，190,464字节。绑定页、SoftDevice、bootloader及UICR不在UF2内。
- 当前UF2 SHA-256：`ae549394f1d88f73fc2bb0b11f552e27af5f5e25b1453463d91605855ef22000`，固件修订号11、F0诊断协议v3、FC协议v1。
- ARM二进制审计确认TinyUSB类表实际指向SET就绪wrapper，控制传输wrapper与UF2入口已链接；生命周期hook及BOS/vendor包装为强符号，vendor class/BOS能力数据已移除。
- **45个Node网页协议/状态测试通过**：包括原有查询、FA/FB，以及FC版本门控、完整页组/空环/满环、跨页ID/time拒绝、过期整组重试、健康空闲接口选择及失败时保留历史快照。
- **13个真实Edge headless浏览器流程通过**：新增修订号11的摘要/完整队列面板、手动快照和JSON导出；保留既有FA/FB、UF2、迁移和查询恢复场景；HID peer均为模拟。

可机读结果：`build/tests/manifest.json`、`build/tests/protocol_test.log`、`build/xiao/manifest.json`、`build/xiao/compile.log`。

编译器`.su`也保留在`build/cache/sketch`：RADIO处理体64字节stack、跳频处理体32字节、`usb::process`232、loop80、setup552；队列初始化移除大型临时对象后 `Receiver::begin` 为40字节。Flash/数据库路径仍有较大局部对象，未将其移到ISR。`.su`是静态单函数用量，不是实测整条调用链的水位。

## 后续本机实物USB对照

2026-11-03已在本机打开指定原版/XIAO HID接口，读取A1及XIAO诊断，并用USBPcap分别记录用户操作：

- 原版24936条USB记录/138.691046秒，1822次8F触觉SET全部USB成功。
- XIAO声明修订号10，24502条USB记录/158.231922秒，2106次8F触觉SET全部USB成功；连接恢复后约89秒未见state1。
- 用户报告两轮连接和触觉正常；原版早期断开为用户关开手柄。XIAO的一次断开位于点击Steam“完成配对”后主机发送`9f04off!`的流程中，FB看门狗计数1，其余断开原因0。
- XIAO采集后仍在线，`invalid_sets=0`、USB丢弃0；协议队列满267，不能据USB成功或主观正常推断每条RF写入已送达。
- 原始文件SHA-256、frame引用、前后诊断和采集边界见 [LIVE_USB_COMPARISON.md](LIVE_USB_COMPARISON.md)。构建脚本本身不执行硬件测试，manifest中的`hardware_tested=false`与此独立运行观察分开保存。

## 尚未验证的范围

修订号11经用户授权已在本机通过F9进入UF2并刷写；75秒时钟/FC观察、20条低频脉冲和32条突发已完成。后者14条入队并退休、18条容量超时，FIFO最大年龄418.124ms；三个窗口断开增量0，MIC失败仍有增长。镜像/原始诊断身份和限制见 [REV11_LIVE_VALIDATION.md](REV11_LIVE_VALIDATION.md)。

尚未采集RF/逻辑分析仪轨迹。上述本机短时观察不能证明：

- 所有环境下长期持续触觉/连接稳定性、全部USB复位/恢复效果及网页UF2入口的实际磁盘枚举。
- 130µs ACK参数、所有切频/恢复边界已实测匹配。
- SDL输入兼容性、四手柄并发、触觉/LED及完整掉电矩阵均已验证。

原生/浏览器测试的RF peer仍使用合成材料；新增触觉fixture的到达时刻来自USB捕获，其软件回放日志不应当作RF抓包。USBPcap及修订号11的HID诊断是另行保存的实物观察；USB时间戳不是RF ACK时间戳。
