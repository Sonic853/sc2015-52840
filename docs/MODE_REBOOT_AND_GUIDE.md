# 修订号16：OpenPuck式重启切换与Xbox Guide诊断

本文记录修订号16流程。当前修订号17在相同启动策略上新增右背＋Steam进入PS5、模式值2及共享映射，见 [PS5_MODE.md](PS5_MODE.md)；当前镜像和验证计数以 [VERIFICATION.md](VERIFICATION.md)为准。

## 切换流程

当前流程为：

```text
完整FD回复被消费，或认证开机组合成立
→ 保存最后模式 / 一次性启动选择并核验Flash
→ USB detach
→ 等待至少60ms
→ 暂停RADIO，NVIC_SystemReset()
→ setup重新加载配置和绑定，消费一次性选择
→ 初始化对应USB布局和无线协议，等待手柄恢复连接
```

这与本地OpenPuck `0114623971dc` 的 `modeSwitchReboot()` / `saveMode()`生命周期一致。旧的“同一进程重建USB并保留RF会话”已移除。仍保留2015版的左背＋Steam/A＋Steam开机组合，不移植SC2无线协议或四背键组合。

### 启动策略

- **记住输入模式默认关闭**：切换时保存一次性目标；新应用启动时先将该选择持久化清除，再开启USB。正常启动完成后再次上电默认Steam。
- **启用记住模式**：每次切换保存最后选择，下一次上电沿用它。回Steam后可以在网页关闭该选项。
- 在Steam网页“Xbox模拟按键与触控板”中勾选“记住输入模式”，点击“保存按键配置”；填写默认配置会取消勾选，保存后生效。
- 与OpenPuck的一次性`bootMode`相同，若在保存选择后、首次启动消费前断电，该请求仍会在下一次上电执行一次；消费完成后才恢复默认上电Steam规则。
- 普通USB总线reset不等于整机重启，也不改变当前模式。

模式请求不在SET/GET中断回调中写Flash。FD短读、中止、旧版本消费都不会保存启动选择或重启。保存失败保留当前模式和USB连接，不在loop持续重试；重新连接网页可看到“启动选择保存失败”。FD确认表示请求已接受，不能冒充随后的Flash写入成功。一次性选择消费失败时不开启目标USB，进入已有初始化失败指示，避免每次启动反复重放未消费选择。

## 持久化记录

沿用映射双页`0xe9000/0xea000`，每页一个48字节记录、generation、CRC和最后提交字；绑定页不变。记录版本升至2：

|偏移|内容|
|---|---|
|0..3|`SCXM`|
|4..7|版本2，LE32；仍能读取原版本1|
|8..11|generation，LE32|
|12..35|原24字节映射wire；flags.bit3新增记住输入模式|
|36|最后模式，0=Steam/1=Xbox|
|37|一次性模式，0/1或FF无选择|
|38..39|保留0|
|40..43|前40字节CRC32|
|44..47|最后写入的提交字0|

版本1升级保留映射，记住模式默认关闭；只在显式保存、切换或消费一次性选择时写新记录。降级到不认识版本2的旧固件会把这些配置视作无效，需重新配置映射。

FD/FE仍是wire版本1，Steam配置接口的固件门槛更新如下：

- FD offset8 bit0配置可保存、bit1忙、bit2默认上电回Steam；修订号16新增bit3记住模式、bit4最近启动选择保存失败。
- FE配置flags的bit3为记住模式，其余映射字段及CRC token不变。新网页对旧固件禁用该选项。
- FE配置flags的bit4..7编码斜向区域角度，见下节；低四位互不干扰，wire长度仍为24字节。
- 模式切换只更新启动记录，不改映射token；完整映射保存也保留当前启动策略的一致性。

## 左触控板斜向范围

在Steam模式连接新版网页，展开“Xbox模拟按键与触控板”，调整“左板斜向区域角度”滑块，点击保存，再切入Xbox使用。可选0～75°，步长5°：

|斜向角度|每个斜方向区域|每个上下左右区域|
|---|---|---|
|0°|关闭|90°|
|15°|15°|75°|
|30°（新默认）|30°|60°|
|45°（旧版）|45°|45°|

这是角度范围，不是中心死区。缩小该值会扩大正上下左右区域，减少斜向误触；径向阈值及按压条件独立。四个象限对称，0°时正好落在45°交界的点选择水平轴。

为兼容旧24字节配置，flags高四位code对应：`[45,5,10,15,20,25,30,35,40,0,50,55,60,65,70,75]`度。code0保留旧45°，code6为30°，code9为关闭斜向。旧记录升级保持45°；没有记录时默认30°。新固件默认flags为`0x64`（左板需按压＋30°）；不与记住模式bit3冲突。

使用整数定点比较`tan((90-angle)/2)`，不会因触摸半径改变扇区宽度。回归独立用角度计算四象限边界两侧、中心/八向、0°和所有编码，并验证网页保存读回及重启保留。

## Xbox独立USB布局

Xbox模式采用OpenPuck的Xbox 360硬件ID和四个vendor接口布局：

|项目|修订号16|
|---|---|
|VID/PID|`045e:028e`|
|device class/subclass/protocol|`ff/ff/ff`|
|bcdUSB / bcdDevice|`0200 / 0114`|
|配置长度|153字节，含9字节配置头及144字节接口体|
|IF0|`ff/5d/01`，IN81/OUT02，20字节输入|
|IF1|`ff/5d/03`，音频附件占位|
|IF2|`ff/5d/02`，扩展附件占位|
|IF3|`ff/fd/13`，安全接口布局占位|
|端点轮询间隔|IN 1ms / OUT 8ms；软件输入提交仍最多约250Hz|

附件端点不打开；没有音频、chatpad或XSM3认证实现，目标仍是PC。厂商/产品字符串标明SC2015，无USB序列号；并非完整仿制OpenPuck全部身份和认证。保留MS OS `MSFT100/XUSB10`兼容元数据，但主机可直接按Xbox硬件ID绑定。布局来源的MIT声明保存在`SCReceiver/xinput_descriptors.h`。

**Xbox模式没有键鼠HID、配置HID、WebUSB或CDC。** 网页断开是预期行为，不能通过重新授权找回配置接口。按A＋Steam开机返回Steam后再配置/进入UF2；默认未记住模式时，正常启动完成后重插也会回Steam。Steam模式保留`28de:1142`的原五HID接口。

FD诊断不再能在修订号16的Xbox模式读取。网页保留对旧修订号12–15复合布局的兼容；当前Xbox需用系统XInput探针观察。

## Steam键 / Xbox Guide

默认映射为：SC2015原始按钮`0x002000` → 配置目标Guide（编号9）→ Xbox按钮`0x0400`。20字节Xbox输入应在按下时以`00 14 00 04`开头，松开后按钮位清零。

新增回归验证了：

1. 单独Steam键只设置Guide位；
2. 开机组合抑制结束后再次单独按Steam仍能设置Guide；
3. 从加密RF输入经生产observer/router/pump到模拟USB IN，按下输出`0x0400`、松开清零。

这些软件测试没有复现映射缺失，因此没有把Guide改成其他按键。现场仍需排除：

- 开机组合尚未完全松开；先松开两键，再单独按Steam。
- 已保存映射把Steam设为“无”或其他目标；回Steam模式在网页确认该项为Guide。
- 普通`XInputGetState`不暴露Guide。诊断脚本现在额外读取系统XInput 1.4/1.3的**未公开ordinal100（GetStateEx）**；SDL也采用这一读取路径。接口不可用或返回错误时显示未知，而不是“未按下”。
- 系统/Steam对Guide的响应配置与收到按键是不同层；没有打开菜单不等于没有收到Guide。

在Xbox模式执行下面的只读采样，期间单独按住Steam约1秒再松开：

```powershell
python tools/diagnose_xinput.py --samples 60 --interval 0.1 --output build/xinput-guide.json
```

查看对应槽的`guide.result`与`guide.pressed`。result=0且pressed随按下/松开变化，说明该XInput槽能读取Guide；即使普通`gamepad.buttons`没有0400，也不能据此判为失败。全局槽可能属于其他手柄，需结合ABXY/摇杆变化确认；工具不自动认定槽归属。

PnP筛选已加入`045e:028e`。当前Xbox不要传`--path`，因为不存在vendor配置HID；该参数只用于Steam或旧复合Xbox固件。探针不改驱动、注册表或输入状态。

## 验证与边界

- MSVC/ASan原生回归通过，包含持久化先于detach、59999µs不reset/60000µs一次reset、失败保存不detach、短/中止GET不保存、一次性消费/记住模式/v1升级，以及重启后重新建立RF连接。
- 重启和Flash掉电场景是回调/记录模型；不执行真实NVMC、电源切断、bootloader或FreeRTOS时序。
- 17个Python、55个Node、16个Edge模拟HID流程通过；ARM构建和描述符审计通过。
- Windows探针成功加载GetStateEx，但本机仍无目标PnP设备，所有槽均1167。**没有实物Guide按键、Windows新身份绑定、重启后RF恢复或震动成功记录。**
- 固件身份及当前hash见 [VERIFICATION.md](VERIFICATION.md)。

## 参考

- 本地OpenPuck `0114623971dc`：`OpenPuck.ino::modeSwitchReboot`、`config.cpp::saveMode/loadCfg`、`mode_xinput.cpp`和`xinput_descriptors.h`。
- [SDL的Windows XInput加载实现](https://github.com/libsdl-org/SDL/blob/main/src/core/windows/SDL_xinput.c)：ordinal100的Guide扩展读取及9.1.0回退限制。
