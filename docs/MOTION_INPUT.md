# 修订号18：SC2015真实陀螺仪与加速度转发

PS5和Switch模式现在从SC2015私有无线输入提取**原始三轴gyro与accel**，转换到对应主机报告。源码为`motion.{h,cpp}`；Steam原生报告保持既有转发，Xbox XInput没有IMU字段。

## 数据来源与启用

每个已保存的RF会话在PS5/Switch模式初始化时，先清除lizard映射，再设置packet version 2、两块触控板raw模式和setting48（`IMU_MODE`）=`0x18`，开启raw accel与raw gyro。下发设置体为：

```text
87 0c 31 02 00 07 07 00 08 07 00 30 18 00
```

Xbox同一设置的IMU值为0；Steam连接恢复原厂默认配置。设置整批入队且不覆盖主机Feature回复；它证明发出了命令，**尚无实物setting读回或真实sensor速率观察**。

标准64字节输入`01 00 01 3c ...`中，accel为LE int16位于28/30/32，gyro位于34/36/38。压缩RF输入每包只能携带选中的后缀，不能将每次普通按键包当作新的sensor采样：

|原厂轮转掩码|suffix控制|长度|标准输入目的偏移|
|---|---|---|---|
|4（quaternion，本模式不启用）|16|8|40|
|8（accel）|4|6|28|
|16（gyro）|10|6|34|

已核对原厂静态字节：

- host BLE镜像`vcf_wired_controller_d0g_5b0f21bd.bin`，SHA-256 `2c42adb211c9f209dae5ac223ed451229d0aa08b139acfbd56f36b0d8add6fc4`。
- `0x8608`组装标准输入，`0xc774`按setting48轮转，`0xcc70/0xcc7c`取长度/控制，`0xa114`发送后缀；表`0x10b40`为`040008001000100804060a06`，共核对798条指令字节。
- production `d0g_module.bin`，SHA-256 `42b8fe7067d570dd49bce72952afa6c68ad56786468b1bbfcf02c2540daa2161`；加载base`0x400`，`0x2314`压缩输入时装入完整后缀，核对71条指令字节。
- 工作区分析输出：`analysis/host_analysis/imu_transport_verification.json`，验证器`analysis/host_analysis/scripts/verify_imu_transport.py`。这些在Receiver仓库之外，是保存反汇编与原BIN的逐字节核对，不是运行时执行证明。

## 新鲜度与隔离

仅**新认证输入**的合法完整6字节向量更新对应分组时间戳；加速度和角速度独立计时。重复包、MIC失败、普通按键包、特殊透传报告和不完整向量不续期。完整补丁跨两组时可同时更新；clear位先清历史，再接受该包完整组。

每组超过100ms没有完整更新便输出零。USB reset/resume、模式切换、配置变更、RF断连及generation变化清理相应历史。USB合成中性输入不会充当开机组合的物理松键。只使用当前新鲜owner的sensor，开机组合抑制期间不输出。

## 单位与轴

按SDL SC2015约定，源gyro为`raw * 2000 / 32768 deg/s`，源accel为`raw / 16384 g`。源转SDL公共XYZ：gyro=`(gx,gz,+gy)`，accel=`(ax,az,-ay)`。SC2/Triton的gyro第三轴是`-gy`，因此不能直接复制OpenPuck的SC2转换。

|目标原始报告|gyro XYZ|accel XYZ|
|---|---|---|
|DualSense|`(gx,gz,gy)`|`round((ax,az,-ay)/2)`|
|Switch Pro|`round((-gy,-gx,gz)*557125/638976)`|`round((ay,-ax,az)/4)`|

计算用int64中间值、对称四舍五入和最终int16饱和，包含`-32768`输入边界。

PS5 Feature5返回gyro零bias、plus/minus=`±8192`、speedPlus/speedMinus均500，主机比例为`1000/16384 deg/s/count`；accel校准为`±8192`，对应8192count/g。输入payload15..20为gyro，21..26为accel，27..30为接收器微秒时钟×3。应用提交最短4ms一包，timestamp是接收器时钟，**不是原始sensor采样时间**。

Switch factory IMU校准gyro零bias/sensitivity13371、accel origin0/sensitivity16384；上述转换匹配SDL的`936/13371 deg/s/count`和4096count/g。启用IMU后最短15ms一包，payload12起三组12字节重复最近有效值（零阶保持）。这不恢复原厂传感器采样历史，也不声称200Hz独立无线采样。子命令41接受固定范围配置，未实现动态量程/滤波选择。

## 验证与实物待测

500条独立IMU断言覆盖分组100ms边界、时钟回卷、Clear、无效header、不同owner/generation、配置/模式变化、全部signed极值及主机物理单位oracle；oracle使用实际PS5 Feature5与Switch SPI6020校准回复。另在生产CCM→解压→Router→生产USB回调（Mock TinyUSB）路径验证PS5/Switch字段。新增USB重置回归先复现旧sensor重发，清历史后通过。

fixture accel=`(16384,0,-8192)`、gyro=`(1000,-2000,3000)`：PS5输出accel=`(8192,-4096,0)`、gyro=`(1000,3000,-2000)`；Switch输出accel=`(0,-4096,-2048)`、gyro=`(1744,-872,2616)`。这些是合成输入，不是实物测量。

实物验收需要：六面静置确认重力轴/1g；各轴正反转确认符号；已知角速度核对灵敏度；静置测gyro偏置；暂停输入验证归零；重连/USB重置后确认不重发旧运动。尚未测量原始bias、各手柄固件差异、无线采样率或主机融合效果；没有额外加SC2 trims或姿态融合算法。

来源：

- [SDL SC2015驱动](https://github.com/libsdl-org/SDL/blob/f7c5d8e1305fa90dbb169ce58da8220c1e8fdba4/src/joystick/hidapi/SDL_hidapi_steam.c)：原始sensor单位和轴转换。
- [SDL PS5驱动](https://github.com/libsdl-org/SDL/blob/f7c5d8e1305fa90dbb169ce58da8220c1e8fdba4/src/joystick/hidapi/SDL_hidapi_ps5.c)与[SDL Switch驱动](https://github.com/libsdl-org/SDL/blob/f7c5d8e1305fa90dbb169ce58da8220c1e8fdba4/src/joystick/hidapi/SDL_hidapi_switch.c)：目标字段、校准与公共轴。
- [Linux hid-steam](https://github.com/torvalds/linux/blob/05441d697f7f9a927264f4058cbbc6a03dfcac9d/drivers/hid/hid-steam.c)：标准输入偏移及轴；其整数16count/dps近似未用于本转换。
