# SC2015 Receiver — XIAO nRF52840

Steam Controller **2015版**私有2.4GHz接收器实现，USB身份为 `28de:1142`。目标板是你提供资料中的 **DX XIAO / Seeed XIAO nRF52840，S140 7.3.0 UF2 bootloader**。

工程已通过实际 ARM 编译、UF2地址检查和主机协议测试。用户已反馈修订号3首次配对成功；**当前修订号4的重连及连接矩阵修复仍需实物复测**。合成测试不代替实际输入、多手柄或时序测量。

## 固件与使用

生成文件：

```text
build/xiao/SCReceiver-xiao-s140-7.3.0.uf2
build/xiao/SCReceiver.ino.hex
build/xiao/SCReceiver.ino.elf
build/xiao/manifest.json
```

1. 双击板卡RESET，进入UF2盘。核对 `INFO_UF2.TXT` 中板型及SoftDevice版本。
2. 将上面的 **XIAO专用UF2** 复制到该盘，重新枚举后应出现Steam Controller接收器的五个HID interface。
3. 无绑定记录时，上电自动接受新配对；2015版手柄通常按住 **X+Steam** 开机。此本地配对在最终密钥得到认证后自动保存，无需保持网页开启。
4. Steam／网页通过AD开启配对时使用B3提交；过早的B3会等待最终密钥确认，不会把bootstrap引导密钥当成永久绑定。
5. 已有绑定时，普通按 **Steam** 开机恢复；重新配对可按外接 **D0(P0.02)→GND** 按键至少500ms，其本地配对也自动保存最终密钥。没有外接按键时使用网页或主机工具。

也可使用 [网页控制台](web/index.html)，通过 GitHub Pages HTTPS 或 localhost + WebHID 操作。当前固件为**修订号4 / 诊断协议v3**，同名UF2已更新；网页兼容v2/v3。重连原因、连接矩阵及复测步骤见 [CONNECTION_FIX.md](docs/CONNECTION_FIX.md)，网页部署见 [WEB_CONSOLE.md](docs/WEB_CONSOLE.md)。修订号3的ACK修复历史见 [PAIRING_FIX.md](docs/PAIRING_FIX.md)。

指示灯（低电平亮）：蓝灯闪烁＝配对开启；绿灯＝至少一个无线连接；红灯持续快闪＝初始化失败。板载RESET保留bootloader的双击恢复用途。没有使用外接射频模块或板载QSPI Flash。

本UF2只含从 **0x27000** 链接的应用，不包含bootloader、SoftDevice、UICR或绑定页。它不是Pro Micro从0x26000链接的固件，也不能通过改UF2地址重定位。若板卡当前bootloader/SoftDevice布局不同，先确认实际布局再构建相应目标；本工程仅提供7.3.0目标。

## 构建

Windows，项目根目录 `Receiver` 下：

```powershell
python tools/dependencies.py
python tools/build.py
python tools/build.py --verify build/xiao/SCReceiver-xiao-s140-7.3.0.uf2
```

依赖被安装到本目录 `.tools/`，不会修改用户Arduino配置或参考openpuck仓库：

- Arduino CLI **1.3.1**，下载校验官方release asset SHA-256。
- Seeed `Seeeduino:nrf52` **1.1.13**，板型 `xiaonRF52840`。
- Adafruit TinyUSB Library **3.6.0**，覆盖Seeed附带的旧版USB库。
- 核心自带ARM GCC **9.2.1**。

可通过 `--cli PATH` 指定Arduino CLI。构建不扫描或刷写硬件；脚本校验板型、链接脚本、实际向量、编译进镜像的原厂HID report descriptor，以及每个UF2块的地址/重叠/家族ID。`compile.log` 和 `manifest.json` 保留构建结果。

## 主机协议测试

```powershell
python -m pip install pycryptodome
python tools/test.py --sanitize
```

Windows默认使用已安装的Visual Studio C++工具链；其他安装路径可传 `--vcvars PATH`。Linux使用 `CXX`/`c++` 和OpenSSL开发库，默认启用ASan/UBSan。

测试直接编译生产 `crypto.cpp`、`protocol.cpp`、`usb.cpp`：

- 108个独立PyCryptodome AES-CCM向量，全部明文长度1..27、两方向、MIC篡改、AAD掩码和错误counter。
- 128组从8051固件算法冻结的合成地址/七槽表；地址位序、39位counter、定时器回卷和Flash暂停后的跳频追赶。
- 新配对、双pending ACK、键鼠/状态重排、局部补丁、Feature切片/重组、密钥更新、绑定提交失败及恢复、重连、四槽隔离和超时。
- 每字节持久化记录破坏与所有截断提交拒绝；generation回卷。
- 生产USB桥接在Mock TinyUSB下的callback延期、队列溢出、报告描述符、输入合并和suspend处理。
- UF2保护区、错误向量及畸形块拒绝。

这些是**合成协议/软件测试**，不是原手柄密文抓包，也不执行8051或nRF固件。Mock USB不验证nRF USBD硬件。

## 控制和诊断

```powershell
python -m pip install hidapi
python tools/receiverctl.py list
python tools/receiverctl.py status --path "上一步列出的vendor HID路径"
python tools/receiverctl.py pair --path "同一接口路径"
python tools/receiverctl.py commit --path "已连接槽的接口路径"
python tools/receiverctl.py capture --seconds 10 --path "同一接口路径"
```

四个vendor接口分别是slot0..3；提交时选中出现连接的接口。`forget` 清空全部绑定记录，`unpair-mode` 关闭接受新配对。工具使用显式设备路径；本次实现过程未向物理设备发送这些命令。

`status` 的扩展Feature命令F0提供：CRC正确/错误、应用收到包数、MIC失败、认证重复、ACK逾期、RADIO/USB队列丢包、Flash失败及活动pipe。

F0 v2还包含实际槽号和归一化RF身份，让浏览器不用猜测USB枚举顺序。F1–F6为本工程新增的绑定快照/事务导入协议，可保存接收器身份及20条绑定；它们不是原版Valve接收器已有的命令。完整字段见 [MIGRATION_PROTOCOL.md](docs/MIGRATION_PROTOCOL.md)。

F0 v3继续保持原字段位置，另用F7提供分pipe收包、最近S1/软件header、ACK安排/TX READY/完成次数及处理延迟。旧版将S1.bit0的ACK请求含义写反，可能只收包却不应答；修订号3已修正，并增加换钥交界和跳频中断保护。

修订号4另外提供F8：已保存绑定数、引导密钥记录数、查询命中/未命中、H=2恢复请求/接受/认证失败、延后保存和未保存即断开次数。它不包含id或key。网页“绑定与重连”与命令行status均显示这些字段，能区分空库、错密钥和等待最终换钥。

- CRC正确持续增加、认证数不增加：检查配对状态、地址/挑战、nonce或会话；不是单纯USB问题。
- `late_ack` 增长：需要检查实物ISR/ACK窗口。
- 连接后无USB输入：检查认证输入计数、host的83/87等Feature转发和目标slot；在线83属性直接由手柄返回。
- 原始接收器不支持此F0诊断，不要将它的返回解释成本工程状态。

## 架构

|文件|职责|
|---|---|
|`protocol.{h,cpp}`|四槽、发现/join、CCM计数与双lane可靠下行、配对、Feature、本体输入解压|
|`crypto.cpp`|标准CCM构造、nonce/AAD/MIC、地址转换、七槽算法和记录编码|
|`radio.{h,cpp}`|Nordic1M RADIO、EasyDMA、定时PRX ACK、TIMER3/PPI与RX环形队列|
|`usb.{h,cpp}`|原厂五接口布局、EP0 Feature队列、输入队列、键鼠和诊断|
|`storage.{h,cpp}`|两个独立4KiB Flash页的CRC/generation/最后提交字双缓冲|
|`SCReceiver.ino`|启动、配对按键、状态LED与看门狗|

没有启用BLE/SoftDevice；保留Flash中的S140以满足原bootloader布局。TIMER3、PPI0/1、RADIO、ECB、RNG由本工程独占；不要同时启动Bluefruit/BLE或使用占用这些资源的库。

### Flash布局

```text
00000..26fff  原MBR / S140 7.3.0，更新不触碰
27000..eafff  应用，链接ASSERT和HEX/UF2检查共同限界
eb000..ebfff  绑定数据库A
ec000..ecfff  绑定数据库B
ed000..f3fff  核心InternalFS保留区，本工程不使用
f4000..fffff  原bootloader/settings，更新不触碰
```

绑定数据库可保存20个 `id8+key16` 记录；同时活动四槽。常规配对通过B3提交，单条导入通过B0保存，整库恢复通过下述F5提交。保存时先写另一页，CRC核验后最后提交，旧有效页保留。Flash暂停射频期间，TIMER3持续计时，恢复时按原绝对deadline追赶跳频，避免人为重置相位。

网页整库恢复通过F3/F4暂存、F5一次提交，包含RF身份；成功后重新配置RADIO地址和跳频表，不需要更换USB连接。暂存期间暂停接入，取消/30秒超时恢复原配对状态；失败不会替换旧数据库。导入要求所有无线手柄已断开。

保留2015版手柄原有绑定，需要 **原接收器RF地址5 + 手柄ID8 + key16**。原版A1能读取身份，但当前应用镜像没有已证实的USB密钥导出命令；二代OpenPuck的A2/A3不是2015版读取接口。网页可以解析已有的32KiB MainBlock或1024字节绑定页转储，不能从19,118字节的`d0ggle.bin`应用文件生成设备绑定。进一步直接USB采集需要原接收器0x7800..0x7fff bootloader/读接口的证据。

### 无线与认证

Nordic1M、5字节地址、最大32字节DPL、CRC16、MSB-first、无白化。P0为VALVE发现地址，P1为接收器专属地址，P2–5为四个连接槽。应用层header与ESB PCF分开。

CCM由nRF ECB硬件加上可测试的CBC-MAC/CTR封装实现，nonce为 `LE39(counter)+方向位` 的5字节加IV8；MIC4、AAD=`header&83`。所有AES等待有超时；认证通过前不交付业务或推进counter。

RADIO IRQ只复制包和选择**已序列化**的ACK；不做AES、USB或Flash。修订号4使用双缓冲完整响应快照，并保留未确认join的精确ACK重发；主循环按RX记录的hop顺序处理watchdog。采用40µs fast ramp、TIMER3/PPI在接收END后名义130µs启动ACK发送。这个时序仍需实测；F0/F7有逾期与实际发送计数。边缘信道原-6dBm在52840没有对应档位，使用-8dBm，其他信道0dBm。

## 实现边界

重点是可编译、可测试的接收器实现，以下效果仍待实物确认：首次发现/join的物理ACK时序、长时间跳频同步、多手柄负载、Steam/SDL真实枚举和输入、触觉/LED等Feature双向控制、断电重连。尚无真实手柄会话的已知明密文向量。

相对旧实现的明确设计选择：短USB报告未定义尾部填零；Feature回复按槽独立缓存以避免跨槽覆盖；队列满时合并过期输入并保留尽可能多的连接通知；持久化失败不清pending、不发送成功提交控制消息。它们不改变已恢复的空口字段。

OpenSteamController当前自定义主控固件没有无线驱动。实物测试请先使用官方手柄固件，并选择私有无线模式。本工程也不支持Steam Controller 2/Watchman/BLE；OpenPuck仅用作XIAO/TinyUSB/构建参考。

协议来源与参考commit在 [docs/provenance.json](docs/provenance.json)；更详细的函数地址及静态证据在工作区 `analysis/receiver_d0ggle/IMPLEMENTATION_{CRYPTO,LINK}.md`、`analysis/radio_analysis/cross_endpoint.md`。
