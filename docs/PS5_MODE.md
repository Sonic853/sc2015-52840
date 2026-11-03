# 修订号17：右背＋Steam进入PS5输入

## 使用与范围

本模式参考OpenPuck的DualSense USB输入及公开主机驱动，目标为**PC上的PS5/DualSense输入**。OpenPuck对应模式也标注PC only；这里没有PS5主机认证或实物主机兼容结论。

1. 接收器接在PC上，手柄已有本接收器绑定。
2. **按住右背键＋Steam键开机**，无线连接后继续同时保持约300ms，再松开组合键。
3. 接收器保存启动选择、detach、等待至少60ms后重启，以`054c:0ce6`启动，等待手柄恢复无线连接。
4. **A＋Steam开机返回Steam**，左背＋Steam仍进入Xbox。组合只在新RF会话前3秒识别；A返回优先，左右背键同时按住且无A时不选择模式。
5. 默认一次性选择在启动时消费，正常启动完成后重新上电回Steam；启用“记住输入模式”后可保持PS5。

Steam网页新增“PS5 / DualSense（PC）”选项，仅修订号17及更新可用。PS5模式本身没有网页配置通道；需要调整映射、斜向角度或进入UF2时先返回Steam。

## 共用映射

PS5与Xbox使用**同一份16键映射、左右死区、轴反转、按压条件和斜向角度**，不另建配置。输入先经过现有映射，再转换成DualSense字段：

|物理输入（默认）|PS5输出|
|---|---|
|A / B / X / Y|× / ○ / □ / △|
|LB / RB|L1 / R1|
|LT / RT|L2 / R2模拟量，带对应数字按压位|
|左前小键 / 右前小键|Create / Options|
|Steam|PS键；开机组合完全松开后再单独按|
|左摇杆及点击|左摇杆 / L3|
|左触控板|按压时十字键；沿用可调斜向区域|
|右触控板及点击|右摇杆 / R3；离开触摸回中|
|左背键 / 右背键|R3 / L3|

自定义配置中A/B/X/Y、Back/Start/Guide等目标按同样对应关系转换。右板Y会按DualSense坐标约定转换为向下为正，保留用户选择的反转效果。左板仍是D-pad，不会再同时发送PS5触摸联系人；右板也是摇杆而非第二个触摸输出。

一个USB虚拟手柄选择一个新鲜RF输入来源，其他在线槽不抢占。失联250ms中性、复用坐标200ms过期规则与Xbox一致。

## USB报告

独立单HID接口，VID/PID=`054c:0ce6`，bcdUSB=`0200`，bcdDevice=`0110`，IN/OUT端点最大64字节、轮询间隔1ms；应用提交约250Hz。使用现有HID对象，不累积额外实例。

- Input ID1：63字节payload＋1字节报告ID，总64字节。
- Output ID2：声明47字节公共体，支持SDL的48字节总长度、Linux的63字节总长度以及最多64字节的零填充形式。
- Feature ID3：47字节payload能力摘要。
- Feature ID5：40字节payload中性校准，分母非零。
- Feature ID9：19字节payload，包含由板卡FICR派生的稳定本地管理地址。
- Feature ID20：63字节payload兼容身份，版本0224用于主机震动格式选择，不是接收器固件修订号。

Input字段以payload起点计：0..3双摇杆，4..5扳机，6序号，7方向帽/面键，8肩键/数字扳机/Create/Options/L3/R3，9.bit0为PS键。方向帽0..7按顺时针从上开始，8为中性。触摸联系人固定inactive，IMU固定中性，USB电池/连接状态为占位，未提供实际电量或动态传感器。自适应扳机、音频、灯效及完整DualSense功能不在本轮实现范围。

这不是OpenPuck的多手柄动态挂载或双板触摸模式：按本工程当前Xbox使用方式提供单手柄、共用映射及静态USB布局。

## 震动与控制传输

Output ID2公共体中right/high位于2、left/low位于3；兼容振动flag0.bit0或新格式flag2.bit2有效时更新电机状态。支持SDL的全零停止及切回音频触觉时停止续发。单独LED/自适应扳机写入不覆盖震动状态。

USB侧只接受完整47字节公共体与可选零填充；错误ID、短公共体或非法尾部不生成脉冲。EP0还验证请求接口、报告类型/ID、实际DATA长度和报告ID前缀，只有完整ACK才调用输出处理，避免短包或中止操作使用旧缓冲。interrupt OUT包含报告ID，control SET_REPORT已由TinyUSB去掉ID，两种形式分别处理。

XInput和PS5共用线程间最新状态邮箱与SC2015 `0x8F`转换策略：收包时锁定RF槽/会话/USB epoch、20Hz限频、2.5秒无主机刷新停止续发，停止/挂起/reset/换会话取消未加密脉冲，已加密lane不跳过CCM计数。详见 [XINPUT_RUMBLE_AND_RECOVERY.md](XINPUT_RUMBLE_AND_RECOVERY.md)。

## 启动与配置兼容

模式枚举新增2=PS5（0=Steam，1=Xbox）。FD v1及48字节配置记录v2不变，启动记录的last/once允许2；读取旧配置保留原映射。网页及CLI仅在固件>=17时接受PS5模式，旧固件不会收到PS5切换写入。保存过PS5启动状态后，降级到不认识模式2的旧固件可能使旧固件将该记录视作无效。

## 验证与复测

已通过：

- PS5原生suite116条：面键/肩键/PS键、轴端点/Y方向、全部方向帽、共享斜向配置、中性输入、组合优先级/窗口/回卷、一次性/持久化PS5。
- Feature长度/短读/MAC稳定、48/63/64字节OUTPUT、旧/新振动标志、全零停止、错误/短/失败/中止EP0、reset与目标切换隔离。
- 认证RF链路集成：右背组合→保存/重启→PS5身份→Cross及PS键输入→主机震动→解密得到左右8F→A组合返回Steam。
- 19个Python、57个Node、17个Edge模拟HID流程；ARM构建及编译后HID report ID/位数审计。

本机PnP枚举未发现接收器或DualSense目标，**尚未实物刷写和验证识别/震感**。接回后先在Steam页面确认17，再分别测试网页切换与右背开机组合，核对PC显示DualSense、所有映射、PS键、停止震动及A组合返回。XInput探针不适合验证DualSense HID输入，应使用Steam手柄测试或支持DualSense的SDL工具。

当前UF2/hash见 [VERIFICATION.md](VERIFICATION.md)。

## 依据

- 本地OpenPuck `0114623971dc`：`mode_ps5.cpp`的USB身份、报告字段、Feature及OUT约定；README注明PC only。
- [SDL PS5 HIDAPI驱动](https://github.com/libsdl-org/SDL/blob/main/src/joystick/hidapi/SDL_hidapi_ps5.c)：64字节输入、Feature探测、振动标志及48字节USB输出。
- [Linux hid-playstation](https://github.com/torvalds/linux/blob/master/drivers/hid/hid-playstation.c)：DualSense字段、校准格式和63字节填充输出。
