# Xbox输入、开机组合键与网页映射（修订号12起）

修订号20新增Xbox右触控板滑动短脉冲反馈，默认沿用已捕获Steam右板参数，网页可保存强度或关闭。与游戏rumble独立管理，见 [TOUCH_FEEDBACK.md](TOUCH_FEEDBACK.md)。

修订号16起采用OpenPuck式“保存启动选择→detach→等待60ms→整机重启”，Xbox为`045e:028e`独立四接口布局，网页配置仅在Steam模式提供。保留修订号15的震动和双向开机组合，并增加记住模式及Guide专用诊断。详见 [MODE_REBOOT_AND_GUIDE.md](MODE_REBOOT_AND_GUIDE.md)。软件回归及ARM构建通过，实物行为仍待验证。

修订号17在此基础上增加右背＋Steam进入PS5（PC DualSense），共用下述映射与配置，详见 [PS5_MODE.md](PS5_MODE.md)。下文Xbox专有USB内容仍适用于Xbox模式。

修订号18新增左肩LB＋Steam进入 [Switch Pro](SWITCH_MODE.md)，PS5/Switch增加 [真实IMU转发](MOTION_INPUT.md)；Xbox无IMU字段，保留原输入/Guide/震动行为。

## 使用

1. 刷入 `build/xiao/SCReceiver-xiao-s140-7.3.0.uf2`；默认上电Steam，保留已有绑定及旧映射。启用“记住输入模式”后上电沿用最后选择。
2. **左背键＋Steam开机进入Xbox；A＋Steam开机切回Steam**（后者需要修订号15）。建立私有无线连接后继续同时保持约300ms，再将组合键全部松开。
3. 固件只在新会话开始的3秒窗口内接受组合。要求同一份新鲜、已认证输入中两键都按着；单键、提前松开、已使用很久之后按组合不触发。中途换组合重新计时；三键同时按住优先Steam。
4. 接收器先持久化启动选择，再断开USB、等待60ms并整体重启。RF协议重新初始化，手柄需恢复连接。切换后松开组合键，再单独按Steam作为Guide。
5. 默认一次性启动选择在启动时消费；正常启动完成后重插Type-C回到Steam。“记住输入模式”启用后则保持最后选择。切换保存后、消费前断电会在下次启动执行一次尚未消费的选择。
6. 手柄若处于BLE模式，先用A+Steam恢复私有接收器模式；本接收器不接收BLE手柄输入。

这里的“开机组合”是在新RF会话的早期输入里识别，不是新增了手柄原厂开机分支；连接窗口无法严格区分手柄冷启动和某些RF重连。已有绑定恢复后才允许组合触发，新配对待保存时不会触发。

## 默认Xbox映射

|物理输入|Xbox输出|
|---|---|
|左摇杆|左摇杆X/Y|
|左摇杆按压|L3|
|左触控板位置|按压时输出八方向十字键；中心死区内释放|
|左触控板按压|默认作为十字键触发条件，不额外映射按钮；松开即释放方向|
|右触控板位置|右摇杆X/Y；松开触摸回中|
|右触控板按压|R3|
|左背键|R3|
|右背键|L3|
|A/B/X/Y、LB/RB|同名按钮|
|左前/右前小键|Back/Start|
|Steam|Guide；开机切换组合期间被消费|
|LT/RT|模拟扳机；压到底的数字开关默认输出对应255值|

只有一个USB XInput gamepad。重启后选取第一个新鲜输入槽，在线期间其他槽不抢占，断开后才选择其他输入来源。250ms没有新鲜输入时输出中性状态；未更新的复用坐标最多保留200ms。

模式转换不会让SC2015凭空拥有第二个物理摇杆。独立坐标恢复、按压位与八方向判定见 [COORDINATE_DECODING.md](COORDINATE_DECODING.md)。

## 网页配置

“输入模式与Xbox按键配置”面板支持：

- 从Steam切到Xbox或PS5（17起），完整Feature回复消费后保存启动选择并重启。返回Steam使用A＋Steam开机组合。
- 16个物理按钮各自映射到无、A/B/X/Y、LB/RB、Back/Start/Guide、L3/R3、四个D-pad方向或LT/RT。
- 左摇杆、右触控板摇杆的软件死区；左板十字键径向阈值。
- 右板X/Y反转；左板默认按压后触发十字键，也可取消该选项改为触摸触发。
- 左板斜向区域角度0～75°，步长5°；新默认30°，旧配置保持45°。越小斜向越窄，0°关闭斜向；点击保存生效。
- 读取、填写默认值、保存并回读配置。填写默认值不自动写Flash。
- 修订号16新增“记住输入模式”，默认关闭，随配置保存；旧固件禁用该选项。

按键配置单独保存，Type-C重连后仍保留。两个物理键可映射到同一按钮；相反D-pad方向同时出现时输出中性方向。

“按压后才触发”是无已保存配置时的固件默认值，也是网页“填入默认配置”的值。若已有保存的触摸触发配置，在网页勾选该项并保存即可应用新选择。

修订号16的Xbox模式没有网页配置接口。需要修改配置或进入UF2时，先A＋Steam开机返回Steam，再连接网页。网页仍兼容旧修订号12–15的Xbox配置HID。操作在本网页串行化；另一进程修改配置后，旧token写入会被拒绝。

## USB布局与Windows驱动

|模式|设备身份|接口|
|---|---|---|
|Steam|`28de:1142`|原有IF0键鼠、IF1..4槽HID|
|Xbox|`045e:028e`|IF0游戏手柄；IF1音频、IF2附件、IF3安全布局占位；没有HID|
|PS5（17起）|`054c:0ce6`|IF0 DualSense游戏手柄HID，带IN/OUT，无配置HID|

Xbox设备class为`FF/FF/FF`，IF0为`FF/5D/01`，20字节输入，IN81轮询1ms/OUT02轮询8ms，软件提交约250Hz。布局与OpenPuck一致，附件端点不打开，也不实现XSM3认证。保留MS OS `MSFT100/XUSB10`兼容元数据；Windows绑定主要可走Xbox硬件ID。

修订号14的XUSB字段修正和Get_Report(Input report0)保留。当前Xbox `bcdDevice=0x0114`，不同于旧复合身份的0115。Xbox模式不能读FD，请用系统XInput探针检查状态及Guide。

旧复合Xbox身份`28de:1143`与额外配置HID在当前固件中已移除。新布局尚待真实Windows枚举验证，不能仅由描述符和构建通过推断驱动已正常运行。

每次应用启动只注册对应模式的接口：Steam五HID，Xbox四vendor接口。切换由整体重启完成，不在旧RF/USB运行状态中重建。

修订号15增加严格8字节Xbox rumble解码，转成有限的SC2015 `0x8F`脉冲，低频→左板、高频→右板，20Hz刷新、主机2.5秒无更新停止续发。取消只移除自生成且尚未加密的消息，不跳过CCM lane；实际停止尾延迟仍需实测。LED等其他OUT不执行Steam Feature命令。没有Xbox主机认证，完整行为见 [XINPUT_RUMBLE_AND_RECOVERY.md](XINPUT_RUMBLE_AND_RECOVERY.md)。

## 手柄初始化与持久化

- Xbox：向已绑定会话排入81清数字映射，以及87设置packet version49=2、左右触控板模式7/8=7，避免桌面鼠标/键盘行为。
- Steam新连接/恢复：85恢复数字映射、8E恢复默认设置，让接收器失电但手柄仍开机的场景能恢复。Steam随后可正常设置自己的配置。
- 两条初始化消息按整批容量检查入队，带会话generation校验；不覆盖host的本地回复、查询owner或Feature版本。
- 映射与启动选择共用`0xe9000/0xea000`两页、48字节版本2记录；能读取旧版本1。显式保存、切换及一次性选择消费会写入，默认无有效记录不自动擦写。布局见 [MODE_REBOOT_AND_GUIDE.md](MODE_REBOOT_AND_GUIDE.md)。
- 绑定页仍是 `0xeb000/0xec000`。链接ASSERT和UF2检查把应用上限收紧到 `0xe9000`，配置/绑定均不包含在应用UF2里。

## FD / FE协议v1

均为64字节Feature payload，不含hidapi额外的Report ID零字节。仅在已验证XIAO修订号>=12上使用；修订号16仅Steam模式提供这些配置HID。

### FD：模式

- 读取：`FD 00`。
- 切换：`FD 05 4D 4F 44 45 mode`（`MODE`加0=Steam/1=Xbox/2=PS5；模式2要求修订号17）。
- 回复：`FD 62 1 revision status current requested owner flags`；修订号12/13其余保留0，修订号14从offset9扩展端点诊断v1，布局见 [XINPUT_ENUMERATION_FIX.md](XINPUT_ENUMERATION_FIX.md)。此处62为十进制payload长度。
- owner为0..3或FF无来源；flags.bit0配置持久化可用、bit1忙、bit2默认上电Steam；修订号16新增bit3记住模式、bit4启动选择保存失败。
- SET处理只准备回复；完整GET的状态完成、且版本仍匹配时才提交切换请求。短GET、未完成GET、被覆盖的回复及USB reset取消的未完成请求不会触发切换。

### FE：映射

- 读取：`FE 00`。
- 写入：`FE 29 01 expectedCRC32LE wireConfig24`。
- 回复：offset0..5=`FE,62,1,revision,status,24`；8..11配置CRC32；12..35为wireConfig；其他保留0。
- wireConfig：byte0版本1；byte1低四位flags（bit0右X反转、bit1右Y反转、bit2左板需要按压、修订号16的bit3记住模式），高四位为修订号16斜向角度code，定义见 [MODE_REBOOT_AND_GUIDE.md](MODE_REBOOT_AND_GUIDE.md)；bytes2..17为16个映射目标；18..19左摇杆死区，20..21右板死区，22..23十字键阈值（均LE16）。
- 映射目标ID：0无、1A、2B、3X、4Y、5LB、6RB、7Back、8Start、9Guide、10L3、11R3、12上、13下、14左、15右、16LT、17RT。
- 物理输入顺序与网页一致：A/B/X/Y/LB/RB/Back/Start/Steam/左背/右背/摇杆按压/左板按压/右板按压/左扳机到底/右扳机到底。
- 两种回复status：0成功、1参数错误、2旧配置token、4忙、6保存失败。配置保存失败保持旧运行映射；相同配置不重复写Flash。FE失败通过自己的状态返回，不冒充旧F0绑定Flash计数。

## 验证范围

当前测试覆盖严格双键/超时窗口/时钟回卷、按住跨模式不重复、真实输入观察与USB合成释放隔离、左右坐标复用/反向同时操作/八方向/点击条件、断开与过期中性、配置破坏/截断/旧token/保存失败、短/中止GET、不随USB reset退模式、物理unplug回Steam、HID对象不累积，以及网页授权/保存/双向切换流程。

真实验收还需：Windows XInput/Guide状态、重启后RF恢复、Steam网页与纯Xbox的切换、一次性/记住模式上电策略、左右板/摇杆并用，以及原Steam配对/触觉回归。构建结果见 [VERIFICATION.md](VERIFICATION.md)。
