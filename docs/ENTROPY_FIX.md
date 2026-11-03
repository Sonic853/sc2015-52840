# 修订号9：修复0x96熵设置被拒绝的回归

后续修订号9实物报告已显示`invalid_sets=0`且熵设置超时循环消失，但当时仍反复无线重连。当前交付修订号11包含本页修复、[ACK相位/会话隔离修正](RECONNECT_LOOP_FIX.md)及 [队列诊断](QUEUE_IMPROVEMENTS.md)；本页保留修订号9历史依据。

修订号8将`0x96`与旧bootloader命令`0x90`一起拒绝，属于本工程引入的兼容回归。沿原版调用链继续追踪后确认，`0x96`操作的是原接收器的PRNG种子/计数状态，而不是升级入口。修订号9恢复本地成功处理。

## 现场证据

用户反馈修订号8连接后触觉再次失灵，Steam反复显示手柄断连。本次材料：

- `sc2015-diagnostics (5).json`：UTC `2026-11-02T22:09:07.854Z`，对应本地11月3日06:09；固件修订号8。
- `controller.txt`：06:05:31起枚举四个接口，`CSetPrngEntropyWorkItem`反复运行约1000–1015ms，伴随`Steam controller device closed after HID IO failure`、接口重开和zombie移除。
- `controller_ui.txt`：06:06:03–06:07:02反复断开/连接，但仍能读取Type2、PID4358和主控固件`0x5B0F21BD`。
- 大量`CPulseHapticWorkItem`等待约1–3秒后集中执行；这是主机任务积压的直接日志证据，不是每个脉冲的空口送达证明。

诊断计数：`invalid_sets=70661`、已接受/查询SET计数1049、协议`queue_full=292`；`radio_queue_full=0`、`late_ack=0`、`radio_faults=0`、`usb_command_drops=0`。8681次认证、247次MIC失败、2574次重复；ACK安排/READY/完成均8984。已保存一条非bootstrap绑定，恢复接受8次，恢复MIC失败0，无新配对或写绑定。

这些证据优先指向主机请求被持续拒绝及随后任务堆积。突发触觉写入可使协议发送FIFO满，但292次队列满不能单独证明无线持续卡死；RX环溢出修复不是这份快照中的触发点。快照采集时connectedMask=0，网页本次inputCount=0，也不能倒推整个测试期间没有输入。activity跨多次UF2和网页重连，不能把全部历史事件算作当前会话断线。

FA旧格式只保留最近被接受的SET；此处最近`0x81`是清数字映射，不是被拒绝的熵命令。**没有USB抓包，因此不能把70661次拒绝逐一归因给0x96，或宣称所有无线断开均已解释。** 0x96错误拒绝本身已有原版静态证据和失败回归确认，与本次熵设置超时高度吻合。

## 原版调用链与勘误

原版`d0ggle.bin`为19118字节，SHA-256：

```text
93112caa267e5d79cd8bc26acfd6cda5a1a6b5a682ab7594b262dde510edba21
```

使用工作区既有正确8051架构的独立Ghidra导出，非REA Evidence。地址为BIN内CODE地址：

1. `dispatch.tsv`的`18aa`将`96`分派到`18e1`。
2. `18e1..18eb`传递Feature参数地址`buffer+2`到`4913`，随后调用`400f`并经`1ad5`返回成功R7=5。
3. `491b..4949`检查CODE `0c31..0c40`的16字节种子区域。已初始化时，`492a..4940`复制参数前5字节到EXTMEM `85df..85e3`；全FF时，`494b..4964`经`27d7`写16字节到`0c31`。
4. `498e..49ac`递增`85df..85e3`计数；`49af..4a6b`读取上述16字节作为AES key，并将芯片身份、计数等组织成AES输入以生成输出。
5. `15bb..15c3`请求该生成函数输出8字节，供发现/挑战路径使用。

上一轮只看到第3步中的Flash写路径便把96归为“不支持旧硬件操作”，没有继续追踪第4、5步的用途。这一判断不成立。`90→18f0`才是另一个受magic约束的旧bootloader入口，仍不在XIAO上仿真。

依据：`analysis/receiver_d0ggle/dispatch.tsv`、`feature_functions.md`、`hop_crypto_functions.md`、`assembly.tsv`和`data_windows.json`。旧 [COMMUNICATION_AUDIT.md](COMMUNICATION_AUDIT.md)已添加勘误。

## 修订号9行为

- 合法`96,len,args`作为接收器本地命令完成，在线/离线四槽均可，不等待手柄回复或RF容量。
- XIAO的`radio::random()`使用nRF52840硬件RNG，不依赖主机播种；该命令作为可选熵提示接受，不模拟旧芯片的Flash播种过程，也不重新生成活动会话的挑战或密钥。
- 不把96转发到手柄；回复为`96 00`加零填充，不回显种子正文。
- 保持真实长度/传输结果验证；声明16字节但实际只有15字节参数、失败OUT等仍拒绝。90仍拒绝，F9升级规则保留。
- 本轮不因这份日志增加FIFO容量或修改ACK/CCM/跳频参数；先消除主机重试循环，持续RF异常需结合下一轮结果判断。

## 最近拒绝诊断

FA仍为版本1，原有字段位置不变；修订号>=9利用保留字节提供可选元数据，旧网页可继续解析既有计数：

|偏移|新增内容|
|---|---|
|10|最近拒绝的命令号；数据不可用时为0|
|11|该帧声明的参数长度；数据不可用时为0|
|62|高2位是slot0..3，低6位是拒绝原因|

原因0无记录；1帧/声明长度无效；2不支持的90旧bootloader命令；3本地参数无效；4OUT传输失败或缓冲无效；5SETUP请求长度不在2..64。

失败传输不读取可能陈旧的数据，只记录slot及原因。合法后续SET不清除拒绝历史。`invalid_sets`从修订号9起也包含SETUP长度拒绝；查询待就绪、环容量不足和复位窗口仍使用各自原有处理，不冒充格式错误。

网页JSON增加`hostLink.lastRejected`，CLI增加`host_link.last_rejected`。旧固件或无记录时为null。只保存header元数据，不保存熵、绑定key或Feature参数正文；具体布局也见 [STEAM_COMPAT.md](STEAM_COMPAT.md)。

## 软件验证

新回归先在修订号8生产代码上失败于四接口熵设置的第一条SET；修复后通过：

- 四个离线槽的96 SET/GET，成功不回显种子。
- 40轮四接口熵设置，与在线手柄的连续加密输入、左右80次触觉写入交错；无额外RF命令、Flash写、重新换钥或挑战变化。
- 12项RF FIFO已满时96仍可本地完成，原触觉FIFO按序排空。
- 完整包之后的短OUT/失败OUT仍拒绝；18字节精确长度成功。
- 原生USB wrapper覆盖五类拒绝元数据；网页兼容旧FA、解析新字段、展示并导出诊断JSON。

修订号9当时主suite111160条断言，独立兼容10741条、RX保留1512条；ASan、4个Python镜像测试、28个Node、12个Edge模拟HID流程及ARM构建通过。该历史UF2 SHA-256：

```text
54dc773daaad1ac622d5429a15a4578782f78f4b3cde6f1c1eca6067ac4fafad
```

同名固件路径`build/xiao/SCReceiver-xiao-s140-7.3.0.uf2`现已更新为修订号10，当前hash与资源见 [VERIFICATION.md](VERIFICATION.md)。以上软件验证不是实物稳定性结论。

## 复测

1. 保留现有绑定升级，刷新网页并确认当前修订号11，然后点击“断开网页”。
2. 完全退出并重启Steam，普通Steam键开机；连续操作左右触控板至少1分钟，同时观察连接状态。
3. 检查新日志中约1秒的`CSetPrngEntropyWorkItem`/HID关闭循环是否消失，以及触觉是否仍随运行时间失效。
4. 若仍失败，保留接收器供电、记下本地时间，保存新Steam日志；退出Steam后导出新网页诊断，重点看`invalid_sets`增量和`lastRejected`。不必先清库或重新配对。
