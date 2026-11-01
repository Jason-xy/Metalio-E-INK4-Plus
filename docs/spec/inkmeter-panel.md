# InkMeter 面板应用：规格、实现与验证

- 规格状态：Ready；Q25于2026-10-31确认，实施补充见D25–D29。
- 实现状态：自动校时修复版已于2026-11-01烧录；真机从无效RTC恢复、RTC写回及重启恢复、真实Sub2API同步通过。见第13.11节。
- 验收阻塞：新UI未重跑30分钟测试，既有内存历史低水位未达50 KiB目标；详见[待验收项](#134-尚需设备验收)。
- 更新日期：2026-11-01（Asia/Hong_Kong）。
- 基线：`develop` / `e7bb30852258f0109ca01641ae66669befe96391`；分支 `feature/inkmeter-panel`。

| 工作 | 阅读入口 |
| --- | --- |
| 调整功能或数据语义 | 第2–6节为规则；改API时核对[固定版本契约](#51-已核实的-api-契约与兼容边界)及[平台能力](#52-全平台额度能力与调用边界) |
| 修改页面 | [设备端基线](#3-设备端功能基线)、[当前原型](references/inkmeter.html)；手机页另读[配置基线](#4-手机配置功能基线) |
| 修改集成或凭据方案 | [架构取舍](../adr/0001-inkmeter-panel-architecture.md)、[固件约束](#7-已核实的固件约束) |
| 开发、使用、构建与测试 | [变更流程](#8-变更流程)、[实现与使用](#10-实现与使用)、[构建](#11-构建)、[自动验证](#12-自动验证) |
| 判断完成或准备发布 | [验收门槛](#9-实现验收门槛)、[镜像与实测记录](#13-验证记录与待验收事项) |

本文是InkMeter启用配置的功能、接口和验收主文档；ADR记录选择理由，原型提供视觉参考。`CONFIG_INKMETER_ENABLED`默认`n`，保持原厂完整应用路径；显式启用时才应用下述InkMeter范围，构建入口见第11节。构建与测试命令均在仓库根目录运行。

## 1. 来源与证据边界

- 需求来源为用户引用的「UI/UX」会话（`6ae2b4b5-dce4-83ee-9cfc-f0934f947428`）、Q1–Q25确认及2026-10-31实施反馈。决策追溯见第2节。
- [当前原型](references/inkmeter.html)是四页视觉基准，包含Token留白/字号、用户名及单色柱状图修正；仓库仅保留此版本。示例指标、账号和网络状态属于演示数据。
- 实施反馈以D26覆盖原手机HTTPS/自签证书方案，以Q19/D14覆盖早期默认上游探测许可。当前规则分别集中在第5–6节。
- 固定版本源码核查、主机fixture和部署实例实测分别记录。实例已验证配置保存与真实请求；部署版本识别、UTC+8分桶和逐项数据对账仍待验收，见第13节。

## 2. 已确认的产品边界

| 决策 | 已确认内容 | 来源 |
| --- | --- | --- |
| D01 | 通过原厂应用列表打开 InkMeter，退出回原厂首页，保留默认启动流程 | 本轮 Q1 |
| D02 | 一个 Sub2API 站点；同时只选择一个监控用户，可选择多个上游额度账户；用户与账户分别选择 | 本轮 Q2、V4 |
| D03 | 首版同时交付设备端四页和手机端五页配置 | 本轮 Q3 |
| D04 | 主要使用场景为 USB 常供电的桌面面板 | 本轮 Q4 |
| D05 | 离开 InkMeter 暂停定时同步并保留缓存；再次进入先显示缓存，再同步 | 本轮 Q5 |
| D06 | Token 四项继续采用 2×2 信息组织，修正留白一致性并提高数字可读性 | V4、用户后续反馈 |
| D07 | 先在原厂“设置→网络”连接 Wi-Fi；手机连接页访问和配对已入网设备 | 本轮 Q6 |
| D08 | 前台常显，电源键可手动待机；待机暂停同步/刷新，唤醒恢复；拔线不自动切换电源策略 | 本轮 Q7 |
| D09 | 使用完整管理员 Key；不增加 Sub2API 后端权限改造 | 本轮 Q8，用户选择完整权限方案 |
| D10 | 手机按页编辑草稿，保存成功才生效；失败继续使用已生效配置 | 本轮 Q9 |
| D11 | 移除总览成功率，保留请求次数 | 本轮 Q10 |
| D12 | 设备现场开启配置模式；10分钟无操作、退出应用或待机时关闭，撤销配对会话 | 本轮 Q11 |
| D13 | 手机到设备使用HTTP明文配置，受D26覆盖；普通 NVS 保存 Key；禁止网页回读、日志或SD导出。设备实物受控，接受离线读取 flash 可取出凭据的边界；保留分区与eFuse | Q12；手机传输由2026-10-31实施反馈覆盖 |
| D14 | 上游额度同步仅由明确手动操作触发；支持force的接口使用false，遵守缓存/节流，未提供节流的接口由设备防重/冷却；默认读取不能隐式访问供应商 | 本轮Q19覆盖此前Q13的默认探测许可 |
| D15 | 统一 Asia/Hong_Kong（UTC+8）；今日从零点到当前，7天/30天均包含今日 | 本轮 Q14 |
| D16 | 用户按整数分钟设置用量同步频率；该周期不驱动额度同步 | 本轮Q15、Q19 |
| D17 | 首版移除环比，保留真实总量与趋势 | 本轮 Q16 |
| D18 | 首版覆盖 Sub2API 全部现有平台提供的额度能力，不缩减为 OpenAI/Anthropic 两类 | 本轮 Q17 |
| D19 | 有SD时持久保存最近成功快照，重启可恢复；无卡/USB导出时仅内存。Key和原始响应不写SD | 本轮 Q18 |
| D20 | 取消手机配置服务的自签证书、指纹核对和浏览器信任步骤，使用设备显示的HTTP地址和一次性配对码 | 2026-10-31实施反馈覆盖Q20 |
| D21 | 正式使用配置关闭内存coredump捕获及自动SD导出，保留脱敏普通日志；转储调试版只用测试Key | 本轮Q21 |
| D22 | 局刷由原厂策略选择，缓存采用固定策略；移除显示页的“局部刷新/离线保留”开关 | 本轮Q22 |
| D23 | 没有安全站点快照入口的平台显示本机上次手动快照，首次提示“待手动同步”；首版不补后端接口 | 本轮Q23 |
| D24 | 逐账户提供“同步上游额度”；打开额度页/普通刷新只读已有快照，不一键探测全部账户 | 本轮Q24 |
| D25 | InkMeter启用配置不编译WorkBuddy、百问和每日清单应用；停用其入口、对话设置、待机待办和启动后台业务，保留其它原厂应用；默认配置的保留边界见D28 | 用户实施阶段补充，2026-10-31 |
| D26 | 因真机内存不足，将手机配置服务改为HTTP，保留现场开启、一次性配对、同源校验和10分钟空闲关闭；接受可信局域网内完整管理员Key与会话可被监听/篡改的明文风险；设备访问Sub2API仍使用校验证书的HTTPS | 用户实施阶段补充，2026-10-31 |
| D27 | 全部原生四页按当前`references/inkmeter.html`的modern布局重构；用户名作为用户显示标识，数字ID仅作内部查询/关联；修复Token堆叠柱在真实单色I1输出中不可见的问题 | 用户持续验证阶段补充，2026-10-31 |
| D28 | InkMeter改为默认关闭的可选配置；`n`保留原厂完整路径，`y`应用D25及InkMeter安全配置。根配置保持上游原样，专用默认片段及构建输出独立；四组合验证独立记录，不继承旧镜像验收 | 用户本轮上游差异收敛计划，2026-10-31 |
| D29 | RTC时间无效时，InkMeter通过自身UDP NTP恢复系统时间并回写RTC；复用既有Network worker，默认关闭配置无此hook；保持原厂香港本地时钟/TZ、RTC与OTA驱动及根SDK配置，自动校时不跳过出站HTTPS验证 | 用户RTC无效恢复与联网自动校时要求，2026-11-01 |

首版范围包含实现与真机验证；Sub2API后端改造、产品发布渠道与OTA分发另行设计。

## 3. 设备端功能基线

四页共用当前监控用户、数据状态、最近成功更新时间和手动刷新入口。设备以横屏呈现，按运行时可绘区域布局；V4 的完整画布 1216×684 不直接作为普通页面可绘尺寸。视觉以[当前modern交互预览](references/inkmeter.html)为准：统一留白、文字层级、透明内容区、内嵌按钮背景及底部导航，不继续沿用早期原生页的布局。

用户名显示按去除首尾空白后的`username → email → 已保存的非旧版占位名称 → 用户名未提供`取值；旧的`#ID`、`用户#ID`或`用户 #ID`不得继续作为用户名称。标题、选择列表、设置页和手机用户列表均使用该规则，不额外显示用户ID标签；数字ID仍用于服务端精确查询、保存与统计隔离。已保存旧占位名称可在验证用户DTO后刷新并持久化，不因此重置当前统计查询。原始模型ID不受此规则影响，仍须完整可读。

以1212×680普通横屏为基准，原生布局采用28 px边距、88 px页头、96 px底部导航，主要触控目标至少84 px。当前原型正文26 px/标题32 px为视觉参考；固件可复用现有25 px/30 px字库，差异须在对照截图中审阅。总览Token数字使用52 px专用字形并保留K/M基线，四格留白一致；不以缩小数字或挤压文字掩盖布局问题。

Token分析柱及图例须适配真实I1单色输出，用纯黑和不同纹理区分四类Token，不能仅依赖在亮度阈值转换后消失的灰色。非零数据段在最终单色帧中须有可见像素，零值不人为补成非零柱。

| 页面 | 原型功能与信息 | 数据与行为约束 |
| --- | --- | --- |
| 消耗总览 | 当前用户单选；今日/7天/30天；Token 总消耗、累计趋势；实际费用 USD、请求数；普通输入/模型输出/缓存写入/缓存读取；缓存命中率 | 成功率和环比均已决定移除；累计曲线按真实分桶求累计和 |
| 用量分析 | 今日逐时、7天/30天按日；Token/费用/请求指标；Token 四类堆叠；原始模型 ID 列表，按 Token 或费用占比排序，长ID换行可读 | 同一用户/范围的真实分桶；列表分页/滚动，不常驻全站原始数据 |
| 账户额度 | 已选上游账户一账户一卡片，双列；按平台展示已提供的窗口、百分比/余额/积分、重置时间；接近上限提示；逐账户手动同步 | 见5.2能力表；余额、积分、本地估算、供应商窗口分别标识 |
| 设备设置 | 网络、站点、当前用户、监控账户数量、用量同步间隔、最后成功同步、本地配置地址/二维码、进入配置模式 | 现场开启/超时关闭，HTTP地址与一次性配对码；版本身份使用原厂固件描述并可识别InkMeter构建 |

交互参考：页签及左右切页；模型/额度区域上下滚动时不触发横向切页；滚动条在拖动时显示；完整原始模型 ID 可辨认。硬件 HOME/PREV/NEXT 与横屏触控映射须沿用原厂事件体系并验证。

切换用户影响用量、费用、趋势与模型数据；额度账户为独立选择域。不能沿用演示脚本中“切用户统一按倍率修改全部数字”的实现。

## 4. 手机配置功能基线

V4 的五个页签可以任意切换，并不是已经定义了前置条件的强制向导。

| 页签 | 基线功能 | 原型与真实实现的差距 |
| --- | --- | --- |
| 连接 | 设备识别、联网状态、访问地址、配置/配对提示 | 已决定复用原厂网络设置；移除 V4 热点首配和重复 Wi-Fi 输入流程 |
| 站点 | 单站点 HTTPS URL、凭据输入/显隐、测试连接 | 演示只检查 URL；真实测试须验证 TLS、认证、接口和所需权限 |
| 用户 | 按用户名/邮箱搜索或完整ID查询、单选监控用户、保存 | 读取管理员可见用户；列表按设备相同显示名称规则，不以数字ID命名或显示用户ID标签；分页，完整数字ID仍走精确查询；不提供多用户聚合 |
| 账户 | 按名称/平台搜索或完整ID查询、多选额度账户、保存 | 管理员可见账户分页；显示已选数量；删除/失效时标示并要求重新选择，不替换为其他ID |
| 显示 | 用量自动同步分钟数 | 保留1–1440整数、默认15、允许用户自定义；移除两个演示开关，额度采用只读已有数据/单独手动同步 |

手机用于配置；日常数据流为设备直接 HTTPS 访问 Sub2API。用户已选择完整管理员 Key，不增加后端改造。设备只调用明确的统计、列表及额度读取接口；不能提供任意后端代理、账户修改、删除或密钥管理入口。完整 Key 的服务端权限不会因设备仅提供读取操作而缩小。

## 5. 数据语义与必须修正的演示行为

- 所查 Sub2API 将四类 Token 归一化为互斥桶，`total_tokens` 为四者之和。缓存命中率为 `cache_read / (input + cache_creation + cache_read)`；零分母或字段缺失显示“—”，不能伪装成0%。不在已有总量上再次加缓存。
- 实际费用采用用户范围 `actual_cost`，即站点倍率后的用户计费值，单位 USD；不是上游订阅付款或供应商账单。不能混用仅按账户查询时的账号成本口径。
- 成功率 98.6% 为固定样例，真实快照不提供可靠成功率；首版移除该字段。
- 额度缺失显示“未提供”，不能当作 0%；5h/7d 是演示窗口，实际按平台能力呈现。
- 总览切换时间范围后，费用标题、时间轴和比较区间必须一致；不能保留“7天数据、今日标题”的原型矛盾。
- 总览采用累计趋势，分析页采用每桶消耗；不能直接沿用存在下降段的演示累计曲线。
- 模型占比、趋势柱、成功更新时间均使用同一查询范围的真实快照；不复用原型固定数组和静态比例。
- 手机用户和账户页采用已确认的草稿→保存→生效；原型“点击立即变更，保存只弹提示”不沿用。保存与切站点后的依赖配置处理须在状态机中明确。

### 5.1 已核实的 API 契约与兼容边界

调查基线为官方 [v0.2.19](https://github.com/Wei-Shaw/sub2api/releases/tag/v0.2.19) 及 main commit `d851a38477597157e78450d30af2d8504764d085`；两者以下关键处理代码一致。用户部署版本尚未确认；已完成联调的子集见第13节，兼容性验收仍需确认部署版本、UTC+8分桶和逐项数据对账。

| 用途 | 接口与边界 |
| --- | --- |
| 用量/趋势/模型 | `GET /api/v1/admin/dashboard/snapshot-v2`，显式传 `user_id/start_date/end_date/timezone/granularity`；设置 `include_stats=false`、`include_trend=true`、`include_model_stats=true` |
| 总览合计 | 对同用户、同日期范围的 `trend` 汇总；`snapshot.stats` 是不随 user_id/日期过滤的全站统计，禁止用作所选用户总览 |
| 用户选择 | `GET /api/v1/admin/users` 分页名称搜索；数字完整 ID 使用 `GET /api/v1/admin/users/:id`。服务器 `search` 不是ID查询，不能把仅过滤当前页当全站ID搜索 |
| 账户选择 | `GET /api/v1/admin/accounts` 分页；只提取 ID、名称、平台、类型和必要状态，丢弃不需要的 `credentials/extra`，不存整包响应 |
| 账户额度 | 区分“读取Sub2API已有快照”和“明确手动同步供应商”。默认只允许经源码确认不探测上游的接口/DTO字段；通用usage/batch和各平台主动quota/balance接口不能一律用于默认刷新 |

鉴权使用 `x-api-key`；当前受限管理员 Key 的权限目录未覆盖额度路由，且其账户列表不含额度快照，故不能替代用户选定的完整管理员 Key。完整账户 DTO 虽对常见敏感键脱敏，但会保留部分 credentials/extra；设备仍需字段白名单，不应日志/缓存原始响应。[鉴权](https://github.com/Wei-Shaw/sub2api/blob/d851a38477597157e78450d30af2d8504764d085/backend/internal/server/middleware/admin_auth.go#L50)、[权限目录](https://github.com/Wei-Shaw/sub2api/blob/d851a38477597157e78450d30af2d8504764d085/backend/internal/pkg/adminaccess/catalog.go#L20)、[账户映射](https://github.com/Wei-Shaw/sub2api/blob/d851a38477597157e78450d30af2d8504764d085/backend/internal/handler/dto/mappers.go#L236)。

日期端点接收 `YYYY-MM-DD`，结束日包含在范围内。请求时区影响查询边界，但所查趋势 SQL 没有显式使用该时区分桶；部署验收必须验证数据库/服务的桶标签与UTC+8一致，未满足时不能将该实例视为统计兼容。[快照处理](https://github.com/Wei-Shaw/sub2api/blob/d851a38477597157e78450d30af2d8504764d085/backend/internal/handler/admin/dashboard_snapshot_v2_handler.go#L168)、[趋势SQL](https://github.com/Wei-Shaw/sub2api/blob/d851a38477597157e78450d30af2d8504764d085/backend/internal/repository/usage_log_repo_trend.go#L284)。

快照支持 `ETag/If-None-Match/304`，但生成时间变化也可能改变 ETag；是否刷屏应比较实际可见数据。`generated_at` 是快照生成时间，不等于底层额度采样时刻。[快照缓存](https://github.com/Wei-Shaw/sub2api/blob/d851a38477597157e78450d30af2d8504764d085/backend/internal/handler/admin/snapshot_cache.go#L63)。

Anthropic OAuth/SetupToken 可通过显式 `source=passive` 被动读取已存额度。混合批量接口虽自动对Anthropic使用passive，其他平台仍可能触发上游请求；`force=false` 不等于绝不访问上游，因此默认流程不能调用通用batch。手动入口才允许相应主动采样。各平台没有提供的窗口保持“未提供”，分别记录设备取回时间与可验证的上游采样时间。[额度服务](https://github.com/Wei-Shaw/sub2api/blob/d851a38477597157e78450d30af2d8504764d085/backend/internal/service/account_usage_service.go#L348)。

真实状态视图必须覆盖：未配置、首次加载、同步中、无数据、离线缓存、认证/权限失败、数据过期、单账户失败、账户或用户失效。各状态必须区分有效的零值与没有数据。

### 5.2 全平台额度能力与调用边界

固定调查版本包含以下13个具体账户平台；`composite`是分组而非账户平台。账号类型包括OAuth、Setup Token、API Key、Upstream、Bedrock、Service Account，不能仅凭平台名假定所有类型都支持供应商额度。下表路径省略 `/api/v1`。[平台注册](https://github.com/Wei-Shaw/sub2api/blob/d851a38477597157e78450d30af2d8504764d085/backend/internal/domain/platforms.go#L24)。

| 平台 | 默认不访问供应商的数据来源 | 手动能力与显示边界 |
| --- | --- | --- |
| anthropic | OAuth/Setup Token显式passive usage：5h、7d、Fable已存窗口 | OAuth手动active usage取得已提供窗口（含Sonnet）；Setup Token继续使用被动/本地估算，不能伪装成完整OAuth能力 |
| openai | 账户extra中的 `codex_5h_* / codex_7d_*`、credits及reset-credit快照 | OAuth手动读取专用OpenAI quota/usage；遍历实际窗口秒数和additional limits，不固定两个窗口 |
| gemini | 账户已有本地quota字段；面板已取得的本地估算缓存 | 支持的OAuth/官方API Key可手动读取usage的本地RPD/RPM估算；该接口可能清除部分旧error，默认不调用。Vertex/第三方未提供的能力明确标示 |
| antigravity | 本机上次手动快照；首次“待手动同步” | 手动usage读取全部模型额度与AI Credits；不能把兼容five_hour字段当固定5h供应商窗口 |
| grok | `extra.grok_usage_snapshot / grok_billing_snapshot` | 手动读取官方quota/usage获得账单、窗口及已观察请求/Token限额；专用quota在必要时可能发短测试推理，界面说明该行为 |
| kimi | extra中的coding窗口、payg余额快照 | CN quota/balance；按返回的5h/weekly窗口或CNY余额显示 |
| zhipu | extra中的coding窗口快照 | CN quota；payg余额未提供 |
| deepseek | extra中的payg多币种余额快照 | CN balance；无coding窗口，不合并不同币种 |
| minimax | extra中的coding窗口快照 | CN quota；周窗口仅在上游提供时显示，payg余额未提供 |
| opencode_go | 顶层 `opencode_go_usage` 或专门GET快照 | POST专门refresh；展示rolling/weekly/monthly。Zen窗口/余额未提供，不双重调用同源CN接口 |
| command_code | extra中的窗口与积分余额快照 | CN quota一次读取窗口及余额，必要时单查balance；窗口按套餐返回 |
| cline | extra中的窗口与积分余额快照 | CN quota/必要的balance；标示实际ClinePass窗口与USD积分 |
| typesafe | 通用账户本地quota字段 | 无原生供应商额度同步接口；供应商额度显示“未提供”，本地额度单独标示 |

CN默认快照读取已知白名单字段：`${provider}_{5h|weekly|monthly}_used_percent`、对应reset、usage时间、余额/币种/多币种数组。主动 `GET /admin/cn-providers/accounts/:id/quota` 和 `/balance` 每次均可能访问供应商；只有并发singleflight，没有统一TTL或force参数，只能由手动入口调用。部分查询还会更新后端冷却/错误状态，不能宣称是无数据库副作用的纯查询。[CN quota](https://github.com/Wei-Shaw/sub2api/blob/d851a38477597157e78450d30af2d8504764d085/backend/internal/service/cn_provider_quota_service.go#L107)、[CN balance](https://github.com/Wei-Shaw/sub2api/blob/d851a38477597157e78450d30af2d8504764d085/backend/internal/service/cn_provider_balance_service.go#L96)。

除平台外，还适配Ollama Cloud/OpenCode Go跨平台挂载：根据安全DTO的eligible判定，优先于平台名推测。默认只读DTO或专门GET；手动才POST `…/{ollama-cloud-usage|opencode-go-usage}/refresh`。站点必须已有相关账号配置，InkMeter不配置供应商session或订阅。[Ollama state](https://github.com/Wei-Shaw/sub2api/blob/d851a38477597157e78450d30af2d8504764d085/backend/internal/service/ollama_cloud_usage.go#L511)、[OpenCode state](https://github.com/Wei-Shaw/sub2api/blob/d851a38477597157e78450d30af2d8504764d085/backend/internal/service/opencode_go_usage.go#L458)。

手动适配器使用固定路由：通用单账户active usage、OpenAI quota、Grok quota、CN quota/balance、Ollama/OpenCode refresh，以及Anthropic已有的reset-credit读取能力。任何消耗/执行reset credits、账户重置或配置更改路由均不调用；Grok可能的探测和供应商采样副作用在按钮说明中披露。[OpenAI quota](https://github.com/Wei-Shaw/sub2api/blob/d851a38477597157e78450d30af2d8504764d085/backend/internal/service/openai_quota_service.go#L42)、[Grok探测](https://github.com/Wei-Shaw/sub2api/blob/d851a38477597157e78450d30af2d8504764d085/backend/internal/service/grok_quota_service.go#L91)。

全平台覆盖指覆盖此版本已有的数据能力，不保证每个平台/类型都有供应商实时额度；“未提供”“尚未采集”“采集失败”和“上次手动结果”分别显示。未来平台/未知字段保持明确的不支持状态，经契约更新后适配，不能猜测或默认转为上游探测。

### 5.3 数值与图表规则

- Token按十进制K/M显示，内部保留原始计数；USD费用显示两位小数，非零小于0.01显示“<0.01”，不做汇率换算。供应商余额/积分保留原币种和语义。
- 命中率保留一位小数；已知无数据为“—”，有效零值仍为0。四类Token、请求数和费用均来自同一查询范围。
- 成功查询缺失的时间桶可补零；错误/不兼容查询不能补成零。总览累计和单调不减，分析页显示每桶消耗；时轴随今日/7天/30天改变。
- 额度按实际窗口/模型列表呈现；已用达到90%延续原型告警，超过100%保留真实数值并标示已达上限，进度条绘制范围限于0–100%。任一窗口缺失不影响其他有效窗口。
- 模型保留原始ID；按所选Token/费用占比独立排序，长ID换行/滚动可读，不用别名替换。

## 6. 运行、配置与缓存状态

### 6.1 生命周期与失败

- 原厂系统时钟及RTC沿用已包含香港偏移的本地时间；InkMeter仅在`UtcNow()`入口减去8小时，内部接收时间、缓存过期及HTTP日期形式的`Retry-After`统一使用UTC。显示、日期范围及午夜换日按香港偏移转换一次；服务端采集/重置时间按原始时区解析，统计请求继续显式传`Asia/Hong_Kong`。周期与超时继续使用单调时钟。此适配不修改原厂OTA/RTC；上游改存UTC时须移除入口补偿。
- 进入：恢复横屏布局、当前用户/范围/页签，先显示匹配的缓存，再开始有界用量同步；额度遵循只读快照规则。
- 前台：保持常显；用户分钟设置（1–1440，默认15）只控制用量周期。一个同步任务尚未完成时合并重复请求，不叠加定时器或无限排队。
- 打开额度页/普通刷新：只读取站点已有快照。无安全入口时使用本机手动结果，首次显示待手动同步。没有隐藏的定时上游额度采样。
- 逐账户“同步上游额度”：一次只采样该账户，完成后更新其规范化快照；平台/类型没有上游接口时禁用并说明，可用的本地估算另行标示。按钮显示忙碌状态，同账户并发合并，工程默认冷却60秒，并遵守更长的服务端 `Retry-After/retry_after_seconds`；遇429或重试延迟时立即停止该计划余下请求，保留已成功结果。不支持force参数的接口不能假装提供统一force保护。
- 离开/待机：停止周期任务，撤销UI会话代次、停止配置服务，释放电源/网络hold。迟到结果不能访问旧页面；仍有效的结果最多更新匹配的数据缓存。
- 唤醒/再次进入：恢复当前状态和缓存，再同步；不根据充电指示自动判断USB是否拔出。
- 系统时间无效时先尝试第10.5节的有界校时；无法恢复则显示待对时并延后用量请求，不将其计为普通用量失败。可信时间恢复或日历换日可重新调度用量，但保留用量、额度快照与逐账户手动同步的单调时钟冷却。
- 网络/服务错误保留最后成功值和时间，显示离线/失败状态；认证失败显示凭据问题，不能擦成零。逐账户失败不使其他成功卡片失效。
- 重试按所配置分钟周期退避并遵守 `Retry-After`，不得通过密集重试突破用户周期或服务器限流。时区/能力不兼容属于明确的配置错误。

### 6.2 配置提交与敏感数据

- 手机通过设备显示的HTTP地址访问并完成设备现场授权，不再生成或核对本地TLS证书。该链路不提供加密和设备身份认证，完整管理员Key、配对信息和会话可被局域网监听或篡改；仅用于用户已接受的可信局域网与受控设备场景。
- 配置窗口10分钟无操作自动关闭，退出/待机也立即关闭并撤销会话。到期前未提交的草稿不生效。
- 配对使用硬件随机的一次性信息；同一窗口连续5次失败即撤销并要求现场重开。仅一个已配对配置会话，Cookie使用HttpOnly、SameSite与短期有效期，不设置HTTP不可用的Secure属性；校验同源请求。连接、请求体和任务内存均有界，窗口关闭在worker完成，不阻塞LVGL。配对不能抵抗链路监听或主动中间人。
- 按页保存；站点/Key先验证TLS、认证及必需接口，再替换已生效配置。异步保存以提交时的草稿快照为准，等待期间的新编辑继续标记为未保存。保存失败继续使用旧配置，错误中不回显Key或原始响应。
- 换站点必须清除旧站点的用户/账户选择和统计缓存，防止相同数字ID映射到新站点的其他对象；同站点换Key后重新验证选择对象的有效性。
- Key只在输入/请求所需期间进入内存；普通NVS持久保存，配置回读仅返回“已配置”。日志、SD、URL、浏览器持久存储均不保存Key、配对秘密或完整响应；清除配置时撤销会话并清除对应缓存。
- 正式使用配置关闭coredump捕获及自动SD导出；普通日志保持敏感字段遮罩。开启内存转储的调试版只使用测试Key，不能将业务日志脱敏当作内存转储保护。
- NVS空间不足或提交失败必须返回失败并保留旧配置，不得为InkMeter重试而擦除共享NVS或原厂设置。配置服务不保留证书、私钥和TLS会话；Key、HTTP请求、响应与任务栈的内存预算按第9节验证，关闭服务须释放其资源。

### 6.3 快照缓存

- 保留规范化指标、范围和时间戳，不缓存原始JSON、credentials、站点Key或任意extra。
- 缓存按站点标识、用户ID、日期范围、时区及schema版本区分；额度还要按账户区分。选择变化时不能显示上一查询的数据并标成已同步。
- 当前schema为2，时间戳统一为UTC；schema 1的主文件、临时文件和备份均忽略，联网后重建快照，不迁移错误时间、不修改NVS站点/Key/用户/账户及周期配置。
- 有卡时保存最近成功快照到 `/sdcard/metalio/e-ink/inkmeter/`；SD未挂载、导出给USB或写入失败时，内存状态继续可用。SD不是运行前提。
- SD写入复用现有访问协调，在worker完成；临时文件/替换不视为严格断电原子保证。重启校验版本与内容，坏缓存丢弃后重拉，不影响配置、原厂文件或Key。
- 缓存过期/失败保持原值并显示真实上次成功时间；缺失值显示“—/未提供”，有效零值显示0。不同账户的新旧状态分别标示。

### 6.4 手机本地接口约定

配置页和API由同一HTTP源提供，服务只在配置窗口内存在。除建立授权所需的设备身份信息外，接口要求配对会话；Key仅在请求体内提交，不出现在URL或回读响应中。HTTP请求体仍为明文，不将此约束描述为传输保护。设备对Sub2API的出站请求保持HTTPS与证书验证。

出站HTTPS保留可信锚、主机名、签名与有效期验证；共享传输的交叉签名、分片和关闭所有权实现见第10.3节。

| 方法与路径 | 用途与返回约束 |
| --- | --- |
| `GET /api/status` | 设备标识、固件/InkMeter构建标识、配对状态；不返回站点凭据/私人统计 |
| `POST /api/session` | 校验当前一次性配对信息并建立短期会话；失败次数有界 |
| `GET /api/config` | 已生效的站点、用户、账户ID和用量周期；Key只返回 `key_configured` |
| `POST /api/site/test` | 测试草稿站点/Key的TLS、认证和必需接口；不保存、不探测供应商额度，返回脱敏能力结果 |
| `PUT /api/config/site` | 验证后提交站点/Key；站点改变清除旧选择与缓存；失败不替换 |
| `GET /api/users` / `GET /api/accounts` | 受控分页/搜索及完整ID查询，只返回显示/选择所需字段；不暴露任意后端代理 |
| `PUT /api/config/user` | 验证并提交单一监控用户 |
| `PUT /api/config/accounts` | 验证并提交额度账户ID集合，允许空集合 |
| `PUT /api/config/display` | 验证并提交 `usage_sync_minutes`，不包含已移除的两个开关 |

输入错误、配对失效、限流、后台忙、网络失败和保存失败均返回可区分状态及脱敏文案。站点测试与默认配置流程不能调用active额度路径。设备端手动额度按钮直接调用内部服务，不因手机配置接口而扩大远程操作面。

## 7. 已核实的固件约束

| 约束 | 源码证据与影响 |
| --- | --- |
| 普通横屏内容区为 1212×680，全屏为1216×684 | [display_orient.cc](../../main/display/display_orient.cc)、[config.h](../../main/boards/metalio-e-ink4-plus/config.h)；使用 `DisplayUiLogW/H`，不能假设竖屏 `UiSx/UiSy` 自动适配横屏 |
| 首页有普通/Slash/像素等入口复用关系 | [home_screen.cc](../../main/display/screen/home_screen/home_screen.cc)；所有支持的主题及国内/Global 构建均需覆盖 |
| UI 不能等待网络/SD | [ui-scheduling.md](../ui-scheduling.md)、[application_tasks.cc](../../main/application_tasks.cc)；投递失败由调用方处理；结果经UI异步通道并校验会话代次；队列和栈配置以源码为准 |
| 网络、电源与刷新已有统一入口 | [board.h](../../main/boards/common/board/board.h)、[power_policy.h](../../main/boards/common/power_policy/power_policy.h)、[epd_refresh.h](../../main/display/epd_refresh.h)；复用原厂策略，采样周期与物理刷新分离 |
| 默认待机与桌面常显存在冲突 | [PowerPolicy README](../../main/boards/common/power_policy/README.md)；默认约180秒待机、1800秒待机关机，`ForegroundNetwork` 本身不防待机 |
| 已有防自动待机能力，但 USB 检测不足 | `StandbyInhibit` 可抑制无操作待机，电源键仍可主动待机；[电池自检](../../main/display/screen/settings_screen/settings_test/settings_test_battery_screen.cc)说明禁用 DPDM 后 `VBUS_STAT` 可能为0但仍在供电；充电状态、USB主机枚举也不能替代可靠外接电源检测 |
| 待机不等于页面卸载 | [standby_screen.cc](../../main/display/screen/standby_screen/standby_screen.cc)的待机是叠加页；需要显式暂停/恢复业务并释放前台保网，不能只依赖 `SCREEN_UNLOADED` |
| 原厂文件传输服务占用端口80 | [ap_transfer.cc](../../main/display/screen/cloud_screen/ap/ap_transfer.cc)、[ap-file-transfer.md](../ap-file-transfer.md)；端口80、路由容量已占满、离页停服、无鉴权，不能直接当 InkMeter 常驻配置服务 |
| 当前 NVS/Flash 加密未启用 | [sdkconfig](../../sdkconfig)、[settings.h](../../main/settings.h)；使用 NVS 包装不等于凭据加密 |
| 真机HTTPS配置服务内存不足 | 用户实施反馈已覆盖原自签HTTPS方案；采用独立HTTP配置服务，限制连接与缓冲，不复用无鉴权文件路由。出站Sub2API的TLS与证书验证保留 |
| 原厂崩溃诊断含SD导出路径 | [sd_log_writer.cc](../../main/boards/common/storage/sd_log_writer.cc)将flash coredump复制到SD；任务栈可能含凭据，已决定正式使用配置关闭捕获及自动SD导出，普通日志遮罩不能替代内存转储控制 |
| OTA 只升级 app，资源独立分区 | [ota.cc](../../main/ota.cc)、[16m.csv](../../partitions/v1/16m.csv)、[lv_adapter_epdiy.cc](../../main/display/lv_adapter_epdiy.cc)；新增资源必须考虑旧资源分区兼容 |

## 8. 变更流程

1. 按本文件顶部入口读取本次改动涉及的规则和源码；列出受影响的页面、契约、平台交叉点及验收项。完成条件：每个改动点均对应第9节的一项门槛。
2. 实现并运行第12节中对应的主机检查；页面改动同时审阅原型与最终I1输出，固件改动完成对应配置的国内/Global构建及尺寸检查。涉及配置开关或共享集成时，执行第11节四组合矩阵。完成条件：受影响的检查全部通过，失败项已修复。
3. 涉及设备行为时，按第9节执行真机检查，并记录镜像摘要、环境、计时范围和结果。完成条件：本次镜像拥有自己的实测证据，未完成项明确标为待验收。
4. 更新规则的所属章节和第13节状态；架构选择变化时更新ADR。完成条件：规则只有一个维护位置，链接有效，设计接受、主机测试和真机验收状态各自明确。

## 9. 实现验收门槛

1. 对照当前`references/inkmeter.html`的modern设计验证全部四页/五页、1212×680普通横屏无裁切、统一布局和触控目标、52 px Token留白及K/M基线、用户名回退与旧名称迁移、完整模型ID、长列表、HOME退出与方向恢复；成功率/环比和两个开关已移除。Token柱须对真实I1输出采样各非零段内部像素，不能只断言控件存在或图例可见；有效零值保持为零。
2. 记录部署版本及脱敏fixture，核对同用户/同范围的四桶总量、USD实际费用、请求数、模型和UTC+8分桶；有效零值、无数据、缺字段、304与鉴权失败分别验证。
3. 对13平台及跨挂载能力建立契约fixture，逐一覆盖现有窗口/余额、多币种、未提供、待手动同步、部分失败、过期数据。未知类型不得猜测窗口。
4. 用请求记录验证打开页面、普通刷新、周期用量更新及站点测试均不触发供应商采样；逐账户手动只触发目标账户，重复点击/429/无force端点受控，不调用任何reset或账户配置修改路由。
5. 验证HTTP配置访问、配对失败限制、同源限制、超时/离页/待机撤销、保存失败回滚；不出现证书/指纹流程。Key不出现在配置回读、URL、普通日志、SD缓存或转储；验证出站Sub2API接受由已有可信锚验证的交叉签名链，仍拒绝证书错误。验证正式使用配置禁用coredump。
6. 验证有卡/无卡/USB导出、坏缓存、换站点/Key、NVS满及断电重启；旧站点数据不得混入新站点，不擦共享设置和原厂文件。
7. 模拟器验证布局和生命周期；真机验证触控/盖板按键、EPD残影、局刷合并、待机/恢复，以及HTTP反复启停、手机配置与出站HTTPS并发的堆峰值。遵守现有内部RAM低水位50KiB底线、app分区容量及资源兼容；记录空闲/开启/请求/关闭后的内部可用量、最大连续块和低水位，不能把SDK默认值或仅成功启动当作内存验收。
8. 覆盖官方支持的国内/Global构建及首页主题；涉及配置开关或共享集成时，四组合均须满足第11节条件：默认配置保留原厂应用，InkMeter配置按D25排除。版本与OTA资源关系按ADR检查。构建、烧录与验收状态以[验证记录](#13-验证记录与待验收事项)为准，OTA分发另行设计。
9. 30分钟网络/存储持续测试与后续原生UI改版镜像分别记录app/ELF摘要、计时范围和内存。前一镜像通过的时长不能写成后续UI镜像的完整持续验证；改版后的原生截图/I1测试、两配置构建与真机复核须单独完成。

10. RTC无效恢复须有当前镜像的冷启动/联网校时、系统香港时间、RTC写回与VL回读、再次重启恢复和真实Sub2API结果；失败/取消、网络受限和RTC回写失败时保留凭据门槛、短单调重试及服务端冷却。关闭配置的四组合隔离仍须通过第11节检查。

## 10. 实现与使用

### 10.1 页面与服务边界

业务入口在`main/inkmeter/`，原生页在`main/display/screen/inkmeter_screen/`。源码收集与D25排除列表集中在私有`main/inkmeter/inkmeter.cmake`；`CONFIG_INKMETER_ENABLED=n`沿用原厂应用及后台路径，`y`才启用InkMeter并停用三个应用。共享调用处用条件编译保留原厂逻辑；仅InkMeter分支使用`main/assistant_voice_disabled.h`空适配。阅读只在BOOT长按的注册处按配置停用入口，`book_vk`保持原厂逻辑。

服务向UI提供规范化快照；网络和SD工作经现有worker完成，页面回调只提交意图。三个应用源码继续保留；InkMeter配置中的阅读、壁纸、云盘、设置、蓝牙和共享平台能力保留。

页面布局和名称规则以第3节为准。Token柱原先使用的灰色全部高于生产I1亮度阈值16，转换后变白；当前采用纯黑/纹理，并在最终I1帧采样每个非零段的内部像素。只检查控件存在或图例可见不足以覆盖此缺陷。52 px Token位图为2,450字节，字体生成入口见第11节。

有效用户DTO经统一名称归一化后可刷新并持久化旧占位名称；持久写入沿用存储锁，保持当前统计代次。大量账户分组、组内滚动；额度查询按小批次让出共享Network worker。容量上限为64个账户、200个模型、单响应768 KiB、SD缓存1 MiB，超限返回明确错误。

### 10.2 使用流程

1. 在原厂“设置→网络”接入局域网；InkMeter配置可按第10.5节自动对时。完成条件：设备已获得IP，系统时间已恢复到支持范围，可用于出站证书验证；未恢复时保持待对时状态。
2. 打开“InkMeter→设备设置→开启配置模式”。完成条件：屏幕显示HTTP地址和8位一次性配对码。
3. 手机接入同一可信局域网，访问该地址并配对。完成条件：会话授权成功；HTTP明文与普通NVS的保护边界见第6.2节。
4. 输入Sub2API根地址及完整管理员Key，测试并保存；随后保存用户、账户和用量周期。完成条件：各页显示保存成功并回读一致；失败时旧配置继续生效。
5. 返回设备查看数据；普通刷新读取已有快照，账户卡手动按钮按第6.1节采样。完成条件：所选用户、范围、更新时间及数据状态可辨认。

### 10.3 传输与内存实现

- 业务`HttpRequest`对实际网络对象执行`dynamic_cast<EspNetwork*>`；非ESP对象在读取Key或`CreateHttp`前拒绝。能力约束留在业务内，`network_interface.h`与`esp_network.h`保持上游接口；携带Key的请求不跟随重定向。
- 真实站点曾在`WE1 → GTS Root R4（交叉签名）→ GlobalSign Root CA`链上握手失败。启用`CONFIG_MBEDTLS_CERTIFICATE_BUNDLE_CROSS_SIGNED_VERIFY=y`后以包内已有GTS Root R4为可信锚验证；SDK标注约增加700字节堆，TLS继续使用PSRAM分配。主机名、签名和有效期检查保留，禁止将服务端任意根证书直接加入信任。
- HTTP分块解析等待完整的块后CRLF再读取下一长度；空、非十六进制、溢出或无效分隔符返回错误。TLS/TCP任意分片边界由第12节传输回归覆盖。
- TLS包装层用`shutdown`唤醒接收、等待任务退出，最终由SDK销毁上下文并唯一`close`。提前释放描述符再由SDK关闭可能误关复用该编号的配置HTTP连接；分配失败返回ENOMEM并清理已获资源。
- 配置Cookie使用`ink_session_http`，避开旧HTTPS版Secure Cookie；属性规则见第6.2节。请求在配置失效时取消，NVS提交前再次检查授权。`httpd_stop`失败保留句柄并重试，成功停止后释放资源。
- HTTP服务限2个连接和2个待接入连接，启用空闲连接回收；任务栈8 KiB留在内部RAM以支持NVS/Flash访问，出站读取缓冲1 KiB。启动检查内部可用量48 KiB及最大连续块12 KiB，重请求检查可用量24 KiB。这些分配门槛用于拒绝无法承担的工作，第9节50 KiB历史低水位仍须单独验收。
- 运行态及UI仅持有凭据配置标志，请求时按站点绑定读取Key。缓存内容、隔离与降级规则见第6.3节；缓存写入失败不改变已成功提交的配置结果。

### 10.4 剩余上游差异及原因

相对`upstream/main@690a023`的保留接点如下。业务、配置片段和字形继续在InkMeter自有目录维护。

| 文件 | 保留原因 |
| --- | --- |
| [main/CMakeLists.txt](../../main/CMakeLists.txt) | 接入私有源码/配置及可选图标输入；保留e7bb308语言头生成。原应用源码列表沿用上游。 |
| [main/Kconfig.projbuild](../../main/Kconfig.projbuild) | 增加默认关闭的产品开关及启用时所需TLS/字号选择。 |
| [main/application.cc](../../main/application.cc) | 按配置保护原厂初始化、激活与后台任务；启用时在既有`DispatchClockTick`调用私有校时服务，关闭时保留原逻辑。 |
| [main/display/lv_adapter_epdiy.cc](../../main/display/lv_adapter_epdiy.cc) | 仅启用配置引用空适配；默认使用真实语音应用声明。 |
| [main/display/screen/book_screen/book_nav.cc](../../main/display/screen/book_screen/book_nav.cc) | 只在启用配置的按键描述中断开百问长按入口，导航helper保留。 |
| [main/display/screen/home_screen/home_screen.cc](../../main/display/screen/home_screen/home_screen.cc) | 按配置选择原应用列表或InkMeter入口及BOOT行为。 |
| [main/display/screen/home_screen/pixel/home_pixel.cc](../../main/display/screen/home_screen/pixel/home_pixel.cc) | 默认保留待办/原图标映射；启用时适配精简应用列表。 |
| [main/display/screen/settings_screen/settings_screen.cc](../../main/display/screen/settings_screen/settings_screen.cc) | 只在启用配置隐藏对话设置，保留原厂枚举和回调代码。 |
| [main/display/screen/standby_screen/standby_classic.cc](../../main/display/screen/standby_screen/standby_classic.cc) | 条件显示原厂待办；启用配置提供不依赖清单应用的返回按钮。 |
| [main/display/screen/standby_screen/standby_screen.cc](../../main/display/screen/standby_screen/standby_screen.cc) | 按配置暂停/恢复原语音应用或InkMeter，保留默认待机和唤醒路径。 |
| [main/boards/common/storage/sd_log_writer.cc](../../main/boards/common/storage/sd_log_writer.cc) | 两行条件包含，使无转储配置也能编译；原厂转储导出逻辑保持。 |
| [components/esp-ml307/include/http_client.h](../../components/esp-ml307/include/http_client.h)、[components/esp-ml307/src/http_client.cc](../../components/esp-ml307/src/http_client.cc) | 独立公共修复：块后CRLF分片、严格长度解析、错误终止和等待者唤醒。 |
| [components/esp-ml307/src/esp/esp_ssl.cc](../../components/esp-ml307/src/esp/esp_ssl.cc) | 独立公共修复：分配失败清理及shutdown→等待→SDK唯一关闭。 |
| [main/assets/lang_home.cc](../../main/assets/lang_home.cc)、[main/assets/lang_config.h.in](../../main/assets/lang_config.h.in)、[main/idf_component.yml](../../main/idf_component.yml) | e7bb308干净构建修复：匹配语言声明、校验条目数量、补齐现有板级依赖。 |
| [README.md](../../README.md)、[README_zn.md](../../README_zn.md) | 可选构建入口及规格/验收指针；原产品说明保留。 |

另有`tests/wifi_tx_parser_test.cc`、`tools/tests/test_lifecycle_fixes.py`按用户决定删除；这是验证代码仅保留本机的维护选择，不改变固件编译和运行行为。

### 10.5 RTC无效恢复与自动校时

私有实现位于`main/inkmeter/inkmeter_clock.*`。`Application::DispatchClockTick`仅在`CONFIG_INKMETER_ENABLED=y`调用`ServiceClock()`；该入口用非阻塞CAS合并请求，向既有Network worker投递工作。实际DNS、UDP及RTC访问在worker执行，不增加常驻任务，也不初始化或接管SDK全局SNTP。

| 场景 | 调度与结果 |
| --- | --- |
| 后台校时 | BootReady、已联网且非待机、SoftAP、升级或致命错误时才投递；无需InkMeter页面保持前台。正常间隔6小时，等待已有联网窗口，不为周期校时主动唤醒Wi-Fi |
| 前台HTTPS时间门槛 | 系统时间不在支持的2024–2099日历范围时，先`EnsureClock`；私有HTTP门槛通过前不读取请求凭据或构造客户端，非ESP对象仍按第10.3节拒绝 |
| 用量等待对时 | 返回`Deferred`，使用至少5秒及校时服务冷却中较晚的单调时间再试；保留用户分钟周期，不进入普通用量失败的指数退避 |
| 校时失败 | 每次选择一个服务器，失败后轮换；冷却依次30、60、300秒，使用单调时钟；取消、队列忙或不可联网按有界路径返回，避免立即失败的联网调用自旋 |
| 系统时间恢复 | NTP得到UTC后仅加一次8小时写系统，保留原厂香港伪epoch及TZ语义；InkMeter仍在`UtcNow()`入口归一化为UTC。日历边缘触发`next_sync=0`，不清除三类服务端冷却 |
| RTC回写 | 复用`Pcf8563::SyncSystemToRtc`并以`GetTime`回读VL、有效日历及系统时间差（允许±2秒）；RTC失败不撤销已恢复的系统时间，60秒后在下次已有联网窗口重试回写 |

校时使用IPv4 UDP/123，候选主机为`ntp.aliyun.com`与`time.cloudflare.com`，分别见[阿里云NTP配置说明](https://help.aliyun.com/en/eci/user-guide/configure-the-ntp-service)与[Cloudflare NTP用法](https://developers.cloudflare.com/time-services/ntp/usage/)。本实现是未认证的普通NTP，nonce只关联请求与响应，不提供服务器身份或防篡改保证；沿用自用可信局域网边界，不能将其描述成NTS或可信时间证明。

单次同步受总8秒绝对截止时间约束，DNS等待最多3秒、UDP等待最多4秒；联网与UDP取消检查以至多100 ms切片。DNS通过TCPIP异步回调及固定生命周期单槽工作：超时/取消后，未结束的回调仍占该槽，避免栈指针或任务句柄失效及未决查询累积。NTP响应校验长度、版本、mode、LI、stratum、origin nonce及可支持时间范围，并处理2036年era翻转；这些检查不替代认证。RTC/I2C、真实UDP可达性及设备内存以真机记录为准。

本方案不修改原厂RTC/OTA驱动、全局TZ、根SDK配置或公共网络接口；默认`n`不链接校时模块。主机验证及实测边界见第12.5和13.11节。

## 11. 构建

### 11.1 独立配置与四组合矩阵

使用ESP-IDF 6.1及ESP32-S31工具链。根`sdkconfig.defaults`和`sdkconfig`保持上游原样，`CONFIG_INKMETER_ENABLED`默认`n`。InkMeter专用片段为`main/inkmeter/sdkconfig.defaults`，显式启用InkMeter并关闭Flash/UART转储、选择NONE；`main/inkmeter/sdkconfig.global.defaults`只切换Global板型。InkMeter片段同时保留已测试的2,096字节中断栈，避免SDK版本默认值变化带入额外栈预算调整。正式InkMeter配置仍强制要求无内存转储，不因配置隔离降低第6.2节安全约束。

在仓库根目录运行对应命令。默认片段按列出的顺序加载，后面的片段覆盖前面的值；每个组合使用独立SDKCONFIG与输出目录，避免旧本地配置混入。若目录已有SDKCONFIG，先核对其有效配置；defaults只用于初始化，不能据新增片段假定旧输出已切换。

InkMeter国内版：

```sh
idf.py -B build/profiles/inkmeter -DIDF_TARGET=esp32s31 -DSDKCONFIG=build/profiles/inkmeter/sdkconfig '-DSDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig;main/inkmeter/sdkconfig.defaults' build
```

InkMeter Global版：

```sh
idf.py -B build/profiles/inkmeter-global -DIDF_TARGET=esp32s31 -DSDKCONFIG=build/profiles/inkmeter-global/sdkconfig '-DSDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig;main/inkmeter/sdkconfig.defaults;main/inkmeter/sdkconfig.global.defaults' build
```

验证原厂路径时，先把根`sdkconfig`复制到对应输出目录作为SDKCONFIG；Global副本将`CONFIG_BOARD_TYPE_METALIO_E_INK4_PLUS`设为`n`、`CONFIG_BOARD_TYPE_METALIO_E_INK4_PLUS_GLOBAL`设为`y`。这样保留旧SDK配置的默认值元数据，避免全新defaults初始化把中断栈等参数改成当前SDK默认值；核验生成配置中的中断栈仍为2,096字节。以下原厂命令用于该已准备的隔离副本。

原厂国内版：

```sh
idf.py -B build/profiles/factory -DIDF_TARGET=esp32s31 -DSDKCONFIG=build/profiles/factory/sdkconfig '-DSDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig' build
```

原厂Global版：

```sh
idf.py -B build/profiles/factory-global -DIDF_TARGET=esp32s31 -DSDKCONFIG=build/profiles/factory-global/sdkconfig '-DSDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig;main/inkmeter/sdkconfig.global.defaults' build
```

完成条件：四组合的配置、编译源与链接路径均被检查；InkMeter两版必须构建成功并适配现有分区。默认配置若暴露上游基线已有问题，单独记录并对照基线，不混入本次配置重构；有效配置、编译列表与ELF同时证明`n`保留原厂应用、`y`包含InkMeter并排除D25应用；InkMeter两组合禁用coredump且保留出站TLS验证。分区、resources/font_data与eFuse保持原布局。构建不烧录硬件，当前本轮验证状态见第13.9节。

构建描述使用SDK应用版本及ELF摘要前8个十六进制字符；CMake不额外探测Git。镜像归属以实际产物摘要与配置清单记录，不能仅由分支名推断。

### 11.2 字形生成

默认配置通过构建目录中的筛选输入排除InkMeter图标，避免原厂镜像残留36,864字节图标像素；启用配置沿用原输入。原厂图标生成脚本及资源分区保持原样。

InkMeter图标、18 px备用标签、52 px Token及112 px主数字随app嵌入，其余25/30 px正文/标题复用现有字体包，不修改resources/font_data分区。新增中文界面文案后可重生成子集字体：

```sh
python tools/generate_inkmeter_fonts.py \
  --source main/display/font/font_misans_regular_160_2.c \
  --output main/inkmeter/inkmeter_digits.c \
  --token-output main/inkmeter/inkmeter_tokens.c \
  --fontpack use_font/font.fontpack \
  --labels-output main/inkmeter/inkmeter_labels.c \
  --labels-sources main/inkmeter/inkmeter.cc main/inkmeter/inkmeter_model.cc \
    main/inkmeter/inkmeter_web.cc main/display/screen/inkmeter_screen/inkmeter_screen.cc
```

## 12. 自动验证

按用户本轮决定，验证代码、夹具、二进制和输出只放本机`build/`（Git忽略目录），保留仓库中测试代码的删除。以下路径为本机工具，新checkout需按第9节在`build/`准备对应夹具；旧二进制不能替代对当前源码的重编译。

### 12.1 本轮最小回归

在仓库根目录运行；额度检查需要Linux/WSL的g++及已解析的cJSON组件：

```sh
node build/ponytail-core-regressions/save-draft.cjs
python3 build/ponytail-core-regressions/quota-plan.py
python3 build/inkmeter-time-regressions/profile-run.py
```

完成条件：浏览器4场景通过，保存期间的后续编辑仍为未保存；真实`SyncAccount`函数的5场景以ASan/UBSan和`-Werror`通过，429/重试延迟停止后续采样并保留部分成功结果。两项均已对修复前版本复现失败。

时间回归链接真实模型并提取当前服务函数，以固定原厂本地时钟和HTTP/SD桩覆盖显示、香港16:00/午夜换日、跨月年范围、远端时间戳、GMT重试、200/304及额度接收时间、过期判断、配置探测和缓存版本恢复；输出位于`build/inkmeter-time-regressions/`。桩不覆盖真实网络、RTC硬件或真机缓存恢复。

### 12.2 共享传输

本机夹具为`build/http-chunked-test/`及`build/esp-ssl-memory-test/`；本轮网络边界复核记录为`build/ponytail-network-review/review.txt`，旧传输记录为`build/ponytail-transport-review.txt`。重新验证时将可执行文件输出到`build/`，并确认TLS夹具中的构造、析构、Connect、Disconnect、Send与当前源码一致。

完成条件：真实HTTP客户端的任意分片、无效尺寸/CRLF/溢出、截断和固定长度兼容通过；TLS分配失败、shutdown→等待退出→SDK唯一close及描述符复用检查通过。TLS夹具替换了ReceiveTask，不覆盖真实接收任务、证书链或设备并发峰值。

### 12.3 完整回归范围

涉及以下领域时，在`build/`维护相应夹具并按第9节记录证据：

| 改动 | 完成条件 |
| --- | --- |
| 模型、查询、缓存、平台适配 | 当前模型源码通过ASan/UBSan；用户隔离、名称迁移、精度/日期/零值、1 MiB缓存边界、13平台与跨挂载调用计划均有断言 |
| 手机配置 | 五页fixture覆盖配对、Cookie、草稿/失败保存、搜索、输入验证、XSS、过期撤销；全部请求命中设备桩，外部请求0 |
| 原生UI | 当前页面、模型、二维码和字形与真实LVGL重编译；1212×680布局截图对照原型，最终I1中每个非零Token段内部有可见像素；触控、滚动和生命周期断言通过 |

既有完整回归证据保留在第13节，其原测试源已按用户要求移除。原生字体加载器若曾用`DESKTOP_LOADER=/nonexistent`检查fallback，重新配置时清除该缓存以恢复自动检测；WSL DrvFS下seek加载耗时不代表设备渲染性能。

### 12.4 配置矩阵检查

四组合构建完成后，在仓库根目录运行`python3 build/profile-validation/validate.py --report build/profile-validation/matrix-final.json`。本机验证器核对有效配置、编译列表、链接符号、app容量和分区，并将结果写入`build/profile-validation/matrix-final.json`；平台分支复核记录为`build/profile-platform-review.txt`。完成条件是第11.1节的每个组合和对应断言均有成功结果；部分构建或旧产物只能记为待完成。此检查不连接设备，不替代真机验收。

### 12.5 自动校时回归

验证源、桩及输出继续只放本机`build/`。完整校时模块的编译参数、源摘要和结果在`build/inkmeter-clock-tests/review.json`；服务整合在仓库根目录运行`python3 build/inkmeter-clock-integration/run.py`，结果为`clock-integration.json`。

完成条件：当前校时模块以ASan/UBSan通过6组回归及19个NTP输入向量；服务整合50项通过，并记录实际源码摘要。模块桩覆盖调度、DNS/UDP、网络快失败与取消、单次时区偏移和RTC回写；服务整合提取真实服务函数及私有HTTP门槛、链接真实模型，时钟传输函数使用可控fixture。两者共同验证时钟失败不发送认证请求、恢复后使用正确日期、保留三类服务器冷却与配置会话撤销；均不访问设备、真实NTP或业务凭据。

## 13. 验证记录与待验收事项

状态截至2026-11-01。第13.1–13.10节保留各轮历史证据；第13.10节记录新近烧录后RTC无效的观测，本轮自动校时修复单列第13.11节。`build/`内的JSON、日志、图像与镜像属于本机产物，未纳入版本控制；复现时按第11–12节重新生成，并在真机测试中记录新的摘要和时间。

内存口径：采样最低值取自周期日志，不是瞬时最低值；SDK的`heap_caps_get_minimum_free_size`累加各匹配内存区的独立历史最低值，这些值可能发生在不同时间。稳定采样与历史低水位分别报告，50 KiB验收目标仍按第9节执行。请求完成数与被接受快照数也分别统计。

### 13.1 镜像与主机验证

| 镜像 | app字节 / 5 MiB分区余量 | 验证范围 |
| --- | --- | --- |
| UI国内版 | 5,151,920 / 90,960 | 构建、烧录校验、首页启动及第13.3节短时复核 |
| UIGlobal版 | 5,145,200 / 97,680 | 构建，未用于本次真机测试 |
| 网络/存储基线国内版 | 5,165,184 / 77,696 | 构建、ota_0烧录及第13.2节30分钟测试 |
| 网络/存储基线Global版 | 5,158,464 / 84,416 | 构建，未用于本次真机测试 |

| 镜像 | app SHA256 | ELF SHA256 |
| --- | --- | --- |
| UI国内版 | `96a4856a4935093f4a3ce4a9badb1244a291167be6f4894e7a95780e6cc4c4f1` | `052cd6b09f4087f9f34b91fe5b181301d8999596522e311aff4cc009c98c23e4` |
| UIGlobal版 | `180501345dd28876917ddd8b17fa49db1801dc2252e2e4e46528213af9b178cf` | `488eba62f8d9e9c915ccf50781857258c4f1f75ba8b5e692701ae096dce05d0e` |
| 基线国内版 | `dea3030b9fec92d405a8f3bae0435fa6861aa457cb80d90ab299776d393f2be5` | `c602abccbe70c040adb605f1185e420e0ecc37e8f4eb805ca7561a485873ee6d` |
| 基线Global版 | `9c8856006f83370a58455ef3a9538404933a66913dc6c4b80db01b1296ce6bf6` | `6fc2d3ac3360b83294f80b6ac3bba4a3fe4bc030ed2a683e38108afb5b5b65d0` |

UI构建清单为`build/inkmeter-ui-firmware.json`，烧录/启动记录为`build/flash-com7-ui-reference.log`、`build/inkmeter-ui-boot.log`。基线清单为`build/sub2api-final-firmware.json`，烧录记录为`build/flash-com7-sub2api-final.log`。COM7设备为ESP32-S31、16MB flash，仅写ota_0应用；NVS配置保留，日志中的启动ELF摘要与对应镜像匹配。

| 检查 | 已有结果 |
| --- | --- |
| 国内/Global链接与资源 | 3个移除应用实现均未链接，分区未改，保留出站TLS/交叉签名验证，无HTTPS服务端和本地证书生成符号 |
| 核心契约 | `-Wall -Wextra -Werror`、ASAN/UBSAN通过，包含名称迁移和真实1 MiB缓存容量边界 |
| 原生LVGL | 完整ctest 1/1通过；四页截图、52 px数字、I1段内部像素、用户名/长行、指针/滚动、零/缺值及待机/HOME已检查；两种字体加载器均通过 |
| 手机配置 | 五页浏览器回归通过，18次请求全部命中设备桩，外部请求0 |
| 传输故障回归 | TLS分配失败、唯一关闭、HTTP停止失败重试及任意分片的chunked解析通过 |

### 13.2 真实站点与30分钟持续验证（网络/存储基线）

使用第13.1节网络/存储基线国内镜像，以临时1分钟用量周期运行，结束后恢复15分钟。账户仅读取已有快照，期间未触发供应商采样。

| 检查 | 当前证据 |
| --- | --- |
| 显示周期NVS保存 | 真机保存临时周期返回200、读回一致；提交0分钟返回400且保留有效值；原15分钟值跨烧录/重启保留。本轮临时1分钟测试结束后恢复15分钟PUT及读回均200，站点、用户1和12个账户选择保留 |
| 主机直连部署实例 | 用户/账户列表均200，当前各12项；只读取必要元数据，不保存完整响应 |
| 修复前设备连接测试 | 返回502；传输诊断为`0x801a`、找不到匹配可信根，未把连接失败记作成功保存 |
| 交叉签名证书链 | 启用SDK已有支持并保留校验后，真机`POST /api/site/test`返回200，耗时5,109 ms；设备真实请求日志状态200、传输错误码0 |
| 站点与监控配置保存 | 站点PUT返回200，回读站点匹配且仅返回已配置标志、不返回凭据；用户1、12个账户、1分钟周期分别保存200，账户保存约21.56秒，配置回读全部匹配 |
| 真实用量接收 | 设备日志确认`usage accepted buckets=7 models=8`，证明快照已解析并接受；未据此认定当前所选范围为今日，也不代替金额/Token逐项对账 |
| HTTP分块边界 | 真实完整客户端的主机分片回归已通过；该独立解析缺陷不是已证实的本次TLS失败原因 |
| TLS关闭所有权 | 修正包装层提前close后SDK再关闭的描述符复用风险；有效描述符、OOM、HTTP停止故障测试均通过；本轮真实TLS/HTTP运行未观察到崩溃或持续内部堆基线下降 |
| 30分钟持续测试 | 2026-10-31 05:17:15.646–05:47:15.665（香港，UTC+8），约30.0003分钟；71次已记录上游请求均200且传输错误码0，29次用量快照被接受；30次配置检查均200、配置一致，无检查错误，不触发供应商额度同步 |
| 本轮内存与错误标记 | 591组内存样本，内部可用量采样最低63,871字节、中位86,807字节；前后各5分钟中位相同，最大连续块31,744字节。区间无panic/重启日志标记；固件历史低水位35,316字节整个区间未变，仍低于50 KiB正式目标 |

脱敏结果在`build/live-sub2api-validation.json`，按正式计时范围分析的统计在`build/memory_summary.json`，曲线为`build/sub2api-soak-memory.png`，源诊断为`build/sub2api-soak-final-memory.log`。同一记录中的上游请求完成数与接受快照数不是一一对应的自动同步次数。

| 内存池 | 采样最低可用量 | 可用量中位数 | 前后5分钟中位数差 | 最大连续块 | 区间内观察到的固件历史低水位 |
| --- | --- | --- | --- | --- | --- |
| 内部8位堆 | 63,871字节 | 86,807字节 | 0字节 | 31,744字节 | 35,316字节 |
| 内部DMA堆 | 36,995字节 | 54,867字节 | 0字节 | 25,600字节 | 3,376字节 |
| PSRAM | 13,145,088字节 | 13,209,600字节 | -1,024字节 | 未记录 | 最低13,094,912字节 |

内部堆后25分钟的每分钟中位数趋势为0字节/分钟；PSRAM在预热后每分钟中位数主要为12,900/12,901 KiB，前后窗口差1 KiB。PSRAM日志只有1 KiB采样精度，这些结果支持“本区间未见持续内部堆下降”，不能证明零泄漏或更长时间稳定。采样最低值不是瞬时最低值，固件历史低水位也不一定由本区间造成；35,316字节的历史值仍使50 KiB正式内存目标未通过。

### 13.3 modern UI 真机短时复核（2026-10-31）

香港时间11:30:27–11:32:48，用户重新打开用量分析页并点击刷新，确认柱状图和用户名均正常。镜像为第13.1节UI国内版。脱敏记录位于`build/inkmeter-ui-live.log`，结构化结果为`build/inkmeter-ui-live-validation.json`。

- 25次真实上游请求均为HTTP200、传输错误码0；捕获1次完整用量快照解析成功。
- 10次normal屏幕刷新耗时413–697 ms，中位数420.5 ms；5次wipe-bw耗时921–1,215 ms，均为`err=0x0`，未见panic。
- 内部堆采样最低76,367字节；网络任务退出后连续5次均为96,195字节（约93.9 KiB），最大连续块43,008字节。PSRAM由请求期间最低12,849 KiB回到12,917 KiB；调用者任务栈最低余3,348字节。
- SDK历史低水位为内部堆31,948字节、DMA堆8字节，正式50 KiB目标仍未通过。本次DMA采样最低44,523字节；历史指标的含义见第13节开头。

本轮没有重新烧录，也未通过测试工具写入配置；应用按已实现规则刷新用户显示名。新UI尚未重复30分钟测试。

### 13.4 尚需设备验收

| 待验收项 | 完成证据 |
| --- | --- |
| 新UI持续运行与内存 | 用新镜像重复30分钟真实请求，记录启停/并发/恢复的内部可用量、最大连续块、SDK历史低水位及任务栈；50 KiB目标达标后方可关闭内存验收项 |
| 硬件交互与显示 | 第9节的触控/盖板映射、方向恢复、残影/局刷合并、待机/唤醒均有真机记录；当前用户确认仅覆盖柱状图、用户名和一次刷新 |
| 配置压力与手机体验 | HTTP反复启停、与出站HTTPS并发、真实手机五页配置及会话撤销均通过；主机fixture不替代此项 |
| 持久化与恢复 | 有卡/无卡、USB导出、坏缓存、NVS满、换站点/Key及断电重启按第9节验证 |
| 实例兼容 | 确认部署版本，并按同用户/范围逐项核对Token、费用、请求、模型与UTC+8分桶；13平台fixture之外的真实平台能力按部署账户验证 |
| 发布 | 独立设计产品发布及OTA分发；刷回原厂app会覆盖InkMeter |

### 13.5 早期诊断索引

以下记录用于定位已修复问题，不作为当前镜像的验收结果：

| 阶段 | 本机证据与范围 |
| --- | --- |
| 首次HTTPS版 | `build/flash-com7.log`、`build/boot-com7.log`；随后因无法开启HTTPS配置服务转为HTTP。原分区/NVS/OTA元数据备份`build/flash-backup-com7/metadata.bin`可能含凭据，不应分享 |
| HTTP入口检查 | `build/flash-com7-http.log`、`build/boot-com7-http.log`、`build/inkmeter-http-live.json`；页面200、未认证401、跨源403、3客户端LRU和50次状态请求通过 |
| 配对与连续读取 | `build/http-pairing-validation.json`、`build/http-pairing-memory.log`；21次配置读取200、无Cookie401、Key未回读，结束后内部可用量恢复；后续保存/真实请求结果以第13.2节为准 |

### 13.6 ponytail审核与上游差异收敛

2026-10-31重新获取`upstream/main`与`origin/main`，两者均为`690a023`。本轮将19个公共文件恢复上游原样，相对main的已跟踪差异路径由47个减至28个（不含未跟踪的InkMeter新增文件）；保留`develop e7bb308`必需的干净构建修复，以及用户明确要求的测试删除。

CMake保留上游应用源码列表，在统一位置排除三个应用；共享调用通过原类声明上的inline空适配收敛，入口与启动后台任务保持停用。未使用字体和重复默认配置已清理。两个实际问题已修复：异步保存固定提交快照；账户多路采样遇429或重试延迟停止余下请求。

本机回归：手机保存4场景、真实同步函数5场景（ASan/UBSan、`-Werror`）均在修复前复现失败、修复后通过；共享HTTP分片和TLS分配/描述符所有权复核通过。验证代码及记录仅保留于`build/`。

构建与链接结果以`build/ponytail-firmware-review.json`为准；国内与Global均通过，分区表与上游布局一致。国内镜像5,152,448字节、分区余90,432字节，app SHA256为`1267d4c47e714d5f6d87c51615c17ee5516806ddbba59fe410f8b9059f4a3787`，ELF SHA256为`0cd93ce5fcb06c4f4eaf7c71e1c6a029f37af18c53fe590969bb42ab51ec32a9`。最终编译列表与ELF确认三个应用实现、聊天激活轮询、HTTPS服务端和证书生成均未带入，出站TLS校验保留。Global镜像5,145,728字节、分区余97,152字节，app SHA256为`ab70c4cf93f1991d6270d96909b18310921804ee6071769b59c7ea1681bb70b0`，ELF SHA256为`3e04342bb4062fab0c6cd982caf57445ed15bdf3196178b2c92dac220af58112`。

本轮没有烧录、真实站点请求或真机内存测试，也未重建已删除的完整原生UI/手机/模型测试源；布局仅删除未用字段与零偏移量。第13.1–13.3节的镜像、30分钟基线和短时复核仍只属于其原镜像，第13.4节的设备验收缺口继续保留。

### 13.7 InkMeter香港时间修复

2026-10-31按第6.1节时间契约收敛系统时间读取，仅修改InkMeter三个源码文件及本文。原厂OTA、RTC文件与本轮开始时逐字节一致，且相对`upstream/main`无差异；国内/Global配置内容保持不变，统计查询继续传`Asia/Hong_Kong`。缓存升级与配置保留规则见第6.3节。

旧源码已用固定原厂本地时钟复现香港16:00显示次日00:00、16:00提前换日及GMT重试剩余90秒误算为0。修复后36项时间/日期/接收/缓存断言以真实模型和当前服务函数通过ASan/UBSan及`-Werror`；原有5项额度限流回归亦通过。夹具、修复前快照、结果、源码差异与镜像摘要保留于`build/inkmeter-time-regressions/`，汇总为`review.json`。

| 配置 | app字节 | app分区字节 | 剩余字节 | 构建与分区检查 |
| --- | ---: | ---: | ---: | --- |
| 国内 | 5,152,432 | 5,242,880 | 90,448 | 通过 |
| Global | 5,145,696 | 5,242,880 | 97,184 | 通过 |

两版ELF均包含`inkmeter::UtcNow()`，分区表摘要一致。构建日志为`build/inkmeter-time-domestic.log`与`build/inkmeter-time-global.log`。后续国内版烧录与时间恢复验证见第13.8节；SD旧缓存升级、断电及真实午夜跨日仍待真机验收，既有镜像的持续运行记录不替代此项。

### 13.8 国内版COM7烧录与香港时间恢复

2026-10-31按用户要求将第13.7节国内镜像烧录到COM7。设备为ESP32-S31，现有分区表与镜像一致、有效OTA选择指向`ota_0`。仅写`0x80000`应用区，5,152,432字节，写后哈希校验通过；app SHA256为`f991a1ba67189cfbbbfe05b98de3070ba68ec0ab8a74f50ac2c78988ae62ef1a`，两次启动ELF摘要均匹配`44ad2bf43…`。

| 验证项 | 证据与结果 |
| --- | --- |
| 配置保留 | 启动应用前读回`0x8000–0x10000`共32 KiB，与烧录前逐字节一致，包含分区表、16 KiB NVS、OTA数据及PHY区；两次启动沿用保存的Wi-Fi，InkMeter使用现有凭据与选择请求成功，未通过配置API写入任何设置 |
| 香港时钟 | 首次启动RTC恢复为`2026-10-31 12:52:15`（香港），主机香港时间`12:52:02.438`；RTC有效标志正常，系统回读一致，无8小时时差 |
| 当前统计与页面 | 2次真实请求均HTTP 200、传输错误0；接受1份用量快照，13个桶、1个模型。当前香港日期为`2026-10-31`；用户按提示检查今日刷新及设置页后回复“已经正常了”，未提供逐项读数或截图 |
| 再次重启 | 通过USB串口RTS触发复位，RTC恢复为`2026-10-31 12:53:58`，主机香港时间`12:53:45.534`；RTC累计前进103秒，主机经过103.096秒，时间连续 |
| 联网前恢复 | 两次均先记录PCF8563恢复和有效HomeClock，随后约9秒才获取IP；恢复不依赖本轮网络校时，监听期间未观察到崩溃 |

RTC相对电脑稳定快约13秒，本轮未调整RTC。此次为复位恢复验证，未做断电、真实午夜跨日、30分钟持续运行或SD缓存文件检查。烧录/读取记录、脱敏串口日志及结构化结果位于`build/inkmeter-time-device/`，汇总为`validation.json`；备份元数据可能含现有凭据，仅保留本机。串口监听已停止，设备运行应用。

### 13.9 可选构建配置与四组合验证

本轮按D28将InkMeter改为默认关闭的独立配置，恢复默认原厂路径；专用配置、条件编译与网络边界的设计见第10–12节。本轮尚未烧录，设备上第13.8节镜像的烧录和时间恢复结果继续作为该历史镜像的证据。

| 检查 | 当前状态与完成证据 |
| --- | --- |
| 四组合构建与链接 | 全部通过；`build/profile-validation/matrix-final.json`记录实际配置、源码、尺寸、摘要、符号及分区检查。根SDK输入哈希未变，原厂镜像无InkMeter网页/源码/图标，InkMeter镜像无三个停用应用实现 |
| 原厂/InkMeter平台分支 | 本机预处理与适配器检查通过：9份共享源码主体在默认国内/Global分支与e7bb308基线一致（预处理比较省略include），真实类头的开/关编译与断言通过；记录为`build/profile-platform-review.txt`。最终四组合链接与源码选择亦通过矩阵核验 |
| 网络边界与公共修复 | 真实类声明及生产门槛片段、构建描述、HTTP分片与TLS故障回归通过；非ESP在HTTP凭据读取/CreateHttp前拒绝，两公共接口保持上游；记录为`build/ponytail-network-review/review.txt` |
| 保存、额度与时间回归 | 保存4项、限流5项、时间36项通过；时间夹具使用`profile-run.py`适配本轮根配置恢复，原时间行为断言保持不变；验证源仅在`build/` |
| 图标与配置阶段 | early-scan未知/关/开、输入隔离、陈旧缓存和真实生成器回归通过；去掉扫描保护或清理逻辑的变异版本被拒绝，见`build/profile-validation/icon-inputs.json` |
| 真机与内存 | 本轮未执行；第13.4节缺口及50 KiB目标未通过的结论继续有效 |

四组合实际产物如下，app分区均为5,242,880字节；四份分区表与上游布局一致。

| 配置 | app字节 | 剩余字节 | 构建/源码/链接/资源检查 |
| --- | ---: | ---: | --- |
| InkMeter国内 | 5,152,624 | 90,256 | 通过 |
| InkMeter Global | 5,145,888 | 96,992 | 通过 |
| 原厂国内 | 5,078,048 | 164,832 | 通过 |
| 原厂Global | 5,070,832 | 172,048 | 通过 |

| 配置 | app SHA256 | ELF SHA256 |
| --- | --- | --- |
| InkMeter国内 | `9c7387c9e24a70acd6837eee2fddda4de5c4c6de1492f892eaacdbceb7c15608` | `b8130d99778b0fd0a32178219213f10e1d5cb729b827169f7061bcc3817d30ad` |
| InkMeter Global | `1fae142a8c38484ffec4cf4b04846284f5b506b04814b7876d4070c1830cb7b4` | `581ad2184616d3940c79d6f044d3d6b07da96f13edf021e1039d70aaa4b2d506` |
| 原厂国内 | `d94fc5aeb89fe27e90ec151324700f8bf194467943fc3db050b1e093acb9c435` | `a083ff2b6f7e8dd83270853d7d798c71dbe8938cb6b2c2b27f54f9f47d078965` |
| 原厂Global | `357524f85aa17b662c98a6361a077af99a52066a343ea847f51b84cf092adc38` | `465b1fa23e293a90d1c2a671805e4610b407828cb02d9c8ee7b25d8c6025e6e8` |

原厂Global沿用上游隐藏WorkBuddy首页入口的行为；其原始源码及真实待机实现仍编译/链接，验证器不把入口被GC当作缺失功能。两版InkMeter的共同有效运行配置与此前已测试配置一致（板型选择除外），记录为`build/profile-validation/runtime-config-preservation.json`。

构建日志和退出状态在`build/profile-validation/build-results.json`；本机测试与矩阵不代替第13.4节真机验收。旧镜像的构建、烧录、时间恢复和持续测试记录继续只属于其原镜像。本轮没有提交、烧录或进行真实站点请求。

### 13.10 独立InkMeter国内版烧录（2026-11-01）

使用四组合校验通过的`build/profiles/inkmeter/Metalio-E-Ink4-Plus.bin`烧录COM7同一设备。仅写`ota_0`的`0x80000`应用区，5,152,624字节，app SHA256为`9c7387c9e24a70acd6837eee2fddda4de5c4c6de1492f892eaacdbceb7c15608`；写入校验通过。设备分区表与构建一致，烧录前后`0x8000–0x10000`共32 KiB逐字节相同，包含16 KiB NVS和OTA选择；资源、字库、bootloader及eFuse未写入。

重启日志确认从`0x80000`加载，ELF前缀`b8130d997…`匹配该镜像，进入原厂首页idle并连接保存的Wi-Fi，监听区间未观察到panic。启动时PCF8563报告VL低电压标志，固件跳过不可信RTC时间，观察到本地时间1970-01-01；本次未观察到自动校时成功。该项是启动观测，尚不能确定发生时间或硬件原因，不能沿用第13.8节的有效时钟结论。

本轮未修改配置API或RTC寄存器，未验证InkMeter页面交互、真实Sub2API请求及长时间内存；需恢复可信时间后再验证HTTPS同步。烧录、元数据对比及脱敏启动记录在`build/flash-20261101-profile/`，汇总为`validation.json`。元数据备份可能含现有凭据，仅保留本机；串口监听已关闭。

### 13.11 RTC无效后的自动校时修复

2026-11-01针对第13.10节RTC VL无效后无法恢复HTTPS同步的问题，按D29增加私有自动校时及服务整合。以下为源码、本机fixture、最终构建与真机验证证据；设备唤醒重新连接后已完成应用区烧录、NTP/RTC恢复和真实Sub2API同步，不沿用旧镜像结果。

| 检查 | 当前证据与边界 |
| --- | --- |
| 完整校时模块 | 已通过ASan/UBSan的6组回归、19个NTP向量（7有效、12无效）；记录为`build/inkmeter-clock-tests/review.json`。含联网立即失败的节流/取消及power hold释放，最新受测`inkmeter_clock.cc` SHA256为`d02eb757da63140f886b4b669b3d106c9a44d8d3d2efc0da7f86b263cde31660` |
| 服务整合 | 50项通过ASan/UBSan及`-Werror`，覆盖日期、HTTP门槛、Deferred短重试、缓存、会话撤销及冷却保留；结果在`build/inkmeter-clock-integration/clock-integration.json`。此夹具的时钟传输为桩，完整模块结果另记上一行 |
| 最终四组合构建 | 全部通过；两版InkMeter已核对最后受测源码与对象文件新鲜度，实际配置/源码/链接/资源/分区及摘要见`build/inkmeter-clock-tests/matrix-final.json`；根SDK输入哈希未变，关闭配置不含私有校时模块 |
| 烧录与RTC恢复 | 通过：COM7仅写`ota_0/0x80000`，5,158,016字节，写入校验成功；NVS/分区表/OTA元数据逐字节相同。首次启动RTC无效，联网后约1秒经`ntp.aliyun.com`恢复香港17:54:46，RTC写回/VL回读成功、delta=0；重启在入网前恢复香港17:55:44、日历有效 |
| 真实Sub2API | 通过：校时后使用现有配置，2次真实出站请求均HTTP200且传输错误0，接受1份18桶/2模型快照；本轮无配置API写入、无Key回读或日志输出，未作金额/Token逐项对账 |
| 设备内存与长期运行 | 本轮仅短时启动/同步：首轮请求后内部可用95,755字节、历史低水位32,684字节，重启历史低水位35,840字节；50 KiB目标仍未通过。未做30分钟长测或物理断电保持测试 |

真实UDP、物理RTC/I2C及复位恢复结果见本节硬件记录；主机桩与短时真机检查仍未证明断电保持、6小时实时时序或设备峰值。普通NTP的未认证边界见第10.5节，历史第13.7–13.10节不作本轮修复已生效的证据。

| 配置 | app字节 | 剩余字节 | app SHA256 |
| --- | ---: | ---: | --- |
| InkMeter国内 | 5,158,016 | 84,864 | `244d2c30f48ff10cbdc6eff0e564c52eebae6177d896421c917267666add2246` |
| InkMeter Global | 5,151,280 | 91,600 | `09d58fbc3a55eaad3025046752d84815a20d5c715bfb6d07ae896c141d4972e8` |
| 原厂国内 | 5,078,048 | 164,832 | `f7cf1ed9298f012943367a9da6b50fe1d4b7b091678a849e65c1d22bbd7a588f` |
| 原厂Global | 5,070,832 | 172,048 | `94969b1bb77bfaf5ebe6cefd3d4a15aa7dc8e58c0abcabf4162f7124b5f4d718` |

本轮烧录app SHA256为`244d2c30f48ff10cbdc6eff0e564c52eebae6177d896421c917267666add2246`，ELF SHA256为`bc3fee928ce4c701c5b8b936464460fb8110859e958330e401c11f98b1b295f9`；两次启动前缀均匹配`bc3fee928…`。原厂RTC/OTA实现及根SDK配置未改；本轮仅增加InkMeter私有校时模块、业务守门与默认关闭配置下不生效的条件tick入口。

硬件汇总为`build/rtc-sync-device/validation.json`，含首次无效时钟恢复、重启前网络恢复顺序与实际请求计数；烧录及元数据对比、脱敏日志同目录保存。备份元数据可能含现有凭据，仅保留本机。串口监听已关闭，设备按原厂启动流程回到首页，未自动提交代码。
