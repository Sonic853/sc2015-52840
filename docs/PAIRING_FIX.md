# 修订号3：能收CRC正确包但无法配对

本页记录修订号3的历史ACK修复。当前下载文件已更新为修订号4；最新重连修复、SHA及连接矩阵见 [CONNECTION_FIX.md](CONNECTION_FIX.md)。

## 实物报告与判断

用户提供的诊断：F0 v2、pairing=true、activePipes=3；CRC正确2076、错误9、应用rx2076；authenticated=0、mic_failures=0，队列/Flash/ACK逾期/射频故障均0；四槽未连接。

这证明RX物理层已经接收CRC正确的包，尚无通过认证的接入；没有MIC失败并不证明密钥正确，而可能根本没进入认证分支。旧报告没有分pipe、S1或真实ACK完成计数，不能从它断言2076包全是pipe0，也不能将late_ack=0当作发出过ACK。

活动记录还显示只迁移了原接收器身份和0条绑定：这没有携带旧绑定key，因此不能据此恢复原绑定；仍可以执行新配对。

## 确定的错误：S1低位判断相反

上一版：

```cpp
if (valid && !(rx.s1 & 1)) { /* send ACK */ }
```

在该nRF5 ESB DMA布局中，S1为 `(PID << 1) | ACK_REQUESTED`：**低位1表示要ACK，低位0表示no-ACK**。上一版会抑制正常发现/配对包的应答。

一手静态证据是原手柄production `d0g_module.bin`：`P:32fe..3316` 对普通ACK事务写入S1低位1，并由 `P:3322..333c` 选择带等待ACK的发送路径；`P:3bfc -> 3da0(...,0)` 的常规排队路径也与之对应。这些地址是基址0x400后的地址，不是文件偏移。

辅助源码参考：Nordic nRF5 SDK的 `nrf_esb.c`，`start_tx_transaction()` 写 `pid<<1 | (noack ? 0 : 1)`；`on_radio_disabled_rx()` 在S1低位1时应答；`rx_fifo_push_rfbuf()` 用其反值生成公开noack字段。参考镜像：

<https://raw.githubusercontent.com/greenlsi/nrf5-sdk/master/components/proprietary_rf/esb/nrf_esb.c>

本实现没有拷入SDK驱动，已将实际使用的判定收敛到 `radio_policy.h::wantsAck()`，并测试PID0..3对应的S1 **1/3/5/7都应答，0/2/4/6不应答**。CRC失败时不应答。

这是确定的实现错误，足以阻断正常手柄握手，且与实物报告吻合；新固件是否已完全解决这只设备的问题仍需刷入复测。

## 同时修复的配对相关交界

1. **跳频与ACK竞态**：旧跳频ISR先检查sending_ack、后屏蔽RADIO IRQ，中间可能被RX END抢占并启动ACK；返回后却继续禁用无线并换频。现在先屏蔽RADIO IRQ再检查，不打断已识别地址、待处理END或进行中的ACK。
2. **实际信道记录**：延后切频时，期望hop索引不等于实际无线信道。现在单独保存current_channel，诊断及接收元数据使用实际值；每次RX清EVENTS_ADDRESS，避免旧事件误阻塞跳频。
3. **批量RX下的bootstrap→newkey交界**：手柄state6必须先认证一包bootstrap下行才切换新key（原 `P:2bb6..2bd4`）。主循环可能在首次service前连续消费join和key-update，从而清掉尚未发送的旧key下行。现在暂存旧上下文counter1的控制04 ACK，并接受已认证过的同一key-update密文重发而不重复装钥；直到收到通过新key认证的上行才停止旧ACK。

没有改变39位counter候选、方向/nonce、MIC4、固定发现H=04等已恢复字段。批量RX回归测试特意不在join和key-update之间调用service，覆盖原合成peer没有覆盖的顺序。

## 刷入及复测

1. 修订号3使用 `build/xiao/SCReceiver-xiao-s140-7.3.0.uf2`，该次构建SHA-256为 `ea44e445ba6e362ef56f8912c52c796ba4f6aa83954c39c0394a9285662cfa6c`；当前同名文件的最新SHA见CONNECTION_FIX。
2. 更新网页静态文件，或使用当前命令行工具。F0版本应为3；网页“配对链路细节”应显示**固件修订号3**。
3. 只保留待测试XIAO，拔下使用相同RF身份的原接收器，关闭手柄后重新按住X+Steam启动配对。
4. 开启网页配对，观察约10–20秒，再导出诊断JSON。此次无需重新迁移身份或清空绑定。
5. 修复后的关键链路是：pipe0收到发现 → ACK scheduled/ready/completed增加 → pipe1接入 → authenticated增加 → 业务pipe2..5出现 → 待保存/已连接。

ACK完成仅指本地发射完成，不证明手柄收到了它；仍无进展时保留F7中的分pipe、最近S1/H、ACK长度和处理延迟继续分析。若已到pipe1但MIC失败增加，下一步应分析接入挑战/nonce，而不是继续改RX PHY。

## F7报告

Feature请求 `f7 00` 后补零；report ID参数0。响应64字节：

|偏移|内容|
|---|---|
|0..3|F7、payload长62、F7版本1、固件修订号3|
|4..10|最近有效RX的pipe/channel/length/S1/H、最近ACK长度/TX pipe|
|11|HFCLKSTAT低字节|
|12..35|pipe0..5六个有效收包uint32，LE|
|36..55|ACK安排、TX READY、完成、不需ACK而跳过、零载荷ACK五个uint32，LE|
|56..59|最大RX END到IRQ处理延迟、最近RX END到TX READY处理延迟，各uint16 LE|
|60..63|当前RADIO state、实际channel、PPI CHEN低字节、修订号3|

F0 v3与v2布局相同，版本3声明F7可用。网页诊断JSON增加`pairingLink`；命令行status增加`pairing_link`。它们不包含密钥。

## 验证边界

74339条原生断言、ASan、17个Node测试、8个Edge模拟HID流程及ARM构建通过。没有本轮刷写或实测修复后的真手柄；不能把合成测试写作“真机已配对成功”。
