# WebHID 网页控制台与 GitHub Pages

入口源码：[`../web/index.html`](../web/index.html)。纯静态HTML/CSS/ES modules，不需要后台、生产npm依赖或GitHub Token。接收器通过USB连接到**运行浏览器的这台电脑**，数据通过WebHID在本地交换。

## 当前网页19.0与Lizard

固件修订号19新增“Lizard 键盘/鼠标”选项，切换前实时读取FD并拒绝旧固件的模式4写入。它使用固定桌面键位，与游戏手柄映射表分开；“记住输入模式”对Lizard同样有效。模式下仅枚举键盘/鼠标，无配置HID，使用A＋Steam开机返回Steam。左背＋右背＋Steam开机也可进入，完整键位见 [LIZARD_MODE.md](LIZARD_MODE.md)。

当前页脚为“网页版本19.0”/“Console 19.0”，所有静态资源统一`?v=19.0`。本轮63个Node测试与25个Edge流程通过，包括旧固件拒绝、Lizard启动策略、中文/英文说明以及返回Steam配置；下面18.3/18.2段落记录相应功能引入时的行为。

## 中英文切换（网页18.3）

页眉“语言 / Language”可随时选择中文或English。首次打开按浏览器首选语言决定：`en`及其地区变体（如`en-US`、`en-GB`）使用英文；中文及其他未提供翻译的语言统一使用中文。

手动选择会保存到当前站点的`localStorage`（`sc2015.language`），下次打开优先使用；没有手动选择时不保存自动检测结果。存储被浏览器禁用时仍可当次切换，下次重新检测浏览器语言。

静态说明、按钮、辅助标签、动态状态、诊断、错误和操作记录均切换；保留当前HID连接、选中的目标/模式及未保存映射。仅翻译显示文本，不改协议数据、设备标识、原始报文或导出JSON内容。页脚确认18.3；HTML与依赖模块统一更新缓存版本。

## 部署到 github.io

将 `Receiver/` 作为GitHub仓库根目录：

1. 上传工程源码，包括 `web/` 和 `.github/workflows/pages.yml`；构建目录、工具链和node_modules已在.gitignore中排除。
2. 仓库 **Settings → Pages → Build and deployment → Source** 选择 **GitHub Actions**。
3. 推送到main/master，或在Actions手动运行 **Receiver console pages**。
4. 打开部署任务给出的URL，通常是 `https://<用户名>.github.io/<仓库名>/`。

workflow只上传`web/`。脚本/样式采用相对路径，支持仓库子路径；`.nojekyll`保留静态文件。没有在本次任务创建远程仓库、提交或推送；部署URL由你的仓库决定。

## 本地打开

从Receiver目录运行：

```powershell
python -m http.server 8000 --bind 127.0.0.1 --directory web
```

打开 `http://localhost:8000/`。桌面版Chrome/Edge支持WebHID；直接双击`file://`、普通远程HTTP或不支持WebHID的浏览器不能完成设备通信。

## 配对和诊断

1. 刷入本工程更新后的 `build/xiao/SCReceiver-xiao-s140-7.3.0.uf2`。当前版本是修订号18、F0诊断协议v3、FC队列协议v1、FD–FE输入配置v1；当前配置页面需Steam模式，网页也兼容旧v2。
2. 点击“连接 / 授权接口”，选择接收器vendor HID接口。若只授权部分槽，再次授权其他接口。
3. 选择目标XIAO，点击“开启配对”；按住X+Steam启动2015版手柄的私有无线配对。
4. 新连接显示“绑定待保存”。勾选自动保存时，本页只保存**自己开启配对后出现的新待保存槽**；也可以按槽点击“保存绑定”。
5. 观察CRC、认证、MIC失败、ACK逾期、队列和Flash计数。普通输入显示序号、活动时间和原始64字节报告。

空库启动或实体D0启动的本地配对由固件在最终key确认后自动保存，不依赖本页；上述checkbox仅控制“由本页AD开启”的配对。普通恢复只需Steam开机，无需X+Steam。

四个槽号从F0显式读取，不使用WebHID枚举顺序。接收器RF身份区分多台XIAO；原接收器另列为身份来源。未授权接口与无线断开是不同状态。

v3还会读取F7。展开“配对链路细节”可见固件修订号、pipe0..5收包、实际ACK安排/TX READY/完成、最近S1和软件H。配对失败复测应使用该版本；详见 [PAIRING_FIX.md](PAIRING_FIX.md)。

修订号4另读F8，展开“绑定与重连”可区分空库、查询丢失、错误密钥、最终key尚未确认、保存失败和未保存断开。保存按钮只发送一次B3，随后等待可能延后的提交完成。连接矩阵见 [CONNECTION_FIX.md](CONNECTION_FIX.md)。

修订号6另读FA，展开“Steam主机通信”显示无线查询的SET忙重试/就绪确认、未发布GET和最近主机命令号，不包含Feature参数、密钥或序列号正文。新版常规刷新从F0活动pipe和F8 pending mask还原所有槽状态，优先选择空闲接口；不再定时发送四个B4并制造重复连接通知。缺F8的旧固件仍用B4。Steam识别异常的证据和复测步骤见 [STEAM_COMPAT.md](STEAM_COMPAT.md)。

修订号7保持FA v1格式，修复无线触觉等重复写入被误去重；现有修订号6网页也可读取它。FA最近SET为`0x8f`不代表手柄已执行触觉；对照和复测见 [HAPTIC_FIX.md](HAPTIC_FIX.md)。

修订号8补齐原版Feature回退、USB/RF容量处理和输入复位，保持上述诊断格式；`command_drops`也包含容量不足而拒绝的USB请求。移除额外BOS/WebUSB能力不影响网页的标准WebHID接口。差异与实物复测见 [COMMUNICATION_AUDIT.md](COMMUNICATION_AUDIT.md)。

修订号9修复96主机熵设置被误拒绝的回归，FA v1的保留字节增加最近拒绝命令、声明长度、槽和原因；新版网页展示并导出`hostLink.lastRejected`，旧固件/无记录为null。旧网页仍可读取既有FA字段。现场日志和修复依据见 [ENTROPY_FIX.md](ENTROPY_FIX.md)。

修订号10另读FB并显示“无线会话”面板，导出`transportLink`：看门狗、主机B2、同ID新会话替换、换钥加密失败和清库计数；在线槽显示实时lane/phase/队列状态，离线槽保留断开前快照。`ack_deferred`是等待认证相位的反馈ACK数，不是发射失败。旧固件不查询FB。布局与复测见 [RECONNECT_LOOP_FIX.md](RECONNECT_LOOP_FIX.md)。

修订号11新增“冻结队列诊断 · FC”。常规轮询只读页0摘要，显示按类分账、USB/RF高水位和最大年龄；点击“读取冻结队列快照”或“下载诊断JSON”才读取四槽详情及最近16项事件。所有页校验相同ID和MCU采样时钟，token失效时整组重试一次。完整结果独立保存为 `queueSnapshot`，带主机 `capturedAt`；常规摘要为 `diagnostics.queueDiagnostics`，后续轮询不会把旧完整快照伪装成新结果。

导出采集失败会附 `queueSnapshotError` 并保留旧快照原时间。只有本网页请求被串行化，另一网页/CLI的FC页0仍可使本次token失效。FC与F0/FA/FB不是同一次冻结；触觉超时是RF队满的子集，分片退休不等于手柄实际执行。字段及计数口径见 [QUEUE_IMPROVEMENTS.md](QUEUE_IMPROVEMENTS.md)。

操作记录最多100条，页面隐藏时暂停定时查询。Feature操作串行，处理未完成/旧回复，单次USB调用有超时。回复不匹配保留槽、最近无线状态和输入监听，下次刷新重试；真正挂起的USB接口暂停查询，可通过“连接 / 授权接口”或“读取已授权设备”重新打开。诊断JSON记录`queryError/queryClosed`。断开网页关闭HID句柄，物理拔出移除相应槽。

Steam或命令行工具也可能访问同一个Feature接口。诊断时可先退出它们，避免交错请求；页面只能串行化自己的请求。

## Steam / Xbox / PS5 / Switch输入与按键配置

修订号17新增“PS5 / DualSense（PC）”，18新增“Switch Pro（USB）”。右背＋Steam或左肩LB＋Steam开机分别进入PS5/Switch；配置与Xbox共用。PS5的A/B/X/Y、Back/Start/Guide对应×/○/□/△、Create/Options/PS；Switch按物理位置对应B/A/Y/X、−/+/Home。两者均无网页配置接口，用A＋Steam返回；详见 [PS5_MODE.md](PS5_MODE.md)、[SWITCH_MODE.md](SWITCH_MODE.md)。修订号18增加两种模式的真实gyro/accel转发，见 [MOTION_INPUT.md](MOTION_INPUT.md)。

**网页18.2起修复PS5选项被旧版本缓存锁灰**：不再按缓存修订号禁用单个选项，点击切换时重新读取FD；实际固件不足17/18仍会明确拒绝，且不发送模式写入。整个切换控件要求健康Steam配置接口，版本/异常原因显示在选项下方。当前页脚为“网页版本18.3”或“Console 18.3”，HTML、CSS及整个module依赖图统一`?v=18.3`以更新缓存。若仍显示旧页脚，重新部署`web/`并刷新；本地修改不等于已发布到Pages。

修订号16的配置页面仅在Steam模式可用（`28de:1142`、IF1..4）。Xbox改为`045e:028e`四个vendor接口，没有WebHID/WebUSB。页面仍支持旧修订号12–15的`28de:1143`复合Xbox配置通道。

选择Xbox并点击“切换输入模式”。完整回复消费后，固件保存启动选择、detach、等待60ms并整体重启，RF重新连接；网页关闭旧句柄。默认一次性选择消费后，下次上电Steam；勾选并保存“记住输入模式”则保持最后选择。Xbox期间网页断开是预期行为，需要A＋Steam开机返回Steam后再配置。

修订号15也支持 **A＋Steam开机切回Steam**，左背＋Steam开机进入Xbox。新RF会话前3秒内同时保持约300ms后松开，三键同时按住优先Steam。Xbox震动通过左右触控板短脉冲模拟；主机2.5秒无新指令会停止续发，行为与复测见 [XINPUT_RUMBLE_AND_RECOVERY.md](XINPUT_RUMBLE_AND_RECOVERY.md)。

若修订号12接受请求后仍回到Steam，请升级至当前固件：修订号13已修复将软件detach误当成Type-C拔出的问题，见 [XINPUT_SWITCH_FIX.md](XINPUT_SWITCH_FIX.md)。实际模式以重新连接后的FD回读为准。

旧修订号14/15的Xbox配置HID可显示端点计数；修订号16起Xbox没有该接口，应运行Windows只读探针读取XInput及Guide扩展状态，见 [MODE_REBOOT_AND_GUIDE.md](MODE_REBOOT_AND_GUIDE.md)。若启动选择写入失败而没有重启，重新连接Steam网页可看到失败提示。

展开配置可编辑16个物理键、左右软件死区、左板八方向阈值、右板轴反转，以及左板是否需要按压才触发十字键。“填入默认配置”仅修改表单；“保存按键配置”写入独立配置页并校验回读。默认左背→R3、右背→L3、右板点击→R3，左板方向与物理摇杆分别解码。常规诊断轮询不会覆盖未保存的表单。

修订号16新增“左板斜向区域角度”滑块，0～75°、步长5°，保存后持久化。数值越小斜向越窄；建议先试20°或30°。新默认30°，旧配置保持45°，0°为四方向。该设置独立于中心死区；旧固件禁用该滑块和“记住输入模式”选项。

旧配置token或保存失败会明确报错，不自动重试写Flash。完整协议和使用条件见 [XINPUT_MODE.md](XINPUT_MODE.md)，坐标解释见 [COORDINATE_DECODING.md](COORDINATE_DECODING.md)。

## 从网页进入固件刷入模式

1. 不支持F9的旧固件需双击板卡RESET，将当前修订号18的UF2复制到出现的磁盘。Xbox / PS5 / Switch模式需先返回Steam才有网页UF2入口。
2. 更新网页并连接XIAO；在“目标 XIAO”选择接收器，“固件更新”应显示修订号5或更新版本。
3. 点击“进入 UF2 刷入模式”。网页重新核实固件版本，通过F9发送`UF2!`，读到成功回复后释放目标的全部槽接口。
4. 接收器重启进入UF2 bootloader，USB和无线暂时断开。等待磁盘出现，复制`SCReceiver-xiao-s140-7.3.0.uf2`。
5. 更新完成后点击“连接 / 授权接口”重新连接，按Steam启动手柄检查恢复。

此入口仅支持本工程XIAO修订号>=5；旧固件按钮禁用，原版接收器只读取身份。它不在浏览器中直接写UF2。已保存绑定保留；待保存的新绑定应先完成保存。正在暂存绑定导入时，固件返回busy而不重启。命令与EP0完成规则见 [USB_FEATURE_FIX.md](USB_FEATURE_FIX.md)。

## 原接收器 → XIAO 的迁移

### 已证实的能力

|操作|当前支持|
|---|---|
|原版USB A1读取CHIPID/RF身份|协议已恢复，网页已实现；本机hidapi读取已验证|
|原版通过USB直接导出全部绑定密钥|**没有已证实的接口**，网页不会发送猜测的读Flash/工厂命令|
|从已有原版MainBlock/双页dump解析id/key|已实现，采用原固件的页选择及最新记录规则|
|XIAO网页导出/恢复完整身份与绑定|已实现F1–F6、原子提交和回读|

不能套用二代OpenPuck的A2/A3：2015版将这些命令转发给手柄。B0是写绑定，B4是连接状态，也不能当作原版备份接口。

### 操作步骤

1. 点击连接，选择原版 `28de:1142` 接收器；网页只对它发送A1读取身份。
2. 在“原版身份来源”选择它，点击“使用所选原版身份”。A1的CHIPID按原顺序转成五字节，再清首字节低三位，得到RF地址。
3. 载入**另行取得**的32KiB MainBlock dump，或从0x7400开始的1024字节双绑定页dump。不是本工作区的`d0ggle.bin`更新应用。
4. 页面显示活动页、记录数量、旧版本去重情况及手柄标识，不显示密钥。读取身份后可以拔下原接收器。
5. 连接目标XIAO，关闭所有无线手柄，点击“导入并应用”。整库及原RF身份会替换目标内容；未提交的暂存不生效。
6. 页面在Flash提交后重新备份回读，比较身份及每条id/key；一致后显示完成。确保原接收器已拔下，再开手柄测试恢复原绑定。

迁移的关键是**接收器身份也要一致**，不是只写24字节id/key。身份提交后，固件同步重建RADIO地址和七槽跳频。恢复原手柄绑定的真实效果仍需硬件验证。

原版两页格式没有CRC，state=F0只是旧固件lookup接受标志，无法证明掉电时记录写全。网页按7600首条完整25字节是否全FF重放启动选择，只在选定页内从20→1扫描同ID最新记录，不合并另一页“猜最新”。页面保留两页概要与完整性边界。

进一步实现原版USB一键采集，需真正接收器 `0x7800..0x7fff` bootloader及其读取/保护路径证据。已有应用进入bootloader的路径会擦写page0，不能作为网页只读备份步骤。详细静态出处在工作区 `analysis/receiver_d0ggle/IMPLEMENTATION_MIGRATION.md`。

## XIAO备份格式

```json
{
  "format": "sc2015-receiver-backup",
  "version": 1,
  "receiverAddress": "a812345678",
  "bonds": [
    {
      "controllerId": "0102030405060708",
      "key": "00112233445566778899aabbccddeeff"
    }
  ]
}
```

以上是合成示例。地址5、id8、key16均为原字节顺序hex；不倒序。最多20条，重复ID/长度错误会在发送到设备前拒绝。零条记录表示恢复为空绑定库。

“备份目标XIAO”下载JSON；“下载诊断JSON”只保存计数、槽状态和普通输入，不包含绑定。备份/转储只在本页内存中处理，不存localStorage、不向网络上传，CSP关闭网络连接。

## 网页测试

```powershell
npm ci --ignore-scripts
npm test
npm run test:browser
```

Node测试不需要浏览器；Playwright测试使用已安装Microsoft Edge headless，启动仅监听127.0.0.1的本地静态服务。当前62个Node测试、24个浏览器流程覆盖原有模式/诊断回归及新增语言选择：英语地区变体、未支持语言回退中文、手动偏好刷新后保留、存储拒绝、动态错误/历史重译、窄屏布局、切换时保留HID和未保存表单。英文页面与完整诊断面板还检查遗漏的中文文案。这些测试的HID均为模拟；历史本机hidapi验证属于修订号11，见 [REV11_LIVE_VALIDATION.md](REV11_LIVE_VALIDATION.md)。
