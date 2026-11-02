# 本次实现验证记录

## 已执行

```powershell
python tools/dependencies.py
python tools/test.py --sanitize
python tools/build.py
npm test
npm run test:browser
```

- Windows MSVC 14.51，`/W4 /WX`；AddressSanitizer已启用并通过。
- **74,339条断言**，108组独立PyCryptodome CCM向量、128组冻结跳频向量，以及4个Python镜像测试。
- 生产协议和USB桥代码直接参加测试；AES block oracle为Windows BCrypt，独立完整CCM oracle为PyCryptodome。
- 模拟手柄覆盖新配对、初始counter=1密钥更新、普通输入、认证重复、双lane ACK、Feature 27+27+10切片/回复、存储失败后提交、断开及恢复绑定。
- ASan曾发现watchdog轮转到phase6/7时会越过四个slot，现已限制到phase2..5；完整测试随后通过。
- ARM GCC 9.2.1、Seeed nRF52 1.1.13、TinyUSB 3.6.0实际编译通过，无编译警告。
- 新增F0 v2槽身份、冻结绑定快照、分阶段整库导入、取消/超时、旧token、重复ID、缺失记录和Flash失败回滚测试通过。
- 新增原固件S1请求ACK低位1/3/5/7、跳频不打断RX/ACK、批量join/rekey、bootstrap ACK重发交界测试通过。
- 应用使用 **79,828字节Flash，26,736字节全局RAM**。
- 编译后的RADIO/TIMER3向量确实绑定到强符号handler；两份HID report descriptor与原接收器字节一致。
- UF2起点 `0x27000`，当前末端 `0x3a800`；312块，159,744字节。绑定页、SoftDevice、bootloader及UICR不在UF2内。
- 当前UF2 SHA-256：`ea44e445ba6e362ef56f8912c52c796ba4f6aa83954c39c0394a9285662cfa6c`，固件修订号3、F0诊断协议v3。
- **17个Node网页协议/状态测试通过**：report ID边界、DataView偏移、超时串行化、原版仅A1、四槽身份、自动保存作用域、dump页选择/去重、提交失败/回读、F7实际ACK进度解析。
- **8个真实Edge headless浏览器流程通过**：模拟HID连接/自动保存、输入与拔出、诊断和绑定下载、原版身份＋dump迁移、JSON拒绝与连接门控、旧固件提示、不支持WebHID及390px布局，以及v3/F7显示与导出。

可机读结果：`build/tests/manifest.json`、`build/tests/protocol_test.log`、`build/xiao/manifest.json`、`build/xiao/compile.log`。

编译器`.su`也保留在`build/cache/sketch`：RADIO处理体56字节stack、跳频处理体32字节；Flash load/save和setup路径有较大的局部对象，未将数据库或密码学工作移到ISR。`.su`是静态单函数用量，不是实测整条调用链的水位。

## 未执行

本次没有刷写XIAO、打开物理HID接口、连接原手柄或采集RF/逻辑分析仪轨迹。浏览器测试注入模拟HID peer，未验证原版A1读取或实物迁移。因此尚不能宣称：

- 在真实设备完成了首次发现、MIC互通或配对。
- 130µs ACK参数、所有切频/恢复边界已实测匹配。
- Steam/SDL已成功识别输入、四手柄并发、触觉/LED及断电恢复均已验证。

主机fixture使用合成材料；测试日志不应当作设备抓包证据。物理调试入口是`receiverctl.py status/capture`及其CRC、MIC、ACK逾期和队列计数。
