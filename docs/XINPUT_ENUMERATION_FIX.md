# Xbox模式已生效但Windows没有手柄：修订号14

本文保存修订号14的历史枚举修复。当前同名UF2为修订号16，改用重启切换和`045e:028e`独立Xbox布局，Xbox不再提供FD配置HID；最新流程及Guide诊断见 [MODE_REBOOT_AND_GUIDE.md](MODE_REBOOT_AND_GUIDE.md)。以下旧`28de:1143`复合布局和产物hash不代表当前镜像。

## 现象与已确认错误

用户反馈修订号13切换后网页能读到Xbox模式，但Steam没有显示任何手柄。修订号13修复的是软件detach误当物理拔线；这次检查的是XInput接口本身。当前本机没有目标接收器，用户暂时不能连接，尚无这轮Windows驱动绑定或USB抓包证据。

离线核对发现，`xinput.cpp`的17字节XUSB接口描述符违反微软MS-XUSBI表46：

```text
旧：11 21 00 01 01 25 81 14 00 00 00 13 02 08 00 00 00
新：11 21 00 01 01 25 81 14 00 00 00 00 13 02 08 00 00
```

IN `wReports=0x8125`之后必须有5个报告长度（20及四个0），再接OUT `wReports=0x0213`及3个长度（8、0、0）。旧版少了第5个IN长度，OUT字段整体提前一字节，尾部多一个0。总长都是17，因此只检查长度无法发现。

新增按字段解码的原生回归先在旧生产代码上失败：

```text
INPUT MODE FAILED ...: descriptor[17] == 0 && descriptor[18] == 0 && descriptor[19] == 0 && descriptor[20] == 0
```

修复后通过。ARM产物审计也改成解析报告计数/长度和对应端点，而不是复制生产数组再比较。Python回归保留旧错误布局并验证其被拒绝。

**这是确定的描述符错误修复；目前不能断言它是现场唯一原因，或Steam已恢复识别。**

## 其他改动

- IF0增加规范表30的`Get_Report(Input)`：`A1 01 0100 0000 length`返回最近生成的20字节gamepad报告，短读裁切、长读最多20字节。仅接受输入report0/IF0/非零长度。EP0有独立快照，interrupt IN后续更新不会改变已安排的控制传输缓冲。
- Xbox的`bcdDevice`从`0x0114`升至`0x0115`。微软按VID/PID/bcdDevice缓存OS描述符探测结果；版本变化使这次修复使用新探测键。没有改系统注册表，也不保证既有驱动绑定必然刷新。
- 保留`28de:1143`复合身份、IF0 `XUSB10`兼容ID、配置HID和修订号13的VBUS判定。
- OUT初次接收安排失败时关闭已开的端点，避免误报接口打开成功。输入/输出传输结果进入FD诊断；Xbox rumble仍未转换。
- 未验证的Get_ID/能力私有回复、Xbox主机认证未实现；未知控制请求仍STALL，并保留最近SETUP的8字节元数据。新增Get_Report是规范兼容补充，没有现场证据证明之前Windows一定请求过它。

## 固件与网页诊断

修订号14在FD v1原保留尾部增加诊断版本1，前9字节及FD/FE命令保持兼容。各偏移均相对64字节payload（不含hidapi前缀Report ID零字节）：

|偏移|字段|
|---|---|
|9|诊断版本1|
|10|flags：bit0 enabled、bit1 opened、bit2 mounted、bit3 suspended|
|11|保留0|
|12..15|generation，模式enable、类reset及成功open时递增|
|16..19|opens，端点打开且初次OUT接收安排成功次数|
|20..23|resets，类reset调用次数，包括init/deinit|
|24..27|os_queries，接受的OS兼容描述符SETUP次数（含短头读取）|
|28..31|in_submitted，成功安排的20字节IN传输次数|
|32..35|in_completed，TinyUSB报告成功且长度20的IN完成次数|
|36..39|in_failed，IN安排失败或失败/长度异常的完成次数|
|40..43|out_completed，成功且长度不超过32的OUT完成次数|
|44..47|out_failed，失败/长度异常的OUT完成或初次/重新安排失败次数|
|48..51|control_requests，XInput类或Xbox vendor callback收到的SETUP数|
|52..59|最近上述SETUP原始8字节；没有记录时为0|
|60..63|control_rejected，上述不支持/参数错误SETUP数|

计数均LE32、自然回卷、本次上电累计，跨USB reset和模式变化保留；不是当前连接独占计数。读取时短临界区复制，FD是单次采样，不与F0/FC构成联合冻结。端点busy、未mounted或suspend时未安排输入不计入失败。失败完成与重新安排失败可以各计一次；不是“丢失手柄动作数”。

网页Xbox模式行显示**端点是否已打开、IN提交/完成/失败**，完整字段随`diagnostics.inputMode.xinput`导出；CLI `status`中的字段为`input_mode.xinput`。旧修订号12/13仍能读取模式，诊断扩展为null。

- `opened`仅证明固件TinyUSB类已打开端点，**不是Windows xusb22成功绑定的证明**。
- 比较多次采样的IN完成增量；旧的非零累计数不能证明现在还在读取。
- `os_queries=0`可能是Windows使用缓存，不能直接判为描述符失败。
- 真正的Windows XInput状态要用下面的系统探针读取。

## Windows只读探针

在`Receiver`目录运行。默认只需Windows自带PowerShell及Python标准库：

```powershell
python tools/diagnose_xinput.py --samples 20 --interval 0.25 --output build/xinput-windows.json
```

输出文件必须尚不存在，父目录必须已存在。脚本读取：

1. 当前`28de:1142/1143`的USB/HID PnP节点：硬件/兼容ID、service、INF、驱动版本、问题码、parent/container。
2. 系统目录中的XInput DLL（优先`xinput1_4.dll`）的0..3号槽：`XInputGetState`、`XInputGetCapabilities`及返回码，按样本记录按键、轴和packet number。
3. 未请求的HID不会被打开；不修改驱动、注册表、映射、绑定或模式，也不发送震动。

若需要把固件诊断放入同一文件，先点击网页“断开网页”，停止其他Feature读写者，再选确切vendor HID路径：

```powershell
python tools/receiverctl.py list
python tools/diagnose_xinput.py --path "上一步列出的Xbox配置HID路径" --samples 20 --interval 0.25 --output build/xinput-with-firmware.json
```

可选`--path`需要hidapi（也支持工程已有`.tools/hid-probe`）。它先验证A1/F0/F7身份及版本，再用`FD 00`读取；这些查询通过Feature SET选择报告，不提交模式或持久化操作。读失败会记录错误并关闭接口，不持续重试已移除设备。

全局XInput槽不能由本工具自动归属到某一接收器，字段明确写`device_attribution=unknown`；其他游戏手柄/虚拟手柄也占用这些槽。`state_result=0`为可读取，1167为未连接，其他错误保留原码且`connected=null`。PnP和多次采样也不是原子快照。

## 接回设备后的复测

1. 刷修订号14，网页确认版本；先用网页切到Xbox，重新授权该身份的配置HID。之后再单独测试左背+Steam组合，避免将两条路径混在一起。
2. 在Xbox模式运行上述探针，期间按A、移动左摇杆。核对IF0节点的兼容ID是否含`USB\MS_COMP_XUSB10`、service/INF及问题码；再看XInput槽是否成功读取并随操作变化。
3. 若仅配置HID存在，IF0无驱动/有问题码，先保存JSON定位枚举/驱动层；若已打开但IN完成不增长，结合generation、suspend、失败数和最近SETUP检查传输层。
4. 若XInputGetState有正确变化但Steam仍不显示，保留探针及Steam现象，这时继续检查Steam层；仅有网页Xbox标签或历史完成数不算通过。
5. 最后复测按键映射、左右板/摇杆并用、重插Type-C恢复Steam及原Steam功能。

## 本轮验证

- MSVC `/W4 /WX` + ASan原生回归通过；输入模式suite426条，原有协议/队列/时钟回归继续通过。
- Python16个测试、Node52个测试、Edge15个模拟HID流程通过。
- Windows无设备真实探针正常返回目标PnP空列表、四个XInput槽均1167；这是工具的无设备路径验证。
- ARM构建、描述符字段审计和UF2验证通过。修订号14 UF2 SHA-256：`fe7efafd94cd6512d9b5a5c369181290d8b6ff07dd3e1acdcb2ce9aa7c0d0432`。
- 应用`0x27000..0x405ff`，406个UF2块，207872字节；Flash103712、全局RAM30860。
- 当前未实物刷写修订号14，没有Windows目标驱动绑定、真实XInput输入或Steam识别成功记录。

## 规范出处

- [MS-XUSBI表46：XUSB接口描述符](https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-xusbi/7238785a-1c68-460c-8416-b62f2b0d4aaf)
- [MS-XUSBI表30：Get_Report(Input)](https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-xusbi/bb5bc203-fbbd-466c-a4af-442097ef2261)
- [Microsoft OS描述符及osvc缓存](https://learn.microsoft.com/en-us/windows-hardware/drivers/usbcon/microsoft-defined-usb-descriptors)
