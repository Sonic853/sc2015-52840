# SC2015 Receiver — XIAO / ProMicro nRF52840
<img width="927" height="838" alt="image" src="https://github.com/user-attachments/assets/691f71e8-94fa-4776-9566-de296bf6b499" />

Steam Controller **2015版**私有2.4GHz接收器实现，Steam模式USB身份为 `28de:1142`。提供 **DX/Seeed XIAO nRF52840（S140 7.3.0）** 和 **OpenPuck同类ProMicro/SuperMini nRF52840（S140 6.1.1）** 两种独立UF2构建。板型和手动日期Release说明见 [FIRMWARE_RELEASES.md](docs/FIRMWARE_RELEASES.md)。

**当前修订号19新增左背键＋右背键＋Steam开机进入Lizard键盘/鼠标模式**：左板画圈滚轮、右板移动鼠标、右扳机左键、左扳机右键，其余默认键位参照OpenPuck。完整映射见 [LIZARD_MODE.md](docs/LIZARD_MODE.md)。保留Switch/PS5真实IMU、Xbox映射、整机重启切换及记住模式。网页19.0支持五种模式选择和中英文切换；配置接口仅Steam模式提供。软件回归和ARM构建通过，实物行为待验证。

## 固件与使用

生成文件：

```text
build/xiao/SCReceiver-xiao-s140-7.3.0.uf2
build/xiao/SCReceiver.ino.hex
build/xiao/SCReceiver.ino.elf
build/xiao/manifest.json
build/promicro/SCReceiver-promicro-s140-6.1.1.uf2
build/promicro/manifest.json
```

1. 首次更新双击板卡RESET，进入UF2盘。安装修订号5后，也可在网页选择目标XIAO，点击“进入 UF2 刷入模式”。核对 `INFO_UF2.TXT` 中板型及SoftDevice版本。
2. 将与板型和SoftDevice对应的UF2复制到该盘；XIAO与ProMicro文件不可混刷。默认Steam模式枚举为五个HID interface。
3. 无绑定记录时，上电自动接受新配对；2015版手柄通常按住 **X+Steam** 开机。此本地配对在最终密钥得到认证后自动保存，无需保持网页开启。
4. Steam／网页通过AD开启配对时使用B3提交；过早的B3会等待最终密钥确认，不会把bootstrap引导密钥当成永久绑定。
5. 已有绑定时，普通按 **Steam** 开机恢复；重新配对可按外接 **D0(P0.02)→GND** 按键至少500ms，其本地配对也自动保存最终密钥。没有外接按键时使用网页或主机工具。

也可使用 [网页控制台](web/index.html)，通过 GitHub Pages HTTPS 或 localhost + WebHID 操作。当前固件为**修订号19 / F0诊断协议v3 / FC队列协议v1 / FD–FE输入配置v1**，同名UF2已更新；网页配置限Steam模式，仍兼容旧版复合Xbox配置HID。重连循环修复与FB诊断见 [RECONNECT_LOOP_FIX.md](docs/RECONNECT_LOOP_FIX.md)，熵设置回归见 [ENTROPY_FIX.md](docs/ENTROPY_FIX.md)，通信差异见 [COMMUNICATION_AUDIT.md](docs/COMMUNICATION_AUDIT.md)，触觉写入见 [HAPTIC_FIX.md](docs/HAPTIC_FIX.md)，网页部署见 [WEB_CONSOLE.md](docs/WEB_CONSOLE.md)。

XIAO指示灯（低电平亮）：蓝灯闪烁＝配对开启；绿灯＝至少一个无线连接；红灯持续快闪＝初始化失败。ProMicro使用P0.15单LED（高电平亮）：配对闪烁、在线常亮、故障快闪。两板可选外接配对键均为P0.02→GND。板载RESET保留bootloader的双击恢复用途。没有使用外接射频模块或板载QSPI Flash。

UF2只含应用：XIAO从 **0x27000** 链接，ProMicro从 **0x26000** 链接，不包含bootloader、SoftDevice、UICR、映射配置或绑定页。它们分别编译，不能通过改UF2地址互换。目标布局分别为XIAO S1407.3.0与ProMicro S1406.1.1。

### Xbox / PS5 / Switch / Lizard输入与配置

同时保持**左背键＋Steam开机进入Xbox**、**右背键＋Steam开机进入PS5**、**左肩LB＋Steam开机进入Switch**、**左背＋右背＋Steam进入Lizard**，或**A＋Steam开机返回Steam**；无线连接后继续保持约300ms，再松开组合键。窗口为新会话前3秒；A返回优先，双背组合为Lizard，LB与背键混合不选择模式。切换会保存启动选择、断开USB、等待60ms并重启接收器。默认一次性选择消费后，下次上电Steam；也可保存“记住输入模式”。四种翻译模式均需返回Steam再配置。

默认左触控板**按压后**输出十字键，松开按压即释放；右触控板模拟右摇杆及R3。左背键R3、右背键L3，物理左摇杆独立。网页可保存16键映射、死区、反转和**斜向区域角度0–75°（步长5°）**；越小越不容易误触斜向，0°关闭斜向。新默认30°，旧配置保持45°供手动调整。Xbox双通道震动用左右触控板短脉冲模拟，主机2.5秒未刷新则停止续发。Steam键默认映射Guide，需完全松开开机组合后单独按；未实现Xbox主机认证。

PS5共用这份配置：A/B/X/Y→×/○/□/△，LB/RB→L1/R1，LT/RT→L2/R2，Back/Start→Create/Options，Steam→PS键。USB为`054c:0ce6`单HID，目标为PC输入，不代表PS5主机兼容；详细范围见 [PS5_MODE.md](docs/PS5_MODE.md)。

Switch按物理位置映射：A/B/X/Y→B/A/Y/X，LB/RB→L/R，LT/RT→数字ZL/ZR，Back/Start→−/+，Steam→Home；USB为`057e:2009`单HID。主机复测开启“Pro手柄有线连接”。PS5/Switch均转发原始gyro/accel，各组100ms无新完整样本后归零；Switch开启IMU后15ms一包，包内三组保持最近有效值，不是三次独立采样。主机识别、真实方向/灵敏度与震感仍待实测。

Xbox模式可运行 `python tools/diagnose_xinput.py --samples 60 --interval 0.1 --output build/xinput-windows.json`，读取PnP、XInput及Guide扩展状态。该工具默认不需要hidapi；当前Xbox没有FD配置HID，不传`--path`。查看`guide.pressed`而不只看普通API按钮位。解释见 [MODE_REBOOT_AND_GUIDE.md](docs/MODE_REBOOT_AND_GUIDE.md)。

Lizard使用独立固定映射，不受上述游戏手柄映射影响：A/B/X/Y为Enter/Esc/PageUp/PageDown，Back/Start为Tab/Esc，LB/RB为Ctrl/Alt，左摇杆为方向键；左板画圈顺时针向下滚、逆时针向上滚。详见 [全部Lizard按键及快捷键](docs/LIZARD_MODE.md)。

## 构建

Windows，项目根目录 `Receiver` 下：

```powershell
python tools/dependencies.py
python tools/build.py
python tools/build.py --verify build/xiao/SCReceiver-xiao-s140-7.3.0.uf2

# ProMicro / SuperMini（参照OpenPuck的Feather配置）
python tools/dependencies.py --target promicro
python tools/build.py --target promicro --clean
python tools/build.py --target promicro --verify build/promicro/SCReceiver-promicro-s140-6.1.1.uf2
```

依赖被安装到本目录 `.tools/`，不会修改用户Arduino配置或参考openpuck仓库：

- Arduino CLI **1.3.1**，下载校验官方release asset SHA-256。
- Seeed `Seeeduino:nrf52` **1.1.13**，板型 `xiaonRF52840`。
- ProMicro采用Adafruit `adafruit:nrf52` **1.7.0**，板型 `feather52840`。
- Adafruit TinyUSB Library **3.6.0**，覆盖Seeed附带的旧版USB库。
- 核心自带ARM GCC **9.2.1**。

可通过 `--cli PATH` 指定Arduino CLI。构建不扫描或刷写硬件；脚本校验板型、链接脚本、实际向量、编译进镜像的原厂HID report descriptor，以及每个UF2块的地址/重叠/家族ID。`compile.log` 和 `manifest.json` 保留构建结果。

### GitHub Actions手动构建

以 `Receiver/` 为GitHub仓库根目录，将源码及工作流提交到默认分支后：

1. 打开仓库 **Actions → Receiver firmware build**（XIAO）或 **ProMicro nRF52840 firmware build**。
2. 点击 **Run workflow**，选择需要构建的分支，再确认运行。
3. 成功后在该次运行的 **Artifacts** 下载 `SCReceiver-<xiao或promicro>-<run_id>-<run_attempt>`。
4. 解压使用对应板型的UF2；同包包含HEX、ELF、`manifest.json`和`compile.log`。运行摘要显示源码commit、日期版本/能力修订号、地址范围和UF2 SHA-256。

工作流仅通过 `workflow_dispatch` 手动触发，在 `windows-2022` 上使用Python 3.11，执行Python工具测试、固定版本依赖安装、干净ARM构建、二进制审计及UF2校验。Arduino依赖按安装脚本内容缓存，仍会运行安装脚本检查/补齐依赖。成功产物保留30天；失败时已有编译日志单独保留14天。原生协议ASan和网页测试仍按各自章节运行，不属于这个固件构建工作流。

### 手动发布双板固件Release

运行 **Actions → Receiver date release → Run workflow**。日期按北京时间命名，例如`2026.01.10`，同日再次发布为`2026.01.10p1`、`p2`……；两板构建并校验成功后才发布。Release的Assets提供带日期/板型/SoftDevice的两份UF2、HEX、manifest及`SHA256SUMS`。日期版本嵌入固件并写入manifest，诊断协议的数字修订号19保持独立。操作、文件名示例及失败重跑方式见 [FIRMWARE_RELEASES.md](docs/FIRMWARE_RELEASES.md)。

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
- 连续相同触觉脉冲的USB→加密RF转发、两个触控板选择值、无应用回复/可选GET/已有回复缓存、重复设置分片及FIFO容量；读取查询仍去重。
- 94族回退、接收器本地命令隔离、USB环预留及复位竞态、RF满时完整命令保留、RX溢出后双lane恢复进度、bootstrap AES失败后重接入。
- 单in-flight键鼠公平性、B1关→开清运动、Boot/Report格式与USB复位/拔出/恢复时输入释放。
- 四接口0x96熵设置与连续加密输入/左右触觉交错、满RF FIFO本地完成、无额外Flash/换钥，以及拒绝命令元数据回归。
- 按手柄端规则处理上行回显的双向模型：旧包认证失败、丢ACK、批量快照及持续触觉；旧会话B2/B3/设置隔离、FB实际断开原因与最终状态快照。
- 队列分类、8ms容量等待、会话/reset取消守恒、FC冻结分页与token失效；使用捕获的278/371条触觉到达序列比较旧/新调度，RF时序为模型参数。
- TIMER3完整微秒计数、自然回卷和身份重配epoch连续性；CLI身份/版本门控与混合快照拒绝。
- UF2保护区、错误向量及畸形块拒绝。
- Switch握手/子命令/SPI RAM、回复队列与HD rumble幅度oracle；PS5/Switch完整CCM输入、原始IMU轮转分组、主机单位/轴极值、超时与USB重置隔离。

这些是**软件协议测试**；触觉到达时刻与参数取自USB捕获，RF对端和调度为模型，不是原手柄密文抓包，也不执行8051或nRF固件。Mock USB不验证nRF USBD硬件；实物结果另行保存。

## 控制和诊断

```powershell
python -m pip install hidapi
python tools/receiverctl.py list
python tools/receiverctl.py status --path "上一步列出的vendor HID路径"
python tools/receiverctl.py queue --path "同一XIAO接口路径"
python tools/receiverctl.py pair --path "同一接口路径"
python tools/receiverctl.py commit --path "已连接槽的接口路径"
python tools/receiverctl.py capture --seconds 10 --path "同一接口路径"
```

四个vendor接口分别是slot0..3；提交时选中出现连接的接口。`forget` 清空全部绑定记录，`unpair-mode` 关闭接受新配对。工具使用显式设备路径；已执行的实物命令与范围记录在验证文档中。

`status` 的扩展Feature命令F0提供：CRC正确/错误、应用收到包数、MIC失败、认证重复、ACK逾期、RADIO/USB队列丢包、Flash失败及活动pipe。

F0 v2还包含实际槽号和归一化RF身份，让浏览器不用猜测USB枚举顺序。F1–F6为本工程新增的绑定快照/事务导入协议，可保存接收器身份及20条绑定；它们不是原版Valve接收器已有的命令。完整字段见 [MIGRATION_PROTOCOL.md](docs/MIGRATION_PROTOCOL.md)。

F0 v3继续保持原字段位置，另用F7提供分pipe收包、最近S1/软件header、ACK安排/TX READY/完成次数及处理延迟。旧版将S1.bit0的ACK请求含义写反，可能只收包却不应答；修订号3已修正，并增加换钥交界和跳频中断保护。

修订号4另外提供F8：已保存绑定数、引导密钥记录数、查询命中/未命中、H=2恢复请求/接受/认证失败、延后保存和未保存即断开次数。它不包含id或key。网页“绑定与重连”与命令行status均显示这些字段，能区分空库、错密钥和等待最终换钥。

修订号5的F9进入XIAO UF2模式：网页收到确认后释放该接收器所有接口，固件在EP0状态阶段完成后调用核心`enterUf2Dfu()`。进入模式及本工程应用更新不擦除已保存绑定。网页只发送进入命令；UF2文件通过出现的磁盘复制。原版接收器和旧固件不会收到F9。

修订号6对无线读取查询先排队并让USB SET返回待就绪失败，主机重试到真实回复到达后才成功；就绪SET不清缓存。FA只报告命令号、长度、忙/就绪及未发布GET计数，辅助定位Steam初始化。新版网页优先通过空闲槽读取F0/F8，不再定时向全部槽发送B4连接事件。Steam实物复测先点击“断开网页”，再重启Steam并普通Steam键开机。

修订号7将无线pending/回复缓存去重限定为读取查询。`0x8f`触觉脉冲、`0x87`设置等每次SET均独立排队；相同参数不再被当成前一次请求的重试。FA v1字段不变，最近SET为`0x8f`只能证明USB收到过脉冲，不能代替手柄实际振动确认。

修订号8在USB成功前预留队列记录，普通写等待RF有完整命令容量；`8f/a2–a6`保持原版尽力写语义。RX环满时不发ACK，避免丢失双lane释放相位。另修正94族GET回退、Output/Feature类型、输入复位和桌面键鼠公平性，移除额外BOS/WebUSB能力；网页仍通过标准WebHID通信。FA格式不变，`command_drops`也包含容量不足而拒绝的USB请求。

修订号9纠正对`0x96`的错误分类：它提供原版PRNG熵，不是bootloader命令。XIAO使用硬件RNG，接受该提示并本地完成，避免Steam反复重试；不向手柄转发或重新播种活动会话。FA保留字节增加最近拒绝command、声明长度、slot和原因；新版网页与CLI可显示，旧网页仍能读取原有字段。

修订号10让普通下行载荷等待对应已认证相位，尚未就绪时发单字节反馈ACK；USB转发命令及B2/B3绑定接受时的会话编号，避免跨重连执行。FB诊断提供看门狗/B2/会话替换/换钥失败/清库的实际断开计数及各槽断开前的lane、phase和序号低8位，网页JSON字段为`transportLink`。

修订号11新增FC队列分账和冻结快照，网页可读取/导出各槽计数、年龄及最近16项元数据事件。调度改为RX8/USB4小批交错，8F在RF容量不足时最多额外等待到8ms预算后再计超时；这不是端到端触觉期限。`haptic_timeouts`是`haptic_full`的子集，不能重复相加。计数和实测取舍见 [QUEUE_IMPROVEMENTS.md](docs/QUEUE_IMPROVEMENTS.md)。

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
|`queue_metrics.h` / `queue_policy.h`|固定大小分类计数/事件、USB/RX批量及触觉等待预算|
|`input_mode.{h,cpp}`|输入源分离、开机组合键、按键映射、八方向及配置记录编码|
|`xinput.{h,cpp}`|PC XInput类驱动、20字节输入、Microsoft OS兼容描述符|
|`ps5.{h,cpp}`|PC DualSense HID、共用映射转换、Feature及双通道震动输出|
|`switch_pro.{h,cpp}`|Switch Pro HID、握手/校准、IMU报告及HD rumble幅度解码|
|`motion.{h,cpp}`|SC2015完整sensor组新鲜度、PS5/Switch单位与轴转换|
|`lizard.{h,cpp}`|固定桌面键鼠映射、左板画圈滚轮、触摸锚点及输入超时|
|`rumble.{h,cpp}` / `usb_tx_guard.h`|三种翻译模式共用SC2015有限触觉脉冲、会话隔离及USB发送优先级保护|
|`storage.{h,cpp}`|映射及启动选择双页、独立绑定双页，CRC/generation/最后提交字保护|
|`board_profile.h` / `build_info.cpp`|XIAO/ProMicro引脚配置及嵌入日期版本|
|`SCReceiver.ino`|启动、配对按键、状态LED与看门狗|

没有启用BLE/SoftDevice；保留Flash中的S140以满足原bootloader布局。TIMER3、PPI0/1、RADIO、ECB、RNG由本工程独占；不要同时启动Bluefruit/BLE或使用占用这些资源的库。

### Flash布局

下表为XIAO S1407.3.0；ProMicro S1406.1.1的MBR/SoftDevice结束于`0x25fff`、应用从`0x26000`开始，其余存储保护地址相同。

```text
00000..26fff  原MBR / S140 7.3.0，更新不触碰
27000..e8fff  应用，链接ASSERT和HEX/UF2检查共同限界
e9000..e9fff  Xbox按键映射配置A
ea000..eafff  Xbox按键映射配置B
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

用户已确认修订号10的Steam连接和触觉；修订号11另有本机升级、绑定恢复、计时及队列负载验证。首次发现/join的物理ACK时序、长期跳频同步、多手柄负载、SDL兼容性、持续Steam触觉及完整掉电矩阵仍待确认。实测仍有MIC失败计数增长与突发FIFO长等待，未声称链路错误清零。尚无真实手柄会话的已知明密文向量。

相对旧实现的明确设计选择：短USB报告未定义尾部填零；Feature回复按槽独立缓存以避免跨槽覆盖；队列满时合并过期输入并保留尽可能多的连接通知；持久化失败不清pending、不发送成功提交控制消息。它们不改变已恢复的空口字段。

OpenSteamController当前自定义主控固件没有无线驱动。实物测试请先使用官方手柄固件，并选择私有无线模式。本工程也不支持Steam Controller 2/Watchman/BLE；OpenPuck用于XIAO/TinyUSB、USB输入模式及构建参考，未移植其SC2无线协议和IMU校正。

协议来源与参考commit在 [docs/provenance.json](docs/provenance.json)；更详细的函数地址及静态证据在工作区 `analysis/receiver_d0ggle/IMPLEMENTATION_{CRYPTO,LINK}.md`、`analysis/radio_analysis/cross_endpoint.md`。
