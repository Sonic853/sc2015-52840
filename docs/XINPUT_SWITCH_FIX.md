# 修订号13：Xbox切换被软件断开事件撤销

本文保存修订号13的历史修复与产物记录。同名UF2现已更新为修订号16，改用整机重启切换，见 [MODE_REBOOT_AND_GUIDE.md](MODE_REBOOT_AND_GUIDE.md)；当前hash见 [VERIFICATION.md](VERIFICATION.md)。

用户报告网页请求Xbox后仍是Steam输入。本轮在修订号12代码中找到可复现原因：**把TinyUSB主动detach产生的UNPLUGGED也当作Type-C物理拔出，覆盖了已经确认的Xbox目标。**

## 根因

锁定依赖TinyUSB3.6.0的 `src/portable/nordic/nrf5x/dcd_nrf5x.c:273–280`：

```c
void dcd_disconnect(uint8_t rhport) {
  NRF_USBD->USBPULLUP = 0;
  dcd_event_bus_signal(0, DCD_EVENT_UNPLUGGED, false);
}
```

驱动注释明确说明：关闭D+/D−上拉不会改变USB供电状态，所以它主动提交一个逻辑UNPLUGGED事件。真正的USB供电移除也会提交同一事件（该文件1036–1057行），仅事件名称不能区分两者。

修订号12的路径：

1. FD切换请求和完整GET消费成功，`requested_mode = Xbox`。
2. `serviceMode()`为重新枚举调用 `TinyUSBDevice.detach()`。
3. 事件hook无条件将UNPLUGGED标成 `physical_unplug`。
4. 下一轮 `serviceMode()`按“拔线恢复Steam”规则把目标改成Steam。
5. 等待结束后重建的是Steam描述符，所以仍是 `28de:1142`。

重新构建描述符的 `begin()`还会再调用一次detach，两处都需要正确处理。问题发生在固件模式切换阶段，不能靠重新选择网页下拉框或修改按键映射解决。

## 修复

在事件hook中读取nRF52840的 `NRF_POWER->USBREGSTATUS`：

- **VBUS仍在**：逻辑断开，保留目标模式；仍执行原有USB队列和输入清理。
- **VBUS消失**：锁存物理移除，恢复Steam。

判定在事件到达时完成，避免快速重插后loop再读取VBUS而漏掉移除。也没有依靠 `in_isr`推断来源，因为物理移除的处理上下文可能不同。普通USB bus reset继续保持当前模式。

## 回归

之前的USB mock只统计detach次数，没有提交该驱动事件，因此没有覆盖这个失败路径。本轮让输入模式测试启用nRF5x的逻辑断开行为：

- **先在未修复生产代码上失败**：`tests/input_mode_test.cpp:190: usb::inputMode() == Mode::Xbox`。
- 修复后网页请求路径完成Xbox重枚举，并连续运行后续loop而不退回Steam。
- 两次软件detach均不会改写目标，HID实例数仍为5。
- 主机bus reset保持Xbox。
- 真实移除在事件时VBUS=0，即使loop处理前已快速回到1，仍恢复Steam。
- 切换中真实拔线优先于已确认的Xbox请求；中断/非中断上下文均覆盖。
- Xbox→Steam显式请求同样通过两次软件断开。

本轮MSVC/ASan完整原生回归、11个Python测试及ARM构建/二进制审计通过；输入模式suite为359条断言。网页协议未改变，FD/FE v1及修订号>=12门槛保持兼容。

## 产物与复测

固件：`build/xiao/SCReceiver-xiao-s140-7.3.0.uf2`，**修订号13**。

```text
SHA-256: 000981341c4c98f449bf83a4d3a2dd33730ee3072bbb91be445cfa3ebeefac5a
```

刷入后确认网页修订号13，再请求Xbox并授权重新枚举后的配置接口。成功切换应由FD回读为Xbox、接收器USB身份为 `28de:1143`；回到Steam则为 `28de:1142`。Steam界面里的手柄产品标识 `1106`不能代替这项判断。

本轮本机HID/PnP枚举未找到目标设备，**尚未完成修订号13实物刷写或Windows XInput驱动验证**。已修复并验证的是上述固件撤销路径；USB驱动加载及实际输入仍需接入设备复测。
