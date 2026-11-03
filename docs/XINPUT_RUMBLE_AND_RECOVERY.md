# 修订号15：Xbox震动、USB恢复与A＋Steam切回

本文记录修订号15的震动/恢复设计。修订号16保留震动转换并改为整机重启；当前17还增加PS5，共用同一脉冲转换。用法见 [MODE_REBOOT_AND_GUIDE.md](MODE_REBOOT_AND_GUIDE.md)和 [PS5_MODE.md](PS5_MODE.md)。

## 使用方式

- **左背键＋Steam开机**：恢复私有RF连接后继续保持约300ms，切到Xbox。
- **A＋Steam开机**：恢复私有RF连接后继续保持约300ms，切回Steam；无需拔接收器。
- 两者都限新RF会话前3秒，要求连续新鲜的认证输入，中途换组合重新计时。若A、左背、Steam同时按住，优先回Steam。
- 切换后松开组合键。只松其中一个不会把另一个残留按键立即送给游戏；同一会话不会反复触发。未保存的新配对不触发切换。
- A＋Steam本身也是官方手柄的私有接收器恢复组合。本工程增加的是连接后识别并切换接收器USB模式，不改手柄固件，也不接收BLE输入。

Xbox模式下，游戏的低频/高频震动分别转换到左/右触控板。SC2015没有Xbox式双ERM电机，采用触觉脉冲近似震动，实际强弱和手感需要实物校准。

## USB恢复与调度

### OUT重新安排

`xinput::service()`从loop重试失败的OUT接收，初次打开时和OUT完成后都保留重试状态。端点busy或claim失败时等下一轮；安排失败释放claim。未mounted、挂起、关闭或reset后不会继续旧重试。

与修订号14相比，初次OUT安排失败不再关闭已经成功打开的端点，IF0仍可继续输入，并由loop恢复OUT。这意味着FD的`opens/opened`现在表示端点成功打开，不能据此推断OUT已安排成功或Windows驱动绑定成功；失败仍计入`out_failed`。

### 发送优先级保护

`usb_tx_guard.h`在loop的USB发送窗口临时提升至`TASK_PRIO_HIGH`，退出恢复原优先级；嵌套调用保持平衡。覆盖HID发送、XInput IN和OUT重试，保持硬件中断开启，不把RF处理或Flash操作放入高优先级窗口。

依据是本工程锁定的nRF5x TinyUSB驱动在占用EasyDMA后、启动DMA前仍有可抢占窗口，而USB task高于loop。策略与OpenPuck的USB TX保护思路一致；原生测试只验证调用优先级和恢复，不模拟FreeRTOS抢占及真实DMA时序。

## 震动协议和队列

USB只接受成功完成的8字节Xbox OUT：

```text
00 08 00 low high 00 00 00
```

长度、类型或保留字段错误时不产生震动。LED及其他OUT不转成Steam Feature。USB callback只更新一个“最新电机状态”邮箱，同时捕获目标槽、RF会话generation、USB输出epoch和接收时钟。没有活动输入来源时丢弃，不能留给稍后连接的手柄。

loop将最新状态转换为SC2015的`0x8F`，每条10字节：

```text
8F 08 side on_usLE16 off_usLE16 countLE16 gain
```

- legacy side **1=左、0=右**，gain=0。
- 低频通道→左板：周期5000µs，脉冲宽度随0..255强度变化，最大1000µs，12次。
- 高频通道→右板：周期2500µs，最大宽度500µs，24次。
- 每组设计时长60ms，最大占空比20%；这组模拟参数是工程策略，不是Xbox原厂波形或已测量的SC2015力度曲线。
- 最多每50ms刷新一组；只在RF FIFO和两个已加密lane均空时入队。高频USB更新合并为最新状态，繁忙时略过刷新机会，不堆积一长串旧震动。
- 没有新的主机震动指令达到2.5秒后停止续发；游戏持续刷新指令可续期。输入来源超过250ms没有新鲜输入也撤销输出目标。
- 未序列化的自生成脉冲超过100ms不再发送。

**没有使用SC2的输出report `0x80`或Steam Deck的`0xEB`rumble格式。** Linux `hid-steam.c`虽然定义了EB，但其FF注册仅针对Deck/Ibex；SDL的SC2015驱动也未实现通用rumble。此处依据已有SC2015 8F实物转发记录与公开的脉冲字段定义实现转换。

## 停止与隔离

零电机值、USB reset/unplug/suspend、模式切换、RF失联/换会话会取消最新状态和未加密的自生成脉冲。仅删除带内部标记的消息，普通Steam 8F及其他命令不受取消操作影响。

已经进入lane的密文拥有CCM计数，不能丢弃或替换；它最多携带一组有限脉冲，由手柄自行播放结束。因此这里的“停止”是停止续发、撤销尚未加密的脉冲，**不是保证收到停止请求后60ms内物理静止**：已经加密但因无线延迟尚未送达的脉冲仍可能晚到。需要实测停止尾延迟，不能为缩短它而跳过CCM计数。

RF会话和USB输出epoch都匹配才接受邮箱状态，旧手柄的震动不会绑定到新手柄。内部pulse入队不覆盖主机回复或Feature版本。FC `admitted`包含内部生成的脉冲数；`haptic_admitted`仍只记录host Feature 8F，保持原有USB-haptic守恒关系。取消/过期清理计入`cleared_fragments`。

## 软件验证

- OUT初次/后续失败重试、claim释放、busy/挂起跳过、reset清理。
- TX窗口优先级提升、嵌套和返回恢复。
- Xbox OUT严格解码、非法/短/失败OUT、零值覆盖、无来源丢弃、RF/USB身份变化、挂起快速恢复后旧邮箱失效。
- 真实生产CCM路径中解密核对左右8F字段、严格下一counter、取消不修改已序列化lane、普通Steam脉冲隔离、过期清理、20Hz合并、2.5秒停止及微秒回卷。
- 完整软件集成：认证RF输入选择owner → Xbox OUT → loop转换 → RF密文解密得到8F；零值/输入超时取消；重新建立RF会话 → A＋Steam保持 → USB回到`28de:1142`。
- 两种组合的单键拒绝、三键优先级、变更组合重新计时、窗口过期、新配对拒绝和切换后松键隔离。

原生/ASan、16个Python、52个Node、15个Edge模拟HID流程及ARM构建通过。Windows只读探针本轮仍返回无目标PnP设备、四个XInput槽1167；未实物刷写，没有真实Windows识别、震感或DMA压力测试结果。当前UF2与计数见 [VERIFICATION.md](VERIFICATION.md)。

## 实物复测顺序

1. 更新固件和网页，确认修订号15，先网页切Xbox。运行 [Windows只读探针](XINPUT_ENUMERATION_FIX.md)，确认IF0绑定及按键/轴变化。
2. 游戏分别测试左、右、双通道及停止；退出游戏、USB挂起/恢复、关闭手柄和换手柄，记录停止尾延迟。需持续超过2.5秒的效果应由游戏刷新主机指令。
3. 接收器保持Xbox，关闭手柄，以A＋Steam重开，连接后继续保持约300ms，应重新枚举为Steam；松开两键，再确认原Steam输入和触觉。
4. 关闭手柄，以左背＋Steam重开，确认仍能进入Xbox。单按Steam、单按A、超过连接窗口再按组合不应切换。

## 依据

- 工程已有8F转发验证：[HAPTIC_FIX.md](HAPTIC_FIX.md)、[REV11_LIVE_VALIDATION.md](REV11_LIVE_VALIDATION.md)。
- [Linux hid-steam.c](https://github.com/torvalds/linux/blob/master/drivers/hid/hid-steam.c)：`steam_haptic_pulse`字段及legacy左右交换；FF注册的Deck/Ibex门槛。
- [SDL SC2015驱动](https://github.com/libsdl-org/SDL/blob/main/src/joystick/hidapi/SDL_hidapi_steam.c)：原始按键位和当前不提供通用rumble的实现。
- 本地OpenPuck `0114623971dc`：`mode_xinput.cpp`的8字节Xbox输出解析、OUT重试，以及`usb_tx.cpp`的发送优先级保护。
