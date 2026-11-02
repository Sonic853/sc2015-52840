# XIAO本地绑定备份/迁移扩展

本协议是本工程新实现的USB Feature扩展，**不是**原版2015 Valve接收器命令，也不是二代OpenPuck A2/A3。位于vendor HID interface1..4，report ID为0，64字节数据：`command, argument_length, arguments...`。WebHID使用`sendFeatureReport(0, payload64)`，不在payload前再加0。

## F0诊断v2

|偏移|内容|
|---|---|
|0..4|F0、参数长62、版本2、active pipe mask、pairing bool|
|5..32|七个应用/认证/存储uint32计数，LE|
|33|逻辑槽0..3|
|34|槽总数4|
|35..58|六个RADIO/USB uint32计数，LE|
|59..63|当前归一化RF身份5字节|

命令格式、原诊断计数位置保持不变；命令行工具仍接受旧v1，网页需要v2来区分槽位。

## F1–F6共同约定

- 所有响应 `[0]=command,[1]=payload_length,[2]=status`。未完成USB回显的length=0不能当作成功响应。
- status：0成功；1参数错误；2事务失效；3仍有无线连接；4其他导入进行中；5记录不完整；6Flash失败；7重复手柄ID。
- token为32位LE、非0。事务跨四个USB槽全局唯一。
- 事务30秒未服务自动释放；导入超时或取消恢复之前的配对状态，不修改持久数据库。
- 导入期间关闭RF接入；开始和提交都检查无线连接，避免修改活动会话身份。

### F1：建立只读快照

请求参数长度0。响应payload长13：

```text
2 status
3 transfer_protocol=1
4..7 token LE32
8..12 receiver_address5
13 used_bond_count
14 capacity=20
```

后续读取来自冻结快照，不受同时写入原数据库影响。新快照可取代旧快照并使旧token失效；不能覆盖正在进行的导入。

### F2：读取快照记录

请求：`token4 | index1`，长5，index0..19。响应payload长31：

```text
2 status
3 index
4 used (0/1)
5..12 controller_id8
13..28 key16
29..32 token LE32
```

unused条目的id/key返回零。客户端读取20个位置并检查used数量、索引、token和重复ID。

### F3：开始整库暂存

请求：`normalized_address5 | expected_record_count1`，长6，count0..20。首字节低3位必须0。响应与F1相同，count为预期条数。

此命令清空**RAM暂存库**，不清持久数据；导入是替换库，不是逐条向旧库追加。F4使用紧凑索引0..count-1。

### F4：写入一条暂存记录

请求：`token4 | index1 | controller_id8 | key16`，长29。响应payload长6：`status, index, token4`。重复发送相同位置不会增加完成位；不同位置不能有相同controller ID。

### F5：一次性提交

请求token4，长4。要求全部expected位置已收到、没有活动手柄。使用已有双页Flash存储写入身份和完整库；失败保留旧库与暂存供重试。响应payload长5：`status, token4`。

成功后清暂存、更换RAM库、刷新接入挑战；身份改变时主循环重新初始化RADIO地址与跳频表、清旧RX队列，不更换USB描述符或断开WebHID。当前已提交token的重复F5可返回成功而不重复写Flash；新事务使旧token失效。

### F6：释放/取消

请求token4，长4；响应payload长5：`status, token4`。释放导出快照，或取消尚未提交的导入。取消不撤销已经提交的库。

## 前端提交判定

导入成功不仅依据F5的status0：网页再通过F1/F2回读，比较归一化身份与按controller ID排序的全部id/key。任何缺失、重复、旧token或回读不一致都报错，不显示迁移完成。

标准B0仍用于单条id8/key16写入，不改变接收器身份。若要保留手柄原有绑定，使用完整迁移协议。原版journal离线解析规范及证据另见 [WEB_CONSOLE.md](WEB_CONSOLE.md)。
