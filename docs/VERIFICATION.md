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
- 本轮使用既有锁定依赖；Windows主suite **113,659条断言**，独立协议兼容suite **15,840条**、无线策略suite **247,618条**、队列指标suite **11,184条**、USB队列/回放及Xbox/PS5集成suite **969,375条**、输入模式suite **835条**、PS5 suite **116条**，另有USB/桌面suite通过结果；108组独立PyCryptodome CCM向量、128组冻结跳频向量，以及19个Python测试（4个镜像、9个CLI协议、4个Windows/Guide探针、2个描述符审计）。
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
- 修订号12覆盖左背+Steam严格同时保持、连接窗口/回卷/持键不重复、认证输入观察与USB合成释放隔离、左右坐标复用、左板左/摇杆右同时操作、八方向/按压条件、点击别名、失联中性、配置逐字节破坏/截断、旧token及失败保存、FD短/中止/失效版本GET、Type-C返回Steam、普通USB reset保留Xbox、两种HID接口号及XInput发送生命周期。初始化整批入队不覆盖主机回复的回归也通过。见 [XINPUT_MODE.md](XINPUT_MODE.md)。
- 原生USB shim驱动生产wrapper，未运行真实TinyUSB控制引擎/FreeRTOS。nRF5x DCD在安排status时合成完成事件，未验证所有物理中止边界。
- 修订号13补充nRF5x主动detach会提交UNPLUGGED的行为，先在旧代码复现切换Xbox后仍为Steam，再以事件时VBUS判定修复。持续loop、两次软件detach、真实拔线/快速重插、切换中拔线及不同callback上下文回归通过。详见 [XINPUT_SWITCH_FIX.md](XINPUT_SWITCH_FIX.md)。
- 修订号14先复现XUSB第5个IN长度错误，再修复OUT字段错位；新增Get_Report短/长读、非法请求、EP0快照与IN缓冲隔离、OUT初次安排失败、完成/失败计数及FD诊断回归。Windows只读探针在无目标设备时成功读到空PnP结果和四个XInput槽1167，未验证目标驱动。见 [XINPUT_ENUMERATION_FIX.md](XINPUT_ENUMERATION_FIX.md)。
- 修订号15覆盖OUT初次/后续失败重试、claim释放、挂起/reset及TX优先级恢复；严格Xbox OUT解码、来源/USB epoch隔离、有限8F波形、20Hz合并、2.5秒超时、取消保留CCM lane和普通host脉冲；双向开机组合、三键优先级和连续持键计时。完整模拟链路从认证RF输入选择owner、接收Xbox OUT、解密核对8F，到A＋Steam新会话切回Steam通过。见 [XINPUT_RUMBLE_AND_RECOVERY.md](XINPUT_RUMBLE_AND_RECOVERY.md)。
- 修订号16覆盖保存先于detach、至少60ms后一次reset、Flash失败不detach、短/中止GET不保存、一次性消费/记住模式/v1记录升级和重启后RF恢复模型；四vendor接口不含HID、附件端点不打开；Guide经认证RF到USB IN的0400按下/清零；斜向0～75°全范围编码、四象限边界两侧及旧45°兼容。见 [MODE_REBOOT_AND_GUIDE.md](MODE_REBOOT_AND_GUIDE.md)。
- 修订号17覆盖右背＋Steam三模式选择、A恢复优先/双背键歧义拒绝、PS5启动记录及旧版网页门控；DualSense面键/轴/Y方向/方向帽/PS键、Feature长度和MAC稳定、48/63/64字节输出、震动标志与停止、短/中止/失败EP0及会话隔离。真实生产CCM模拟链路完成右背切换、PS键输入、双通道8F及返回Steam。见 [PS5_MODE.md](PS5_MODE.md)。
- 应用使用 **109,620字节Flash，31,276字节全局RAM**。
- 编译后的RADIO/TIMER3向量确实绑定到强符号handler；两份HID report descriptor与原接收器字节一致。
- UF2起点 `0x27000`，当前末端（不含）`0x41d00`；429块，219,648字节。配置/启动选择、绑定、SoftDevice、bootloader及UICR不在UF2内，应用上限仍为`0xe9000`。
- 当前UF2 SHA-256：`f63e737b3b483099becab934a108445505b1d375ee771de71e8fe1676c0c78cd`，固件修订号17、F0诊断协议v3、FC及FD–FE协议v1（配置仅Steam模式可用，模式2新增PS5）。默认左板按压触发、斜向30°、不记住模式；旧映射升级保留。
- ARM二进制审计确认TinyUSB类表实际指向SET就绪wrapper，控制传输wrapper与UF2入口已链接；生命周期hook及BOS/vendor包装为强符号，vendor class/BOS能力数据已移除。
- XInput类驱动及字符串wrapper为强符号；编译后的144字节Xbox配置体按字段核对IF0报告/端点和四个vendor接口边界，确保无HID。40字节XUSB10兼容描述符与18字节OS字符串也通过审计；完整安全字符串的178字节头在native验证，多包物理传输仍待实测。
- 编译后的PS5 HID描述符独立解析主条目与报告ID位数：Input1=63字节、Output2=47字节、Feature3/5/9/20分别47/40/19/63字节payload；整体报告ID前缀另计。
- **57个Node网页协议/状态测试通过**：新增PS5切换/记住模式/共用配置及旧固件不发送模式2写入；保留既有协议回归。
- **17个真实Edge headless浏览器流程通过**：新增PS5选项及返回Steam配置流程；保留旧版功能和窄屏布局，HID peer均为模拟。

可机读结果：`build/tests/manifest.json`、`build/tests/protocol_test.log`、`build/xiao/manifest.json`、`build/xiao/compile.log`。

编译器`.su`保留在`build/cache/sketch`，当前函数栈用量以这些文件为准。Flash/数据库路径仍有较大局部对象，未将其移到ISR。`.su`是静态单函数用量，不是实测整条调用链的水位。

## 后续本机实物USB对照

2026-11-03已在本机打开指定原版/XIAO HID接口，读取A1及XIAO诊断，并用USBPcap分别记录用户操作：

- 原版24936条USB记录/138.691046秒，1822次8F触觉SET全部USB成功。
- XIAO声明修订号10，24502条USB记录/158.231922秒，2106次8F触觉SET全部USB成功；连接恢复后约89秒未见state1。
- 用户报告两轮连接和触觉正常；原版早期断开为用户关开手柄。XIAO的一次断开位于点击Steam“完成配对”后主机发送`9f04off!`的流程中，FB看门狗计数1，其余断开原因0。
- XIAO采集后仍在线，`invalid_sets=0`、USB丢弃0；协议队列满267，不能据USB成功或主观正常推断每条RF写入已送达。
- 原始文件SHA-256、frame引用、前后诊断和采集边界见 [LIVE_USB_COMPARISON.md](LIVE_USB_COMPARISON.md)。构建脚本本身不执行硬件测试，manifest中的`hardware_tested=false`与此独立运行观察分开保存。

## 尚未验证的范围

**修订号17尚未完成实物刷写或DualSense枚举验证。** 本轮PnP查询没有`054c:0ce6`、`045e:028e`或目标XIAO设备。软件报告/回调与模拟重启通过，不能证明真实Windows/Steam/SDL识别、右背开机、PS键、震动或主机兼容。PS5模式按PC DualSense实现，未实现主机认证、动态IMU/触摸、自适应扳机及音频。

修订号11经用户授权已在本机通过F9进入UF2并刷写；75秒时钟/FC观察、20条低频脉冲和32条突发已完成。后者14条入队并退休、18条容量超时，FIFO最大年龄418.124ms；三个窗口断开增量0，MIC失败仍有增长。镜像/原始诊断身份和限制见 [REV11_LIVE_VALIDATION.md](REV11_LIVE_VALIDATION.md)。

尚未采集RF/逻辑分析仪轨迹。上述本机短时观察不能证明：

- 所有环境下长期持续触觉/连接稳定性、全部USB复位/恢复效果及网页UF2入口的实际磁盘枚举。
- 130µs ACK参数、所有切频/恢复边界已实测匹配。
- SDL输入兼容性、四手柄并发、触觉/LED及完整掉电矩阵均已验证。

原生/浏览器测试的RF peer仍使用合成材料；新增触觉fixture的到达时刻来自USB捕获，其软件回放日志不应当作RF抓包。USBPcap及修订号11的HID诊断是另行保存的实物观察；USB时间戳不是RF ACK时间戳。
