# ProMicro / XIAO手动构建与日期Release

以`Receiver/`作为GitHub仓库根目录。将工作流及对应源码提交到默认分支后，在仓库 **Actions** 中使用 **Run workflow**。三个用户入口均仅由`workflow_dispatch`触发。

## 工作流入口

|Actions名称|工作流文件|结果|
|---|---|---|
|Receiver firmware build|[firmware.yml](../.github/workflows/firmware.yml)|构建XIAO，下载本次运行的Artifact|
|ProMicro nRF52840 firmware build|[firmware-promicro.yml](../.github/workflows/firmware-promicro.yml)|构建ProMicro，下载本次运行的Artifact|
|Receiver date release|[release.yml](../.github/workflows/release.yml)|构建两种板型并创建带下载附件的GitHub Release|

两种单板入口和Release共同调用 [build-firmware.yml](../.github/workflows/build-firmware.yml)，使用Windows2022、Python3.11、Arduino CLI1.3.1及固定版本core/TinyUSB。每个目标独立runner/cache，先执行Python工具测试，再干净编译、二进制审计和UF2校验。

单板Artifact名为`SCReceiver-xiao-<run_id>-<run_attempt>`或`SCReceiver-promicro-<run_id>-<run_attempt>`，保留30天；内含UF2、HEX、ELF、manifest及compile.log。失败时已有compile.log另存14天。单板构建默认`release_version=development`，不创建标签或Release。

## 日期版本规则

Release工作流在开始时取**北京时间（UTC+8）**日期，Tag与Release标题均使用该版本，不加`v`前缀。例如同一天运行：

```text
2026.01.10
2026.01.10p1
2026.01.10p2
...
2026.01.10p10
```

跨天首次运行使用新的日期。查找全部分页的Git标签和Release（包含草稿），按数字最大后缀+1分配；已存在`p3`时不会回填`p1/p2`。裸标签也占用版本，避免覆盖先前失败或人工创建的版本。日期在计划阶段确定，构建跨午夜仍使用同一个计划版本。

**日期版本与协议修订号分开：**日期字符串写入两份固件的`sc2015_build_version`，构建后从ELF对应字节核验；也写入文件名和manifest的`release_version`。原F0/F7/FD等单字节`firmware_revision`是功能门控修订号，目前为19，不用日期替换，避免破坏现有网页/CLI协议。

## Release下载

运行`Receiver date release`成功后，在仓库 **Releases → 对应日期版本 → Assets** 下载对应板型：

```text
SCReceiver-2026.01.10-ProMicro-nRF52840-s140-6.1.1.uf2
SCReceiver-2026.01.10-XIAO-nRF52840-s140-7.3.0.uf2
```

同日再次发布时文件名也带`2026.01.10p1`。同时提供两份应用HEX、各板型manifest和`SHA256SUMS`。manifest记录板型、SoftDevice、源码commit/逐文件SHA-256、日期版本、能力修订号、镜像范围及审计结果。

发布过程：

1. 全工作流使用固定concurrency组串行分配版本，正在执行的发布不会被后续运行取消。
2. 两个板型都构建成功后，发布job只下载**本次run与attempt**的Artifact。
3. 重新检查板型/SoftDevice/起始地址、UF2全部块、向量、HEX摘要及其与UF2逐字节的一致性、源码摘要、版本和commit。`.gitattributes`将固件源码固定LF，保证Windows构建与Linux发布核验的原始字节一致。
4. 发布前再次检查版本是否被占用，再原子创建指向选定commit的新Tag；不移动现有标签、不覆盖已有Release附件。
5. 上传带完整附件的草稿，成功后才转为正式Release。只有发布job有`contents: write`，使用仓库自带`GITHUB_TOKEN`，无需个人Token。

如果外部操作抢占版本，或上传中途失败，流程停止。已创建的Tag/草稿保留，下一次完整运行会选择下一个版本。重跑请用 **Re-run all jobs**（或重新Run workflow），以重新分配版本并生成匹配的两份产物；仅重跑失败job不会重新构建先前成功的Artifact，不能用它覆盖旧发布。

仓库/组织若禁止工作流创建标签或Release，需要允许该发布job的内容写权限；失败会保留GitHub错误，不会被当成“尚无版本”。

## 两种板型的区别

|项目|ProMicro nRF52840|XIAO nRF52840|
|---|---|---|
|参考硬件|OpenPuck同类SuperMini/ProMicro，Adafruit UF2 bootloader|DX/Seeed XIAO，S140 7.3.0 UF2 bootloader|
|FQBN|`adafruit:nrf52:feather52840`|`Seeeduino:nrf52:xiaonRF52840`|
|固定core|Adafruit1.7.0|Seeed1.1.13|
|SoftDevice布局|S1406.1.1|S1407.3.0|
|应用起点|`0x26000`|`0x27000`|
|LED|P0.15，高电平亮；配对闪烁、在线常亮、故障快闪|原RGB低电平亮：蓝配对、绿在线、红故障|
|可选外接配对键|P0.02→GND，保持500ms|D0/P0.02→GND，保持500ms|

ProMicro按OpenPuck `0114623` 的默认`feather52840`目标和`status_led.h`的SuperMini P0.15设置实现。借用Feather编译配置并不表示所有同名克隆板具有相同LED/时钟或bootloader；这里提供的是上述S1406.1.1配置。两种应用都在`0xe9000`前停止，配置/绑定四页`0xe9000..0xecfff`仍受链接和镜像校验保护。

**UF2不可混刷，也不能只改地址重定位。** 两个文件分别链接；都不包含SoftDevice、bootloader、UICR或存储页。转换为ProMicro目标后，原SC2015空口协议、五种USB输入模式和配置记录格式不变；A1历史签名`SC2015 XIAO`继续作为本工程配置协议族标记，以兼容现有网页，不用它判断物理板型。物理板型以下载文件名、manifest和板卡`INFO_UF2.TXT`为准。

## 本地构建

Windows，在Receiver目录执行：

```powershell
python tools/dependencies.py --target all
python tools/build.py --target promicro --clean
python tools/build.py --target xiao --clean
python tools/build.py --target promicro --verify build/promicro/SCReceiver-promicro-s140-6.1.1.uf2
python tools/build.py --target xiao --verify build/xiao/SCReceiver-xiao-s140-7.3.0.uf2
```

只安装某板依赖时将`all`换成`promicro`或`xiao`。不指定target仍默认XIAO。可加`--release-version 2026.01.10p1`本地构建指定日期版本；实际Release的版本由工作流自动分配。本地输出目录为`build/<target>/`，Arduino编译缓存为`build/cache/<target>/`；这些命令不会扫描或刷写设备。

## 本轮验证

- 两个目标以日期样例`2026.11.04`成功ARM编译、链接、UF2校验及嵌入版本审计，原生协议ASan及33个Python测试通过。
- actionlint **1.7.12** 检查全部工作流通过；离线测试覆盖日期合法性/北京时间日界、p9→p10、全部分页与草稿、网络失败拒绝、双板混用/源码变化/HEX不一致拒绝、Tag占用、草稿上传失败不发布及不覆盖旧附件。
- 实际两份本地产物已成功走通`release.py package`，输出在`build/release-local-check/`。本地工作树尚未提交，manifest的`source_dirty=true`；该目录是打包验证结果，并非已发布Release。
- 尚未在GitHub远程执行新工作流，也未实物刷写ProMicro；本轮没有改写已连接的XIAO。历史XIAO实测与当前目标构建分开记录。
