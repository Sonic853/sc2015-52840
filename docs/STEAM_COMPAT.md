# 修订号6：Steam能配对，却无法列出手柄

后续用户反馈和修订号6日志已确认Steam能读取详情、识别手柄并接收输入。当前修订号7继续处理 [重复触觉写入被误去重](HAPTIC_FIX.md)，本页保留初始化修复依据。

## 日志将失败定位到了哪里

用户确认：网页可以显示连接与按键输入，Steam可以识别接收器并配对，但控制器列表为空。`sc2015-diagnostics (3).json`为修订号5，activePipes=6、一个非bootstrap绑定、H=2恢复接受1次，活动槽state2、5064个USB输入，四槽均无查询错误。

用户提供的`controller.txt`显示：

1. `28de:1142`的interface1–4均被Steam打开，使用V1 HID协议。
2. 收到`wireless_pairing`、`wireless_established`和真实输入流。
3. 随后出现`Controller Info Msg garbled`、`Read failure`、`Controller Serial# invalid`。
4. `CGetControllerInfoWorkItem`反复失败，最后将设备当作zombie移除。

因此，USB枚举与配对通知已经进入Steam，失败集中在控制器详情/字符串的异步Feature事务。没有依据改成普通Game Pad HID或伪造属性/序列号。

日志须按HID路径分段：失败段interface1包含实例后缀`619329d`，02:59/03:08完整成功段则是`3a67dbf0`。路径变化本身不能唯一标识物理设备，但这些成功不能直接当作同一XIAO的成功证据。失败路径曾读到有效HWID/构建时间后又在字符串阶段失败，表明问题不只是读取按钮输入。

累计173次MIC失败不能单独证明仍在持续失步；F0与F7是分开采样，5908与5910也不能直接判为丢包。此次修复不基于这些计数推断PHY故障。

## 原版的关键SET忙语义

依据已有正确8051架构的独立Ghidra汇编导出，**非REA Evidence**：

- `CODE:1951..19b0`处理`82/83/84/89/8a/8b/8c/aa/ab`。
- 未连接或本槽回复已缓存：`1ad5..1ae2`返回R7=5，完成SET状态阶段。
- 在线且回复未到：排无线请求、记录pending，`19ae`返回R7=0；相同pending查询不重复排队。
- `3265..32a9`把R7=0变为`C7B4=11h`（STALL），把5变为`C7B4=02h`（释放状态阶段）。排队成功不等于USB SET成功。
- GET `296c..2a41`仍可返回64字节`83 00`占位。这是提前GET的退路，不是主机判定无线查询就绪的方法。

SDL公开Steam Controller HID实现也在SET失败时重复同一请求，一旦成功就转入GET；它按命令号匹配回复，不能依赖客户端自动拒绝所有同命令零长度占位。

修订号5的问题是：TinyUSB在SET ACK回调中只复制命令，USB操作先成功，主循环才进行无线请求。主机随后可能收到同命令但空内容的成功GET，正好绕过旧客户端的忙重试。

## 修订号6的实现

1. **在TinyUSB DATA阶段决定读取查询SET的结果。** 首次只复制到命令环，返回false形成STALL；相同请求重试不重复排无线片。主循环发布真实回复后，SET重试成功，ACK不再清空或重排缓存。
2. 保持上述原版读取命令族；AE字符串和BA芯片读取也采用相同就绪门控，属于本工程的读取兼容扩展。USB就绪门控不作用于写设置、触觉、AD、B3。修订号6协议层仍错误地对重复写入去重，该后续问题已在修订号7修正。
3. 本地命令尚未执行/发布时GET返回待重试失败，不把未发布的占位当有效状态。已离线的查询在本地处理后正常返回空离线回复。
4. 完整GET的消费入队时立即使匹配的USB发布版本及就绪确认失效。`pending_gets`阻止主循环在实际消费前重新发布旧缓存；新查询清除旧Ready状态。覆盖GET完成→publish→同命令SET、以及process后publish前的竞态。
5. 为在途完整GET预留消费环记录；队列已满时GET失败重试。协议层消费继续校验版本，不清除更新的回复。
6. 跟踪实际OUT长度，拒绝不完整头/旧尾部和失败OUT。长度0/1的SET在SETUP即拒绝，避免零长度控制传输跳过DATA后重放旧命令。
7. 网页能有界重试读取查询的NetworkError和未发布GET；不自动重试有副作用的写命令，不吞权限错误或真正挂起的调用。
8. 新版网页常规刷新使用F0/F8全槽mask，优先从空闲接口查询，不再每秒向所有接口发B4重复连接事件。首次授权、主动控制与旧固件退路仍可能使用B4；不能保证任意两个客户端同时操作一个Feature接口都互不干扰。

协议/AES/Flash仍在主循环；三个link wrapper只复制、校验并使用已发布状态。ARM构建检查TinyUSB类表实际指向wrapper，不仅检查符号存在。

## FA主机链路诊断v1

F7修订号>=6声明支持；请求`fa 00`补零到64字节，Report ID单独传0：

|偏移|内容|
|---|---|
|0..3|FA、payload长62、版本1、固件修订号|
|4..6|最近非诊断SET的slot、command、实际字节数|
|7..9|最近非诊断GET的slot、command、payload长度|
|10..11|保留0|
|12..39|七个LE32计数：sets、query_busy、query_ready、gets、unpublished_gets、invalid_sets、command_drops|
|40..59|每槽5字节：当前query command、请求总长度、query status、已发布reply command、reply payload length|
|60|已连接槽mask|
|61|待处理SET槽mask|
|62|保留0|
|63|重复固件修订号|

query status：0无、1等待无线、2回复就绪、3无线离线。last命令排除A1/B4及F0以上本地命令，避免重新授权读取身份/状态时覆盖最近无线查询线索；它不能辨别具体主机进程。`invalid_sets`统计DATA阶段的长度/结果拒绝；SETUP拒绝不包含在其中。

`query_busy`增加是正常的SET待就绪重试，不等同于射频丢包。需结合就绪次数、GET命令/长度、队列丢弃及Steam同一时间段日志看。FA没有Feature参数正文、绑定key、手柄序列号或chip ID正文。网页JSON字段`hostLink`，CLI `status`也会读取FA。

## 验证与实物步骤

软件回归直接运行生产协议/USB wrapper，模拟传统HID的属性→字符串→设置事务，覆盖50次pending SET不重复转发、真实分片回复、离线查询、立即GET、消费发布竞态、短/零长度OUT及写命令不等待回复。27个Node测试、12个Edge模拟HID流程和ARM构建通过；数字与UF2 hash见 [VERIFICATION.md](VERIFICATION.md)。

原生控制传输shim按锁定TinyUSB的SETUP/DATA/ACK顺序驱动，但不运行真实USB硬件或FreeRTOS调度。nRF5x DCD在安排状态阶段时合成完成事件，不提供独立的物理状态ACK中断；因此不能把模拟测试的“无完成回调不消费”扩展成所有物理中止保证。

1. 刷入当前UF2（修订号7，包含本页修复），并更新网页。已有绑定先保留，普通按Steam开机，无需先清库或重新配对。
2. 只连接目标XIAO，先在网页点“断开网页”，退出其他HID调试工具。
3. 完全退出并重新启动Steam，普通Steam键开机，检查设置→控制器是否持续列出手柄并能测试输入。
4. 如果失败，先保存该次`controller.txt/controller_ui.txt`；退出Steam并保持接收器供电，再打开新版网页下载含`hostLink`的诊断JSON。采集前后注明当地时间。
5. 同时观察有无新的USB重新枚举；日志中的`hid_read failure`可能是拔插、重启或真正USB故障，仅凭文字不能判定其原因。

用户已确认修订号6的Steam识别和输入；本代理未操作物理接收器。日志仍有断开/重新初始化及DongleBugWorkaround超时，详情就绪修复不等于已验证所有USB和连接边界。

公开host参考：<https://github.com/libsdl-org/SDL/blob/main/src/joystick/hidapi/SDL_hidapi_steam.c>、<https://github.com/libsdl-org/SDL/blob/main/src/joystick/hidapi/steam/controller_constants.h>。它们佐证传统host实现，不代表当前闭源Steam内部代码已被完整恢复。
