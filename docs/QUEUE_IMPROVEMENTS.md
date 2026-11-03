# 修订号11：队列分账、冻结快照与有界触觉等待

修订号11落实 [队列审计](USB_DIFFERENCES_AND_QUEUE_REVIEW.md) 中的分类计数、时间诊断、小批交错调度和有限等待。软件回放及 [本机实物验证](REV11_LIVE_VALIDATION.md) 已完成。它能吸收部分瞬时突发，**持续拥塞下仍会丢弃，也可能产生明显的已入队等待**。

## 1. 调度与接受规则

`SCReceiver/queue_policy.h` 固定当前参数：每轮先处理最多8个RX，再 `service/publish`，处理最多4个USB记录，再 `service/publish`；每次loop执行4轮。RX仍按记录的hop推进协议；AES、Flash和队列状态修改仍由loop负责。

- 普通无线设置/查询：有足够容量容纳整条命令才入RF FIFO，否则保留USB记录。
- `8F`：以SET DATA接受时的时间戳计算年龄。遇到RF容量不足时，8ms以内保留USB记录，等待后续RX/相位释放；年龄达到8ms且容量仍不足，计一次超时和一次RF拒绝，退休未开始的USB记录。
- **8ms不是端到端期限**：loop检查可能晚于8ms；检查时已有容量会正常接受。FIFO/lane中的已接受命令继续按原规则交付。其他命令的队头阻塞也可让触觉等待超过8ms。
- `A2–A6`：保持立即尽力尝试，容量不足即分账拒绝。
- 会话generation过期先退休；离线、USB reset取消另计。相同字节的触觉仍是独立写入。
- 每槽FIFO仍为12片，另有两个已序列化lane；不跳过已分配的CCM counter，不改变修订号10认证相位门控。

本轮未引入按槽重排、控制预留容量、RF FIFO老化/替换或扩容。A1/B4回复仍由loop末尾发布，不能据调度变化宣称7–43µs立即GET的STALL已消除。

## 2. 计时与冻结边界

生产USB与协议共用 `radio::queueMicros()`：TIMER3为32位、1MHz，独立capture3，不覆盖RX的capture0、ACK/跳频compare1/2及主循环/ISR的capture4/5。

Seeed核心1.1.13的 `delay.h` 在DWT启用时返回 `DWT->CYCCNT / 64`，会约每67.109秒回零，不能直接按32位微秒差值处理。新的队列时钟使用完整TIMER3计数；约71.6分钟自然回卷，差值按uint32计算。身份导入导致RADIO重新begin时，记录旧计数offset并延续逻辑时间；重配期间短暂STOP不计时。首次TIMER3启动前使用 `millis()*1000`，精度受RTOS tick限制。

时钟不是UTC、不是启动唯一标识；跨重启、超过一整轮回卷的间隔不能直接相减。原有RF END/READY时刻仍在各自TIMER3 epoch内使用。未安装clock hook的native调用者明确返回协议时间不可用。

FC页0在短临界区冻结时间戳、USB计数、深度和pending；仅loop可修改的协议计数/事件环在恢复中断后复制，pending RF年龄按同一采样时刻计算。没有把整份事件环复制、AES或Flash放入临界区。采样代码仍有执行成本，不能视为零干扰。

**冻结范围仅为FC**。F0/F7/F8/FA/FB仍是顺序读取，不能与FC组成一个全局原子快照。

## 3. 计数口径

|字段|单位及含义|
|---|---|
|`admitted` / `haptic_admitted`|RF接受的整条Feature命令 / 其中8F；查询复用不重复计数|
|`haptic_full`、`a2_full`…`a6_full`、`other_full`|对应完整Feature容量拒绝；内部enqueue意外拒绝也归other|
|`initial_retries` / `count_retries`|04 / 03内部控制因容量不足的重试次数；同一控制可累计多次|
|`commit_waits`|提交因FIFO满而延后的容量检查次数，不增加旧F0 queue_full|
|`offline`|协议层离线转发命令次数，排除本地命令|
|`serialized` / `retired` / `cleared_fragments`|分片完成加密 / 认证相位退休 / 会话或transport重置清除；含内部控制，bootstrap ACK缓存另算|
|`usb_admitted` / `usb_processed`|进入 / 退休USB环的SET记录，包含本地诊断，不包含GET消费记录|
|`usb_haptic_received`|USB环接受的8F命令数，不是所有host尝试数|
|`usb_haptic_waits`|每条8F首次遇到容量不足计一次，不随loop重试递增|
|`usb_haptic_timeouts`|期限已到且仍满的8F拒绝，是 `haptic_full` 的子集|
|`usb_haptic_stale`|会话generation过期丢弃，不是年龄超时|
|`usb_haptic_offline` / `usb_haptic_reset_cancelled`|离线分派 / USB reset取消的8F|
|`fifo_high_water`|每槽明文FIFO分片高水位，最大12，不含两个lane|
|USB高水位/深度|共享环中的SET及GET消费记录，最大31|
|`max_fifo_age_us`|分片入FIFO至序列化/清除的累计最大年龄；快照还考虑当前最老FIFO项|
|`max_lane_age_us`|序列化至退休/清除的累计最大年龄；快照还考虑当前有效lane|
|`usb_max_age_us`|loop已经检查过的SET记录的最大年龄；不保证覆盖尚未检查的全部队尾项|
|`max_rx_latency_us`|loop处理RX时距RADIO END的最大时间|
|`max_process_gap_us`|相邻 `usb::process` 调用开始时间的最大间隔，不是函数运行耗时|

累计计数跨会话/USB reset保留，上电重新初始化；uint32自然回卷。高水位和最大年龄不能通过相减得到某个测试窗口的最大值。RX目前提供延迟，未新增RX深度高水位。

只经生产USB入口提交8F时，每槽同一FC快照满足：

```text
usb_haptic_received = haptic_admitted + haptic_full
                    + usb_haptic_stale + usb_haptic_offline
                    + usb_haptic_reset_cancelled + pending_haptics
```

`usb_haptic_timeouts`不能再次加进右侧。RF入队后会话清除的片仍属于已接受命令；`retired`也不证明手柄已执行触觉。多分片时不能将退休分片数直接当作完成命令数。

旧F0 `queue_full`继续累计Feature容量拒绝及内部04/03重试，不能与旧PCAP命令总数直接组成丢包率。FC页0自身也进入USB计数，采样时当前FC命令尚未退休，因此诊断会留下正常的 `usb_admitted - usb_processed` 差值。

## 4. FC v1线格式

仅XIAO修订号>=11。64字节Feature payload，WebHID report ID单独为0，hidapi前置0后共65字节。所有多字节整数为LE。

请求：

- `FC 00` 或 `FC 01 00`：创建新冻结快照并返回页0。
- `FC 05 page idLE32`：读取指定快照的页1..17。
- 任意接口的新页0替换全局快照；USB reset使快照失效。ID跳过0，但不保证跨启动唯一。

所有回复的公共头：

|偏移|含义|
|---|---|
|0..5|`FC,62,1,page,firmwareRevision,status`|
|6..9|snapshot ID|
|10..13|snapshot clock，微秒|

status：0成功、1非法选择参数、2快照失效。错误页的ID/时钟可能属于新的快照，不能拼接入旧结果。

### 页0：摘要

14 RF容量12；15 USB容量31；16保留事件数0..16；17 USB高水位。

从18开始的十个LE32依次为：

```text
haptic_admitted_total, haptic_full_total, a2_a6_full_total,
control_retries_total, haptic_waits_total, haptic_timeouts_total,
max_fifo_age_us, max_usb_age_us, max_rx_latency_us, max_process_gap_us
```

58..61等待预算8000；62各槽RF高水位的最大值；63协议clock可用标志0/1。total为四槽之和，年龄为四槽最大值。

### 页1..4：槽0..3接受/拒绝

从14开始的十二个LE32依次为：

```text
admitted, haptic_admitted, haptic_full,
a2_full, a3_full, a4_full, a5_full, a6_full,
other_full, initial_retries, count_retries, commit_waits
```

### 页5..8：槽0..3推进/USB

从14开始的十二个LE32依次为：

```text
offline, serialized, retired, cleared_fragments,
max_fifo_age_us, max_lane_age_us, usb_admitted, usb_processed,
usb_haptic_received, usb_haptic_waits, usb_haptic_timeouts, usb_haptic_stale
```

页1..8的62为该槽FIFO高水位，63为待处理触觉数；同槽两页的值必须一致。

### 页9..16：最近16项事件

每页offset14、38各一条24字节事件，62为实际条数0..2，63保留。顺序是环中最旧到最新；覆盖旧事件后计数仍为16。

```text
LE32 id, at_us, generation, age_us
uint8 slot, kind, command, length, needed, fifo_count, lane_flags, usb_depth
```

kind：1 HapticFull、2 BestEffortFull、3 OtherFull、4 InitialRetry、5 CountRetry、6 UsbWait、7 UsbTimeout、8 StaleUsb、9 Offline。

`length`为Feature逻辑整帧长（命令+长度+参数）或内部控制body长度；`needed`为所需分片数。lane_flags的bits0/1表示lane有效，bits2/3表示认证相位见证。事件不保存参数正文、key、IV。协议层拒绝没有USB年龄/深度上下文时填0；0不是测得零等待。一次触觉超时可能产生Wait、Timeout和HapticFull三项，不能将事件行数当作丢弃数。事件ID同样可自然回卷。

### 页17：USB补充

每槽在 `14 + 12*slot` 放三个LE32：`usb_max_age_us, usb_haptic_offline, usb_haptic_reset_cancelled`。62为采样时USB深度，63为默认USB批量4。

## 5. 网页与CLI

- 网页常规轮询只读FC页0；“读取冻结队列快照”和“下载诊断JSON”读取完整页组，完整结果保存为 `queueSnapshot`，带独立主机 `capturedAt`。
- 完整读取校验每页版本、ID、时钟、行数和容量；token失效时丢弃已读页并整组重试一次。页数为10..18，按实际事件数选择事件页。
- 本网页串行化整组读取，防止自己的轮询打断；其他网页/进程仍可使token失效。常规轮询不会覆盖已经保存的完整历史快照。
- 导出失败时保留旧快照的原采集时间，并附 `queueSnapshotError`，不伪装成新采样。
- CLI `status`读摘要，`queue`读完整快照；FC前校验XIAO A1、F0身份与F7修订号，原版和旧固件不发送FC。

```powershell
python tools/receiverctl.py list
python tools/receiverctl.py queue --path "明确选定的XIAO vendor HID路径"
```

## 6. 真实到达序列的软件回放

`tests/haptic_arrivals.json`只保留两份既有USBPcap的8F请求到达时刻及10字节逻辑命令：原版frames11627..13249共278条、跨度4.912194秒；XIAO frames14379..16197共371条、跨度4.986931秒。输入PCAP SHA-256、选区和解压后payload SHA-256随fixture保存。生成器位于工作区 `analysis/receiver_d0ggle/scripts/export_haptic_fixture.py`。

测试直接使用生产Receiver、CCM、USB桥和认证相位门控；旧方案USB16→RX32→service，新方案RX8→service→USB4→service×4。**RF轮询4/8ms、loop周期1ms、RX认证延后1.5ms是模型参数**，并非USB抓包测得的RF服务时间。相同时刻采用host→RF→loop顺序，不模拟MMIO执行耗时。

下表“拒绝”为已进入USB环但未进RF FIFO的命令；延迟为模型peer接收时刻减USB到达时刻，只统计已接受命令，单位ms：

|负载|RF轮询|旧→新拒绝|旧→新p95|旧→新最大RF年龄|
|---|---:|---:|---:|---:|
|16条同时到达|4ms|4→2|92→108|82→98|
|原版278条|4ms|0→0|13.128→13.128|19→19|
|XIAO371条|4ms|0→0|13.246→13.246|20→20|
|16条同时到达|8ms|4→2|184→216|162→194|
|原版278条|8ms|19→18|209.403→216.818|191→192|
|XIAO371条|8ms|50→49|212.584→220.355|191→192|

32条同时到达的USB环边界，两方案各1条USB拒绝、17条RF拒绝、14条交付。新策略USB最大年龄9ms，说明8ms不是硬实时调度保证。每个已接受命令按序恰好交付一次，重复新策略回放结果一致；所有场景维持连接、RF RX环丢弃0。

**结果是小幅减少突发拒绝，同时付出部分尾延迟**，没有复现旧现场267次，也不证明现实中的吞吐已经改善。完整输出保存在 `build/tests/protocol_test.log`；实物突发已测得更长FIFO年龄，见 [实物记录](REV11_LIVE_VALIDATION.md)。

## 7. 后续复测

同一固件启动周期内，在负载前后各导出一次完整FC快照，比较同字段的uint32增量及pending余额。最大年龄/高水位是累计值，若需要新窗口最大值，应在独立启动的可控试验中测量，不能相减。

Steam触觉复测采用同一USB拓扑、配置与恢复模式，运行时断开网页。记录触觉感觉、实际断开和完整诊断；结合分类计数，再决定未开始整命令的新鲜度策略、控制保障或容量调整。已序列化lane与未知第8触觉参数保持原语义边界。
