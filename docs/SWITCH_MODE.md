# 修订号18：左肩LB＋Steam进入Switch Pro输入

## 使用

1. 更新至修订号18，使用已绑定的2015版Steam Controller私有无线连接。
2. **左肩键LB＋Steam开机**，无线恢复后保持约300ms，再完全松开组合键。
3. 接收器保存启动选择、detach、等待至少60ms后整体重启，以`057e:2009`的USB Pro Controller启动。等待手柄恢复无线连接。
4. PC端使用支持Switch Pro的Steam/SDL输入；Switch主机复测时开启“设置→手柄与感应器→Pro手柄的有线连接”。**本轮尚未实测PC或Switch主机识别。**
5. **A＋Steam开机返回Steam**，然后连接网页调整配置。左背＋Steam仍为Xbox，右背＋Steam仍为PS5。

组合限新RF会话前3秒，A恢复优先；多个模式选择键同时按住且无A时不切换。默认一次性启动消费后，下次正常上电回Steam；保存“记住输入模式”后保持最后选择。保存后、首次消费前断电会在下次启动执行那次选择。重启流程及Flash失败处理沿用 [MODE_REBOOT_AND_GUIDE.md](MODE_REBOOT_AND_GUIDE.md)。

Switch模式没有网页配置HID。网页18.2通过Steam配置接口选择Switch，并在发送前重读FD，要求固件>=18。模式值3沿用FD/FE v1、24字节映射和48字节记录v2；旧固件降级后可能不接受含模式3的启动记录。

## 共享映射

复用Xbox/PS5的16键映射、死区、右板反转、左板按压条件与斜向区域；按**物理位置**转换面键：

|物理输入（默认）|Switch输出|
|---|---|
|A / B / X / Y|B / A / Y / X|
|LB / RB|L / R|
|LT / RT|数字ZL / ZR；映射后0..255值达到40触发|
|Back / Start|− / +|
|Steam|Home；需先完全松开开机组合|
|物理左摇杆及点击|左摇杆 / L3|
|左触控板|按压时八方向十字键，沿用斜向角度|
|右触控板及点击|右摇杆 / R3|
|左背 / 右背|R3 / L3|

没有独立Capture映射。摇杆12bit中心2048、范围±1800，与返回的factory校准一致；无来源或失联250ms中性。一个USB手柄绑定一个输入owner，其他槽不抢占。

## 协议实现

- 单HID，bcdDevice=`0220`，IN/OUT最大64字节、端点轮询1ms。Input ID`30/21/81`及Output ID`01/10/80/82`均声明63字节payload，报告ID另计。
- `80`握手1/2/3回复`81`；4/5接受不回复，6重置USB协议状态。稳定MAC由FICR生成，与RF身份/密钥无关。
- 子命令`01/02`注册/信息、`03`选择`30`完整输入或0停止、`04`触发时间占位、`06/08/38`电源/LED no-op、`30/31`player lights、`40`IMU开关、`41`固定灵敏度兼容ACK、`48`震动开关。
- `10`读取factory IMU/摇杆校准、参数和颜色，单次最多29字节。`11`仅写`8000..80ff`的256字节RAM用户校准镜像，最大29字节，重启丢失；不能访问接收器Flash。USB bus reset保留该RAM镜像。
- `21`仅NFC配置兼容ACK，`82`不生成NFC数据；未实现NFC/IR、Bluetooth配对或`3f`简单输入模式。不支持的子命令NACK。
- 回复队列8项，满时需回复的整条请求丢弃且不改状态；发送失败保留队首，成功后才退休；USB epoch阻止旧ticket操作新队列。
- EP0只处理完整OUTPUT：核对IF0、报告ID、实际长度、ID前缀及子命令参数，DATA/ACK失败不执行。interrupt/control的ID包装分别处理。

主机选择`30`后开始输入；未启用IMU时应用最短4ms一包，启用后15ms一包（约66.7Hz）。IMU在payload12起为三组accel6＋gyro6；三组均保持最近有效样本，**并非三次独立5ms采样**。源数据轮转、坐标转换与超时规则见 [MOTION_INPUT.md](MOTION_INPUT.md)。电池/连接状态为虚拟占位，不是手柄电量遥测。

## 震动

解析每侧HD rumble的absolute/relative/hold编码，取幅度峰值，再复用SC2015有限`8F`脉冲：左/右分别模拟两通道，20Hz续发、单组60ms、主机2.5秒无刷新停止。未复现HD rumble精细频率波形，实际手感待校准。

停止、挂起、reset及owner变化只取消未加密内部脉冲；已序列化CCM lane保持正常交付。详细队列边界见 [XINPUT_RUMBLE_AND_RECOVERY.md](XINPUT_RUMBLE_AND_RECOVERY.md)。

## 验证

- 825条Switch专项断言：握手、注册、SPI范围/校准、RAM写入、满队列/失败发送/reset、面键/方向/轴、组合与启动记录。
- HD rumble全部128个absolute幅度码与独立`exp2` oracle对照，另测relative/hold/停止编码。
- 500条共享IMU专项断言及生产CCM集成：LB组合→保存/重启→USB握手→输入/Home/三组IMU→双通道8F→A返回Steam。
- ARM编译后报告ID/位数审计、Python/Node/Edge模拟回归；当前结果与UF2身份见 [VERIFICATION.md](VERIFICATION.md)。

参考本地OpenPuck `0114623971dc` 的`mode_switch_pro.cpp`，以及SDL `f7c5d8e1305fa90dbb169ce58da8220c1e8fdba4`的[Switch HIDAPI驱动](https://github.com/libsdl-org/SDL/blob/f7c5d8e1305fa90dbb169ce58da8220c1e8fdba4/src/joystick/hidapi/SDL_hidapi_switch.c)。没有移植SC2无线协议或专属IMU校正。
