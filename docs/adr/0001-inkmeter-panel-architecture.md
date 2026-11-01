# ADR 0001：InkMeter 的固件集成与边界

- 状态：Accepted；2026-10-31用户于Q25接受，实施补充D25–D29继续有效。
- 基线：`develop` / `e7bb30852258f0109ca01641ae66669befe96391`。
- 规则主文档：[InkMeter规格、实现与验证](../spec/inkmeter-panel.md)。本文维护选择理由和代价；接口、参数、状态机与测试结果在主文档维护。

## 背景

InkMeter是在Metalio E-INK4 Plus上持续显示Sub2API用量及账户额度的原生应用。设备已有导航、worker、网络、电源、NVS、显示及OTA设施；目标是在这些设施上局部接入，便于持续合并上游。

两个实现约束决定了方案：完整账户额度读取超出受限管理员Key的权限；真机无法承担原手机HTTPS配置服务的内存开销。用户分别选择了完整管理员Key和手机HTTP配置，并接受相应保护边界。

## 选择与理由

| 选择 | 理由与代价 | 规则位置 |
| --- | --- | --- |
| 默认关闭的独立构建配置 | 根配置保持上游原样，显式启用才裁剪应用；代价是国内/Global的原厂与InkMeter四组合都需验证 | [配置矩阵](../spec/inkmeter-panel.md#111-独立配置与四组合矩阵) |
| 普通原生应用、原厂入口及启动流程 | 保留上游生命周期与导航；离开后停止业务，重新进入依靠缓存恢复 | [产品边界](../spec/inkmeter-panel.md#2-已确认的产品边界)、[生命周期](../spec/inkmeter-panel.md#61-生命周期与失败) |
| 设备直连单个Sub2API，使用完整管理员Key | 无日常手机或适配服务依赖，覆盖已核实读取接口；设备功能受限不等于Key的服务端权限受限 | [API契约](../spec/inkmeter-panel.md#51-已核实的-api-契约与兼容边界) |
| 额度区分已有快照与显式手动采样 | 满足用户“默认读取站点已有数据”的要求；缺少安全快照入口时只能显示本机手动结果 | [平台能力](../spec/inkmeter-panel.md#52-全平台额度能力与调用边界) |
| 手机独立HTTP服务、现场配对、普通NVS | 减少服务端TLS与证书开销，保持现有分区和启动方式；仅适用于已接受的可信局域网、受控实物场景 | [配置与敏感数据](../spec/inkmeter-panel.md#62-配置提交与敏感数据) |
| 前台常显并显式接入待机钩子 | USB供电识别不可靠，待机也是overlay；单靠页面卸载或充电状态无法表达暂停/恢复 | [固件约束](../spec/inkmeter-panel.md#7-已核实的固件约束) |
| RAM缓存，SD可用时持久化 | SD导出或无卡仍可运行，避免新Flash分区及大JSON挤占NVS；断电可靠性依赖缓存校验与重建 | [缓存](../spec/inkmeter-panel.md#63-快照缓存) |
| 私有UDP NTP与RTC回写 | RTC VL无效时恢复HTTPS所需时间；复用已有worker和条件tick hook，保持原厂时钟语义及默认配置隔离。普通NTP不认证，真实可达性与RTC保持须设备验证 | [校时规则](../spec/inkmeter-panel.md#105-rtc无效恢复与自动校时) |
| 当前原型为视觉基准，字形嵌入app | 复用字体包并保持资源兼容；I1转换曾使灰色Token柱消失，须对最终帧做像素检查 | [设备UI](../spec/inkmeter-panel.md#3-设备端功能基线)、[实现陷阱](../spec/inkmeter-panel.md#101-页面与服务边界) |

## 未选择的方案

| 方案 | 未采用原因 |
| --- | --- |
| 只读适配服务或Sub2API后端权限改造 | 可缩小设备凭据权限，但增加部署、后端开发与维护范围；用户Q8已选择直连完整Key |
| 手机SoftAP首次配网 | 原厂网络设置已能完成联网；手机配置从设备入网后开始 |
| 手机自签HTTPS及证书信任流程 | 原Q20接受，后被真机内存失败及D26的HTTP要求覆盖；设备出站HTTPS仍保持验证 |
| 默认调用通用usage/batch或附加force=false | 部分接口仍会隐式访问上游，无法满足已有快照读取边界；逐平台使用已核实的被动来源 |
| 硬件NVS/Flash加密迁移 | HMAC provider首次初始化可能烧写受保护eFuse，既有明文NVS也需迁移；用户选择普通NVS及保留分区/eFuse |
| 全局SNTP接管、修改RTC/OTA驱动或增加常驻校时任务 | 容易改变原厂时钟生命周期或增加内部RAM负担；D29选择私有UDP客户端、既有worker与原驱动接口 |
| 新长期产品分支或发布渠道 | 当前在feature工作树实现；产品渠道与OTA分发留待发布设计 |

权限依据为[固定版本权限目录](https://github.com/Wei-Shaw/sub2api/blob/d851a38477597157e78450d30af2d8504764d085/backend/internal/pkg/adminaccess/catalog.go#L20)；NVS取舍依据为[ESP-IDF NVS encryption](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s31/api-reference/storage/nvs_encryption.html)。平台接口与副作用的源码证据集中在规格第5节。

## 集成边界

- 业务放在`main/inkmeter/`，页面按`main/display/screen/inkmeter_screen/`约定接入。服务输出规范化快照，UI通过异步通道接收；平台交叉点集中在现有导航、worker、存储、电源与EPD设施。
- 配置HTTP服务独立于原厂文件传输路由，绑定InkMeter配置模式；原厂服务端口80已使用且无配对鉴权，不能直接扩展为凭据入口。路由与授权契约见规格第6.4节。
- `StandbyInhibit`与`ForegroundNetwork`分别管理常显和有界网络工作；待机显式暂停并归还相应hold，恢复时显示缓存再同步。实现须保持与原厂电源策略的协作。
- 按D28，`CONFIG_INKMETER_ENABLED=n`保留原厂完整应用路径；`y`才按D25排除三个应用。共享调用的原厂逻辑保留在条件编译分支中，私有`main/inkmeter/inkmeter.cmake`集中收集与排除源码。专用配置片段和独立输出避免改写根配置；构建矩阵以规格第11节为准。
- 阅读只在BOOT长按注册处按配置停用入口，保留原厂`book_vk`处理；InkMeter时间适配继续局限在自身边界。D29仅增加一个条件`DispatchClockTick`调用，私有模块调度与RTC回写规则以规格第10.5节为准；原厂RTC/OTA驱动、TZ与根SDK配置保留。

HTTP不提供链路保密、完整性或设备身份保证；配对与同源校验不能防止局域网中间人。普通NVS不能防止离线读取flash。正式配置关闭内存转储及自动SD导出以避免任务栈泄露Key；该保护不改变前述已接受风险。详细凭据规则以规格第6.2节为准。

## 出站传输与资源取舍

出站证书校验继续使用SDK信任包。真实部署的交叉签名链触发失败后，选择SDK已有交叉签名验证能力，以包内既有可信锚结束验证；保留主机名、签名和有效期检查。选择理由是修复验证路径，而非引入服务端根证书信任。业务通过实际对象的`dynamic_cast<EspNetwork*>`在读取Key/CreateHttp前拒绝非ESP网络，公共网络接口保持上游原样。具体配置、额外堆预算、HTTP分片和套接字关闭所有权见[传输实现](../spec/inkmeter-panel.md#103-传输与内存实现)。

图标和必要字形随app嵌入，resources/font_data保持原布局。现有OTA只更新app；刷回原厂app会覆盖InkMeter。未来合并以明确的`upstream/develop`提交为基线，解决冲突后按规格重验，发布渠道另行决策。

保留的共享差异按问题承担边界，而非通过InkMeter开关隐藏公共缺陷：

| 共享差异 | 保留原因 |
| --- | --- |
| `http_client.h`、`http_client.cc`、`esp_ssl.cc` | 任意分片的HTTP解析、TLS分配失败清理与唯一关闭关系均影响共享网络；回归范围见规格第12.2节 |
| `develop e7bb308`干净构建修复 | 保证上游基线可构建；与InkMeter产品行为分别审阅 |

构建描述直接使用SDK版本及ELF摘要，不增加Git探测或公共网络能力接口。四组合证据与本轮未烧录状态集中在[规格第13.9节](../spec/inkmeter-panel.md#139-可选构建配置与四组合验证)。

RTC无效恢复使用普通UDP NTP，不提供时间服务器认证；随机nonce只负责关联报文。选择此方案是在现有自用可信网络边界中恢复系统/RTC可用性，不将联网成功或NTP字段校验作为可信时间证明。协议、调度、取消与回写约束集中在规格第10.5节，源摘要和本轮待验收结果集中在[第13.11节](../spec/inkmeter-panel.md#1311-rtc无效后的自动校时修复)。

## 接受与后续变更

Accepted表示架构选择已确认；设备验收状态由[规格第13节](../spec/inkmeter-panel.md#13-验证记录与待验收事项)单独维护。变更按[第8节流程](../spec/inkmeter-panel.md#8-变更流程)执行，并满足[第9节门槛](../spec/inkmeter-panel.md#9-实现验收门槛)。调整信任边界、后端依赖、分区/eFuse或发布方案时，新增或修订ADR，写明触发条件、取舍与被替代决策；参数和测试结果在规格所属章节更新。
