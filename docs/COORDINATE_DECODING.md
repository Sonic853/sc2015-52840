# SC2015左摇杆与左触控板坐标如何分离

## 截图说明

用户提供的Steam测试界面同时显示左板 `(-23592,-5913)`、左摇杆 `(32320,-5835)`。这说明应用维护了两组独立的逻辑状态，并不说明每一无线包必定同时包含两对坐标。

图中的LG/RG是左右背键，不是触控板点击。蓝色点能显示触摸位置，单凭该图不能确定左板按压位为1。用户随后确认原版接收器无线连接下也显示 `28de:1106`：这与此前私有接收器83属性回复声明PID4358（0x1106）一致，见 [实际USB对照](USB_DIFFERENCES_AND_QUEUE_REVIEW.md)。这是Steam显示的手柄产品身份，不能当作接收器USB PID或蓝牙专属标记。

Steam客户端具体版本的内部实现没有在本轮反编译。可核查依据是公开SDL驱动中的Valve协议处理、Linux驱动，以及本地官方主控固件的输入组装函数；三者的私有无线复用机制相符。

## 标准报告：同一坐标槽，两个来源

以下偏移针对64字节Steam输入报告，不含额外hidapi Report ID前缀：

|字段|含义|
|---|---|
|16..17、18..19|共享的有符号LE16 X/Y|
|byte10.bit3，按钮mask `0x00080000`|本包共享坐标来源：1左板，0左摇杆|
|byte10.bit7，mask `0x00800000`|左板和摇杆都活动，需要保留另一来源的上次值|
|byte10.bit1，mask `0x00020000`|左板点击位，含下述旧版兼容别名|
|byte10.bit2，mask `0x00040000`|右板点击位|
|byte10.bit6，mask `0x00400000`|左摇杆点击位|

当两者同时活动，byte10.bit3会随包改变，不能把它简单解释成“手指刚离开左板”；实际左板活动应结合bit3或bit7。

处理规则：

```text
本包来自左板：更新 leftPad；both=1则保留 leftStick，否则将 leftStick 回中
本包来自摇杆：更新 leftStick；both=1则保留 leftPad，否则将 leftPad 释放
```

因此“左板按在左侧、左摇杆推向右侧”可以是：

```text
包A：pad=1,both=1，共享X=-20000 → leftPad.X=-20000
包B：pad=0,both=1，共享X=+20000 → leftStick.X=+20000
合并状态：D-pad Left按下，同时LeftStick.X为正
```

不要把两个X平均成0，也不要每包同时覆盖两种输入。接收器按槽分别缓存，依据每包标志更新，不能假设收到的包一定一左板一摇杆交替（可能丢包或重复）。

本地BLE host镜像 `0x8608`构造标准状态时，`0x8834`附近检查物理摇杆活动，双方活动则设置 `0x800000`，并用报告序号奇偶决定本轮坐标来源；`0x8898`附近按来源取左板数据，随后写共享字段及 `0x80000`标志。不是在接收器猜方向。该函数的已保存指令与原BIN已在前次 `boot_chord_verification.json`中核对，语义仍是静态分析。

公开SDL的 `FormatStatePacketUntilGyro()`明确维护 `sPrevLeftPad[2]`、`sPrevLeftStick[2]`，将它们还原到 `sLeftPadX/Y`与`sLeftStickX/Y`；其内部状态就是可以同时显示两组坐标的形式。Steam界面可能还应用校准、旋转等处理，所以截图数值不当作原始USB字节。

## 独立字段确实存在，但不能混用传输格式

- USB直连报告文档的54..57有未校准摇杆值，58..61有左板值；Linux明确将它们标作无线不发送。
- 本接收器恢复的正常无线状态只有42字节，复制至USB[8:50]，后面清零；不能从54..61读出第二套有效坐标。
- BLE分块格式另有 `k_EBLELeftJoystickChunk=0x80`与`k_EBLELeftTrackpadChunk=0x100`，这是另一条协议分支，并不代表当前私有无线格式。
- `SETTING_ENABLE_RAW_JOYSTICK=46`不是“增加一对无线字段”的证据。本地 `0xc4dc` 中该设置控制摇杆处理/死区相关路径，不能靠设置名称推断报文扩展。

## 按压与八方向

标准报告中每块触控板各一个按压位，共两个；方向来自X/Y。固件另可报告四个逻辑D-pad方向位，但它们仍是处理后的方向，不是触控板有八个机械开关。

修订号12用径向中心死区和八个约45°方向区间生成XInput十字键。对角线由两个相邻方向位同时置位；相反方向冲突输出中性。默认要求左板按压，松开按压即释放方向；网页可取消该条件改为触摸触发。

截图的左板向量距正左约14°，位于正左方向区间，输出左而非左下；左摇杆仍独立保持正X、负Y。按压只决定是否允许十字键输出，不参与摇杆X/Y判定。

旧版固件在左板未活动时可能把摇杆点击镜像到左板点击位。转换层在 `pad=0,both=0`时将该位归一化为摇杆点击，避免用户把左板点击映射到其他按钮后，按摇杆却多触发一个按钮。

## 参考

- [SDL Steam HIDAPI驱动](https://github.com/libsdl-org/SDL/blob/main/src/joystick/hidapi/SDL_hidapi_steam.c)：`FormatStatePacketUntilGyro`、`UpdateBLESteamControllerState`。
- [SDL Valve报告结构](https://github.com/libsdl-org/SDL/blob/main/src/joystick/hidapi/steam/controller_structs.h)：标准状态、BLE字段块。
- [Linux hid-steam](https://github.com/torvalds/linux/blob/master/drivers/hid/hid-steam.c)：标准报告偏移表与 `steam_do_input_event`。
- 本地 `analysis/host_analysis/exports/ble_final_decompiled.c` 的 `0x8608`、`0x8c10`、`0xc4dc`；目标SHA-256见 [可行性分析](INPUT_MODE_FEASIBILITY.md)。
- 新实现 `SCReceiver/input_mode.cpp` 与 `tests/input_mode_test.cpp`。反向同时操作、按压/释放、左右缓存、八方向均有软件回归，实物手感仍需验证。
