# 修订号11本机升级与队列验证

2026-11-03经用户明确授权，将连接中的XIAO从修订号10升级到11，并执行HID诊断、75秒计时观察、低频触觉和短突发验证。**快照协议、计数守恒和有限等待按预期工作；突发仍有18条容量拒绝，并测得418.124ms最大明文FIFO年龄。**

## 1. 升级与身份

- 预检A1确认 `SC2015 XIAO`、RF地址 `f0d8bac40b`，F7为10；此前设备离线、已保存绑定1条。此时Steam仍运行，后续操作前已确认退出。
- F9只发送一次 `UF2!`，取得 `f9 02 00 01`；随后出现 `F:`，卷标 `XIAO-SENSE`。
- `INFO_UF2.TXT`：UF2 bootloader0.6.1，Model Seeed XIAO nRF52840，Board-ID `Seeed_XIAO_nRF52840_Sense`，S140 **7.3.0**。
- 复制的UF2：372块、190464字节，地址范围 `[0x27000,0x3e400)`，SHA-256：

```text
ae549394f1d88f73fc2bb0b11f552e27af5f5e25b1453463d91605855ef22000
```

- UF2盘移除，四个vendor HID重新枚举，F7/FC均声明11，RF身份保持。绑定数仍1，手柄普通开机恢复到槽0（网页槽1、USB IF1），测试后保持在线。
- 这是已验证镜像复制及运行声明，不是读取设备完整应用Flash后的字节比对。未读出绑定密钥。

## 2. 实测结果

75秒观察使用空闲IF2；触觉测试选实际在线IF1。Steam和网页控制台保持关闭，无并发Feature操作。触觉为捕获格式 `8f 08`，参数使用100µs/单次的小脉冲并交替板选择值；不发送设置、配对或提交命令。

|场景|实际结果|
|---|---|
|75秒观察|16次页0采样及前后完整快照；全部解析、ID/time一致性与四槽守恒检查通过|
|计时跨度|host 75,000,477µs；MCU 75,000,956µs；逐5秒采样的最大host/MCU差544µs，包含USB/host调度误差|
|跨67秒|MCU由27,545,141推进至102,546,097µs，未出现DWT除64式的67秒回零|
|20条低频触觉|每条间隔约100ms；USB收到20、RF入队20、序列化20、相位退休20；full/timeout/stale/offline/reset取消均0|
|32条短突发|32次成功SET共10.9217ms；RF入队14、序列化14、相位退休14；容量拒绝18、USB等待超时18，二者是同一组命令|
|拥塞事件|累计等待18，最后16项事件成功读取；可见Wait/Timeout/HapticFull，部分Timeout年龄8016、8503、8272µs|
|连接与驱动|三个测试窗口的五类断开增量均0；结束时RADIO RX环满、late_ack、radio_faults、USB command_drops均0|

20条低频与32条突发不能混为同一负载。后者约10.9ms的人工压力不是先前Steam稳定段的典型到达率；18/32也不是Steam的实测丢弃率。

突发结束的FC累计上限：

|指标|值|
|---|---:|
|明文FIFO最大年龄|418124µs|
|lane最大年龄|86152µs|
|USB SET最大已检查年龄|8629µs|
|RX最大排队延迟|894µs|
|USB process最大调用间隔|1888µs|
|RF / USB高水位|12 / 18|

这些是上电累计最大值。FIFO最大值在本次突发前为108µs，因此418.124ms来自此次新负载；它不是测得的触觉执行时间。FIFO与lane最大值也不能相加当作同一条命令的端到端延迟。

F7设备计数报告最大IRQ延迟12µs、最后一次TX READY延迟132µs；未使用RF/逻辑分析仪验证微秒空口时序。

### 仍存在的观察

MIC失败仍增长：75秒窗口+238、低频脉冲窗口+7、突发窗口+1；同期认证继续推进、没有断开。当前证据未归因这些失败，不能把本次通过写成“无线错误清零”。也没有本轮同条件修订号10对照，因此不能把实物结果当作已证明的吞吐提升。

相位退休只证明接收器端认证反馈推进，手柄触觉强度和主观感受需用户确认。此次没有重新采集USBPcap或空口报文。

## 3. 可复查记录

工作区 `analysis/receiver_d0ggle/`：

|文件|SHA-256|
|---|---|
|`live_xiao_rev11_preflight_20261103_01.json`|`1a169552d502b747a205b958eec1f39738c679fe0f483676bcd8990144e77a47`|
|`live_xiao_rev11_bootloader_20261103.json`|`59eac0e047bcf0488b7eeb07ac7b71579ff40e0e6a536bdc8262dad7817df4ce`|
|`live_xiao_rev11_observe_20261103.json`|`40fc8e2673913c256a030a3611f67c8a9fbe64e707a9b22886e5dfa54f0a3e6e`|
|`live_xiao_rev11_pulses_20261103.json`|`6e2fb28e8a30bdf6b9ca658223f79a17768aebb4aaed91ac631cdf50ad35ab95`|
|`live_xiao_rev11_burst_20261103.json`|`0c15607cdbae7ddff01a81c3d74ea3e345a5517e9d78071ba6978fd0dd67fdfa`|

`live_xiao_rev11_summary_20261103.json`保存输入digest、MCU/host时间差、计数增量和driver解析。探针保留在 `scripts/verify_live_rev11.py`，离线汇总器为 `scripts/summarize_live_rev11.py`。时间为UTC；75秒观察为09:30:05.803–09:31:21.016，脉冲为09:31:52.261–09:31:55.481，突发为09:33:34.783–09:33:36.050。

每份完整FC的页内ID/time已校验。F0/F7/FB是前后顺序读数，不属于FC冻结范围。软件构建manifest的 `hardware_tested=false`仍只描述构建工具自身，本文件记录另行执行的实物验证。

实现、线格式与回放取舍见 [QUEUE_IMPROVEMENTS.md](QUEUE_IMPROVEMENTS.md)；早期修订号10的实际Steam场景见 [LIVE_USB_COMPARISON.md](LIVE_USB_COMPARISON.md)。
