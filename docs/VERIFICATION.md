# 本次实现验证记录

## 已执行

```powershell
python tools/dependencies.py
python tools/test.py --sanitize
python tools/build.py
npm test
npm run test:browser
python tools/verify.py
```

- Windows MSVC 14.51，`/W4 /WX`；AddressSanitizer已启用并通过。
- **87,602条断言**，108组独立PyCryptodome CCM向量、128组冻结跳频向量，以及4个Python镜像测试。
- 生产协议和USB桥代码直接参加测试；AES block oracle为Windows BCrypt，独立完整CCM oracle为PyCryptodome。
- 模拟手柄覆盖新配对、初始counter=1密钥更新、普通输入、认证重复、双lane ACK、Feature 27+27+10切片/回复、存储失败后提交、断开及恢复绑定。
- ASan曾发现watchdog轮转到phase6/7时会越过四个slot，现已限制到phase2..5；完整测试随后通过。
- ARM GCC 9.2.1、Seeed nRF52 1.1.13、TinyUSB 3.6.0实际编译通过，无编译警告。
- 新增F0 v2槽身份、冻结绑定快照、分阶段整库导入、取消/超时、旧token、重复ID、缺失记录和Flash失败回滚测试通过。
- 新增原固件S1请求ACK低位1/3/5/7、跳频不打断RX/ACK、批量join/rekey、bootstrap ACK重发交界测试通过。
- 修订号4新增连接矩阵通过：无H=0缓存的持久H=2恢复、最终key延后提交、本地无主机自动保存、断电/快速重启、四槽满及最后join重发、所有在线槽03广播、历史RX/watchdog顺序、错key/id/地址/挑战拒绝，以及20条绑定满库/已有id更新。
- 修订号5新增活动无线回复与本地B4/A1/F0/F7/F8/F1缓存隔离、占位GET请求去重、同命令不同参数、B2关闭后回显、离线B3状态，以及F9 magic/import/过期版本/一次性请求覆盖。
- 生产USB桥覆盖F9确认、短GET、GET SETUP后中止、EP0状态完成后延期消费。构建另核实状态完成钩子为强符号，且核心UF2入口实际链接。
- 应用使用 **84,548字节Flash，28,428字节全局RAM**。
- 编译后的RADIO/TIMER3向量确实绑定到强符号handler；两份HID report descriptor与原接收器字节一致。
- UF2起点 `0x27000`，当前末端 `0x3bb00`；331块，169,472字节。绑定页、SoftDevice、bootloader及UICR不在UF2内。
- 当前UF2 SHA-256：`68bfb5b8c43da1455fbd21af11c52ab33dab2cee6892fc45b7c051fe3ecc8633`，固件修订号5、F0诊断协议v3。
- **23个Node网页协议/状态测试通过**：新增回复不匹配保留状态及恢复、USB挂起隔离/显式重开、目标UF2命令、旧版和导入busy拒绝。
- **11个真实Edge headless浏览器流程通过**：新增修订号5按钮及F9参数、释放全部目标接口但保留原版身份来源、查询失败保留无线状态及下次刷新恢复。

可机读结果：`build/tests/manifest.json`、`build/tests/protocol_test.log`、`build/xiao/manifest.json`、`build/xiao/compile.log`。

编译器`.su`也保留在`build/cache/sketch`：RADIO处理体56字节stack、跳频处理体32字节；Flash load/save和setup路径有较大的局部对象，未将数据库或密码学工作移到ISR。`.su`是静态单函数用量，不是实测整条调用链的水位。

## 未执行

用户已经反馈修订号3首次配对成功；最新修订号4实物报告显示一个持久绑定、H=2接受4次、activePipes=6，确认恢复接入成功，也暴露活动槽Feature查询失败。本代理没有刷写XIAO、打开物理HID接口或采集RF/逻辑分析仪轨迹；浏览器测试注入模拟HID peer。因此尚不能宣称：

- 修订号5的Feature修复或网页UF2入口已经在物理XIAO通过。
- 130µs ACK参数、所有切频/恢复边界已实测匹配。
- Steam/SDL已成功识别输入、四手柄并发、触觉/LED及断电恢复均已验证。

主机fixture使用合成材料；测试日志不应当作设备抓包证据。物理调试入口是`receiverctl.py status/capture`及其CRC、MIC、ACK逾期和队列计数。
