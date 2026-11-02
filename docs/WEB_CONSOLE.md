# WebHID 网页控制台与 GitHub Pages

入口源码：[`../web/index.html`](../web/index.html)。纯静态HTML/CSS/ES modules，不需要后台、生产npm依赖或GitHub Token。接收器通过USB连接到**运行浏览器的这台电脑**，数据通过WebHID在本地交换。

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

1. 刷入本工程更新后的 `build/xiao/SCReceiver-xiao-s140-7.3.0.uf2`。当前版本是修订号7、诊断协议v3；网页也兼容旧v2。
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

操作记录最多100条，页面隐藏时暂停定时查询。Feature操作串行，处理未完成/旧回复，单次USB调用有超时。回复不匹配保留槽、最近无线状态和输入监听，下次刷新重试；真正挂起的USB接口暂停查询，可通过“连接 / 授权接口”或“读取已授权设备”重新打开。诊断JSON记录`queryError/queryClosed`。断开网页关闭HID句柄，物理拔出移除相应槽。

Steam或命令行工具也可能访问同一个Feature接口。诊断时可先退出它们，避免交错请求；页面只能串行化自己的请求。

## 从网页进入固件刷入模式

1. 旧固件首次升级仍需双击板卡RESET，将当前修订号7的UF2复制到出现的磁盘。
2. 更新网页并连接XIAO；在“目标 XIAO”选择接收器，“固件更新”应显示修订号5或更新版本。
3. 点击“进入 UF2 刷入模式”。网页重新核实固件版本，通过F9发送`UF2!`，读到成功回复后释放目标的全部槽接口。
4. 接收器重启进入UF2 bootloader，USB和无线暂时断开。等待磁盘出现，复制`SCReceiver-xiao-s140-7.3.0.uf2`。
5. 更新完成后点击“连接 / 授权接口”重新连接，按Steam启动手柄检查恢复。

此入口仅支持本工程XIAO修订号>=5；旧固件按钮禁用，原版接收器只读取身份。它不在浏览器中直接写UF2。已保存绑定保留；待保存的新绑定应先完成保存。正在暂存绑定导入时，固件返回busy而不重启。命令与EP0完成规则见 [USB_FEATURE_FIX.md](USB_FEATURE_FIX.md)。

## 原接收器 → XIAO 的迁移

### 已证实的能力

|操作|当前支持|
|---|---|
|原版USB A1读取CHIPID/RF身份|协议已恢复，网页已实现；设备实测待完成|
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

Node测试不需要浏览器；Playwright测试使用已安装Microsoft Edge headless，启动仅监听127.0.0.1的本地静态服务。浏览器用模拟HID验证连接、四槽映射、自动保存、拔出、JSON下载、原身份＋dump迁移、旧固件提示、UF2模式命令、查询异常恢复和窄屏布局。没有访问真实设备或验证RF。
