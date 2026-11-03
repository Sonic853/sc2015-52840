# 修订号8：原版接收器与XIAO通信差异复核

**修订号9勘误：本页原先把96归为应拒绝的旧硬件操作，这是错误的。** 原版后续PRNG路径证明它是主机熵设置；修订号8的拒绝导致兼容回归。当前已恢复本地完成，现场证据、调用链及修复见 [ENTROPY_FIX.md](ENTROPY_FIX.md)。其余内容保留修订号8的审计范围。

## 范围与证据

本轮从修订号7基线`488cf59`检查USB枚举/控制传输、Feature分派、输入/键鼠、无线ACK与换钥边界，并形成修订号8。固件文件、构建数字和验证命令见 [VERIFICATION.md](VERIFICATION.md)。

原版为工作区`d0ggle.bin`，19118字节，SHA-256：

```text
93112caa267e5d79cd8bc26acfd6cda5a1a6b5a682ab7594b262dde510edba21
```

复用并核对`analysis/receiver_d0ggle/`的`identity.json`、`dispatch.tsv`、`assembly.tsv`及独立Ghidra导出。接收器为8051，文中`D:`/`CODE:`是该BIN内地址；手柄radio的`P:`为production加载地址。它们是**独立静态证据，不是REA Evidence或新采集的运行轨迹**。

用户已确认修订号6的Steam识别和输入；XIAO→原版→XIAO同配置对照只有原版有左右触觉。修订号7已修复重复写入去重，但尚未收到其触觉实测结果。本轮发现的队列、复位等错误有源码及软件回归支持，不能直接认定为上述实物现象的全部原因。

## 1. USB与Feature

|检查项|原版静态行为|修订号8处理|
|---|---|---|
|读取查询|`D:1951..19b0`：在线未就绪SET忙，已有结果后完成|保留修订号6门控；`82/83/84/89–8c/aa/ab`去重；AE/BA仍为读取兼容扩展|
|普通写与尽力写|默认`D:19b1..19cc`检查入队失败；`8f/a2–a6`的`19ce..19d8`不检查转发结果|普通写保留到RF有完整命令容量；尽力写RF满时计数并丢弃，不延迟重放旧脉冲|
|94族GET回退|`D:29b0..29c9`分派，`29cb..29d9`与`29dc..29ea`构造固定回复|无线回复优先；无回复时`94/97 → 94 02 00 00`，`91/92/93/95/98/99 → 94 02 02 00`；SET仍转RF|
|`90`|`D:18f0`为旧接收器bootloader入口|USB拒绝，不发给手柄；XIAO升级入口仍是F9|
|`96`（勘误）|`D:18e1→4913`提供PRNG熵，`49af`使用该种子生成输出|修订号8错误拒绝；修订号9接受为本地熵提示，XIAO使用硬件RNG|
|`9b`|`D:19da..19ff`只有原始`9B TEST`格式进入工厂路径|普通合法Feature为本地no-op；工厂格式不在本工程支持范围|
|AD参数长度|`D:1939..193d`忽略非2参数长度并完成|结构合法但长度不为2时no-op成功|
|B1 selector|`D:3fef`忽略非零selector|长度2且selector非零时no-op成功；无效结构仍拒绝|
|HID Output|`D:2eec → 2f10`为本地GPIO指示处理|Output不执行Feature RPC；XIAO无对应GPIO副作用|
|GET_INPUT|`D:2e6e → 2f1d`返回单字节0|返回一个零字节，不把interrupt-IN快照塞入EP0|
|首次GET_FEATURE|无先前命令/回复可返回零内容|启动回复版本有效，首次发布后可读全零64字节|
|BOS/WebUSB/MS OS|原版USB2.0设备没有这些额外能力|关闭TinyUSB vendor class，并显式包装BOS/vendor控制回调；WebHID仍使用标准HID|

`localFeature()`只分类`90/96/9b/a1/ad/af/b0–b4/f0–fa`。`a2–a6`不是本地绑定读取，`ae/ba`和`fb–ff`也不会被误归为本地命令。A1、B0–B4及F0–FA的既有身份、绑定和诊断格式继续使用。

### 异步队列的接受规则

XIAO将USB、AES/Flash和RF处理分离，因此不是逐指令复制旧8051固件：

1. Feature SET的DATA阶段先预留USB命令环记录；环满立即STALL，不能在成功status后的void回调中才发现无处保存。
2. 完整GET也预留一条版本化消费记录。新的SETUP取消旧EP0预留，短/失败OUT不借用旧缓冲尾部。
3. 普通无线写在USB成功后拥有该记录；仅当RF FIFO能容纳**全部**`ceil((len+2)/27)`片才移入。64字节命令为27+27+10，不会只提交一部分。等待期间同槽GET保持未发布。
4. 主循环每轮继续处理RF，再重试USB队头；无线断开后可以排空该槽等待记录。一个满槽可能暂时阻塞后面的其他槽命令，这是有界FIFO的设计取舍。
5. `8f/a2–a6`沿用原版尽力写语义。RF满时`queue_full`增长，不表示手柄执行成功；USB环满仍返回失败，因为连本地处理记录都无法保留。
6. 总线复位/拔出通知在IRQ只设标志。清理前新Feature请求失败重试，主循环取消旧USB命令、GET消费、查询快照及EP0预留。RESUME保留Feature队列，仅复位输入。

普通写在RF满但USB环尚有空间时可以先成功，这是本工程的缓冲设计，区别于原版直接因RF入队失败而STALL；它保证在连接持续且RF恢复进度时保留一次已接受的操作，不保证设备复位/断电后执行。

`command_drops`字段位置不变，但它也统计队列容量导致的SET/GET拒绝；其增加不能直接解释成“已经成功的命令丢失”。FA不记录发送进程或参数，ACK完成计数也不代表触觉执行。

## 2. 输入与桌面键鼠

- 原版`D:3bb5`输入路径与当前基本状态解包、三张重排表、42字节持久状态补丁、特殊报告透传和全局USB sequence相符，保留这些字段。
- 断开顺序按`D:2af9`修正：`2b08`先调用`2b8d`清输入并发送neutral，`2b11`再调用`2a6e`发送state1。
- 原版USB复位/恢复路径`D:2ce7/2d1e`遍历四槽调用`2b8d`。XIAO在loop中清输入缓存和USB输出积压，发递增sequence的neutral；RF连接、密钥、counter及持久绑定保留。
- B1关闭会清除键鼠待发快照和累计鼠标位移，最多提交一次键盘/鼠标释放。逐条同步B1，避免同一loop批次“关→开”使旧运动漏清。已交给TinyUSB的in-flight包不能撤回。
- IF0只有一个共享IN端点；键盘与鼠标都待发时轮换成功提交，防止键盘持续更新使鼠标饿死。
- IF0遵循标准Boot/Report格式：Boot为无Report ID的8字节键盘、抑制鼠标；Report为ID1键盘、ID2鼠标。切模式清理旧运动，返回Report后保留新到达的相对输入。

有意保留的差异：标准Boot处理而不是原版异常bit1分支；键码去重和有界鼠标累加/分包，而不是照搬旧键码交换链及8位溢出。它们主要影响桌面键鼠边界，不改变IF1–4的64字节vendor输入格式。

## 3. 无线队列与换钥

### RX溢出时不发载荷ACK

原版`D:21ef..2323`在无线处理路径完成认证/分派及相位处理，`2324..2366`记录对端已观察的下行相位。XIAO为满足ACK时限，在ISR选取预先序列化的双lane快照，随后才由loop认证RX。

旧XIAO代码在RX环满时丢弃上行，却仍可发带数据ACK。模拟peer可能已接收并推进下行counter，而loop永远看不到用于释放旧lane的相位见证；后续第三个下行因此卡住。测试用真实`Receiver`/CCM/`ResponseSnapshot`复现了旧策略停滞。

修订号8先将有效包完整提交RX环，再允许请求ACK的包得到回复。32项环实际保留31项；溢出不ACK，让peer重试。`queue_full`记录溢出，`ack_skipped`继续表示有效但未请求ACK的包，不混入溢出。测试覆盖环回卷、旧快照重复、批量认证/发布及三个连续下行恢复进度；不执行nRF MMIO或测量130µs时序。

### bootstrap换钥异常

换钥时生成旧key的bootstrap ACK可能因AES硬件失败而返回。旧路径仍继续提交上行counter，可能将重试误判为重复。现在生成失败立即断开该会话并停止后续counter/phase/watchdog提交。因为peer可能已收到旧ACK并切key，保持半更新会话不能安全恢复；它可重新发现/接入。故障注入回归验证未写绑定、旧会话不能复活、重新接入后正常换钥/保存。

Nordic1M、5字节地址、MSB-first、CRC16、ACK请求低位、CCM nonce/AAD/MIC、39位counter和双lane正常相位公式没有新的反证，本轮保持。nRF52840边缘信道使用可用的−8dBm档位，区别于原版−6dBm。

## 4. 验证与仍待确认的项

|工作项|状态与覆盖|
|---|---|
|USB/Feature差异|上述有证据差异已修正；生产wrapper的启动GET、类型分派、94回退、环容量、DATA/ACK预留、复位竞态回归通过|
|无线差异|RX保留/ACK策略及换钥异常已修正；真实协议与CCM的软件模型通过；PHY时序仍待实物测量|
|输入/键鼠|四槽reset、neutral顺序、Boot/Report、单in-flight公平性及B1关→开回归通过|
|构建|ARM编译通过；真实向量、HID类表wrapper、强生命周期/BOS/vendor包装符号及原版HID描述符字节检查通过|
|现场异常归因|部分完成；没有本轮USB/RF抓包，Steam内部workaround和间歇断线的完整原因未确定|
|旧硬件/工厂功能|90和9B TEST未仿真；96已在修订号9实现本地兼容处理；F9提供XIAO升级入口|
|低优先级缺口|上行控制04的peer信息尚未持久记录；没有证据证明它导致当前触觉问题|

修订号8当时Windows计数为主suite **100405**、协议兼容suite **10741**、RX保留suite **1512**，另外USB/桌面suite独立输出通过结果。ASan、4个Python镜像测试、108个独立CCM向量、128个hop向量、27个Node及12个Edge模拟HID流程通过。该轮96测试错误地要求拒绝，现已更正；当前数字见 [VERIFICATION.md](VERIFICATION.md)。

本轮复位竞态测试先在未加复位门控时失败：清理前的新SET错误成功；修复后通过。桌面mock限制端点只能有一个in-flight传输，避免无限ready的mock掩盖饥饿问题。

这些测试直接编译生产协议/USB桥及无线策略；没有运行真实TinyUSB控制引擎、FreeRTOS调度或8051固件。nRF5x DCD合成status完成事件的既有边界见 [STEAM_COMPAT.md](STEAM_COMPAT.md)。本轮没有刷机或打开物理HID。

## 5. 实物复测顺序

1. 使用当前修订号10，保留现有绑定；网页确认修订号为10。新增FB及ACK相位修正见 [RECONNECT_LOOP_FIX.md](RECONNECT_LOOP_FIX.md)，UF2地址和SHA-256见 [VERIFICATION.md](VERIFICATION.md)。
2. 点击“断开网页”，完全退出并重启Steam，普通按Steam键恢复。检查手柄详情和持续输入。
3. 同一配置下左右触控板各连续滑动约10秒，再交替滑动；记录持续反馈、只响一下或完全无反馈，以及是否伴随断线。
4. 分别测试手柄关开、接收器拔插、电脑睡眠/恢复，检查输入是否恢复及有无卡键/旧鼠标运动。桌面键鼠模式下同时改变键盘和鼠标输入。
5. 异常后记下本地时间并保存`controller.txt`、`controller_ui.txt`；退出Steam、保持接收器供电，再导出含FA/F7/F8的诊断JSON。无需上传绑定备份中的密钥。
