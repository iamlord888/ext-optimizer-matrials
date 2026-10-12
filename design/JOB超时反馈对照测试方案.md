# JOB 超时反馈对照测试方案 / JOB Timeout Feedback Test Plan

日期：2026-09-11。状态：上一轮 JOB 与正确 TPC-DS SF1 A/B 已完成并发现 Rows target 遍历、跨轮 EXACT 和 Nested Loop 兼容差异；代码已按 master 语义修订，等待重新构建后全量重跑 A/B。

后续更新：远端独立实例已停止并清理，后续实验只使用本机 Docker `gaussdb-local`，不连接共享实例 `29000`。本机容器映射 `127.0.0.1:5432`，JOB 数据已存在。当前协议采用匹配 SDK 和成套 package 部署，`partial_capability(0)` 返回布尔成功值；真实 `statement_timeout` 与 `PQcancel` 必须得到 SQLSTATE `57014`、完整 DATA/END snapshot、drain idle 和预期 backend 复用/重连。不能沿用远端实例路径或其历史协议结论。

## 目标与范围 / Objectives And Scope

本次 A/B 运行 JOB 的 113 条 SQL 和 TPC-DS SF1 的 103 条逻辑 SQL，不运行 JOB-light、JOB-complex 或任何 scale factor 0.1 数据库。B/C/D 仍只运行 JOB 的 113 条 SQL。用户指定 master 为基线，当前 impl-refactor 工作区为候选，以 Make/package 构建验收。

1. A/B：验证集成超时机制但关闭单轮超时时，相对于 master 的调优行为、反馈及交付是否存在非预期回归。
2. B/C：在其余配置一致时，测量 10s 单轮超时上限对总调优时间、反馈覆盖、最终可交付计划和实际执行性能的影响。
3. D：对同一 candidate SQL、同一实际物理计划，比较完整执行与取消时的观测，优先检查 A-rows < E-rows 的节点是否被正确识别为已完成。分别报告执行状态正确性、evidence 正确性与 Rows target 可用性。

## 已完成的环境检查 / Read-only Preflight

| 检查项 | 实际结果 | 结论 |
| --- | --- | --- |
| Docker | 本机容器 `gaussdb-local`，镜像 `gaussdb-local:507.0.0-centralized-initialized`，状态需为 running | 通过 `docker inspect` 固化容器 ID、镜像 digest、端口和挂载 |
| OS/工具 | Docker 容器内 Linux x86_64；容器内 GCC/Make/Flex/Bison/Python 可用 | 在容器内原生 Make 构建 |
| 磁盘 | benchmark 产物统一写本机 `/Users/penggan/data/...`；容器临时构建留在容器工作区 | 不写宿主机公共 `/data`，测试前检查本机空间 |
| 数据库连接 | 本机 `127.0.0.1:5432` 映射到 `gaussdb-local`；正式 adviser 在容器内以 OS 用户 `omm` 走本地 socket/端口 `5432` | 避免把本地密码写入命令或产物；不连接共享实例 `29000` |
| SQL 客户端 | 容器内自带 gsql；adviser 在容器内通过本地端口 `5432` 连接 | 可直接执行测试 |
| GaussDB | `507.0.0 build d414f3bb` 及其匹配 SDK | 用于成套构建当前 `extopt.so` |
| 容器工作区 | 候选 `/home/omm/job-bench-current-20260909`；master 使用独立只读源码快照目录 | 正式构建记录容器路径和 commit/diff 指纹 |
| JOB 数据 | 容器 `job` 已有 21 表、44 索引、108 统计及精确行数验证记录 | 正式运行前只读复核，不重载数据 |
| 轻量查询 | title 主键过滤的完整 EXPLAIN ANALYZE、Outline、Query Block 正常，Total runtime 约 0.122ms；query_source 返回文本 | 基本执行/扩展能力通过，不代表全量 JOB 或新客户端已验收 |
| 现有 extopt | 必须安装本轮 package 中的候选 extopt | 精简后的 DATA/END、row5 和真实取消/恢复需要本轮重验 |
| 部署权限 | 本机 Docker 可执行，容器内 `/home/omm` 工作区可写 | 只操作 `gaussdb-local`，不接触其他实例 |
| 并发干扰 | 容器只用于本地实验，但仍与宿主机其它进程共享 CPU、内存和磁盘 | 单 worker、交替顺序并记录负载，不能视作物理独占机器 |
| SQL 集 | JOB `/Users/penggan/Downloads/cardinality_benchmark/job/sql` 共 113 文件；TPC-DS `/Users/penggan/Downloads/cardinality_benchmark/tpcds/sql` 共 99 文件、物化后 103 条逻辑 SQL | 正式运行前逐文件 hash、语句数、SQL manifest 固化 |
| TPC-DS 数据库 | `tpcds_sf01` 经行数核验实际为 SF0.1，所有相关结果已作废；`tpcds_sf1_partial` 虽名称带 partial，但 24 张业务表行数完整匹配标准 SF1 | A/B 只对 `tpcds_sf1_partial` 执行；不使用 SF0.1 结果 |

测试实例持久设置并已验证：query_dop=1、htap_router_mode=row、work_mem=64MB、shared_buffers=32MB、enable_thread_pool=off、statement_timeout=0、enable_adaptive_cost=off、enable_feedback_cardest=off、`enable_nestloop=off`、`enable_mergejoin=off`、`enable_index_nestloop=on`、`enable_hashjoin=on`。普通 Nested Loop 和 Merge Join 关闭，索引 Nested Loop 与 Hash Join 保留。客户端会设置 cost_param=2，并在每批开始前记录实际 GUC。

last_analyze/last_autoanalyze 为空不等于没有统计信息；需要在正式运行前对现有统计快照做内容指纹。不执行 ANALYZE/VACUUM/数据重载以免改变现有基线。

JOB 导入、extopt 安装和持久 Join GUC 设置已完成。四项 Join GUC 的 `source` 均已验证为 `configuration file`。后续 benchmark 查询只读；临时 HAPF 测试库不属于 JOB 数据。所有 benchmark 产物、日志、SQL 副本、指纹和报告统一写本机 `/Users/penggan/data/job-timeout-benchmark-*`，不写公共 `/data`。

## 实验定义 / Arms And Parameters

| 组 | 源码版本 | 单轮 EXPLAIN 超时 | 比较目的 |
| --- | --- | --- | --- |
| A | 当前本地 master `a02e94907a972b75f2ce599853cd71bec243b415` | 无此参数，保留原实现 | 未集成基线 |
| B | impl-refactor `6bc9013d5f4dbe03e2987ef6c7bbd1439725f352` + 当前工作区修改 | `--explain-timeout-ms=-1` | 关闭超时的回归对照 |
| C | 与 B 完全相同的二进制 | `--explain-timeout-ms=10000` | 开启超时的效果 |

三组共同参数（使用本机容器；四项 Join GUC 已持久配置）：

```text
-p 5432 -U omm -d job -s public -w
--verify-initial-plan=true
--search-timeout-ms=300000
--enable-extoptimizer=false
--sampling-blocks=1000
--patience-iters=3
--iter-min-improvement=0.10
```

不设置 target-time-ms，不增加 max-iterations，保持原有默认关闭迭代上限。每条 SQL 启动一个独立进程/会话，认证通过受保护 stdin/凭据文件，不把密码放在 argv、日志或 manifest。日志路径按版本、query、重复编号隔离。

动态采样保留。两版均使用 `TABLESAMPLE system(...) REPEATABLE (8642719)`，但仍需保证表物理布局、数据和统计未变化；对比实际 sampling SQL 和采样结果，不能仅因 seed 相同就默认一致。

### 需要明确的语义

- `search-timeout-ms=300000` 是每条输入 SQL 的搜索总预算，从参数解析后开始计时，包括连接、原计划验证、元数据和采样等前置消耗。外层进程耗时与 summary 调优耗时仍分别记录。
- C 的 `10000ms` 是上限，不是每轮固定执行 10s。实际值为用户上限、剩余预算扣除 8s 收尾预留、完整 best 的 `floor(1.25 * best_ms)` 三者最小值（并有最小 1ms 规则）。逐轮记录 effective_timeout_ms。
- verify-initial-plan 仍使用原始完整 EXPLAIN ANALYZE 路径，不受 C 的 10s 单轮反馈限制。若初始验证就用尽 5 分钟，C 也可能没有机会进入 Partial 调优。该类查询单独统计，不认定为超时机制无效。
- sampling、replan、cancel/drain、fallback、final plantrace 各有自己的耗时。既有 fallback/plantrace 使用独立约 10s 预算，外层 wall time 可略超过 300s，不把差额全部算作单轮执行时间。
- 关闭外置优化器并不表示不需要 extopt：query_source 仍是客户端初始化依赖，C/D 还需要新增 HAPF 服务端模块。

## 准备与放行 / Preparation Gate

1. 固化 A/B 源码快照、worktree diff hash、所有实际参与构建文件的 SHA256 manifest、编译器参数、客户端/libpq/optimizer SO/服务端 SO hash。不得只以 `6bc9013d5f` 表示尚未提交的候选实现。未跟踪但参与构建的文件必须纳入快照。
2. 使用容器内两个全新目录构建 A 与 B 的 Make/package，使二者使用相同 GCC/glibc；不复用来源不完整的历史包，不覆盖候选包。master 仅在隔离源码快照中加入同一 20MB 诊断日志宏 guard，不改变其调优逻辑。
3. 在 `gaussdb-local` 中由本机同步工作区，执行 `make -C src/backend generated-headers`，再用匹配 SDK 构建并安装当前 package 中的 extopt；保留旧 SO 备份并记录容器 ID、镜像 digest、服务端 SO hash。客户端与 extension 不是同一轮 package 时停止 C/D。
4. 安装后重新执行 `CREATE EXTENSION extopt`/`partial_capability(0)`、小规模受控取消、HAPF 完整性和恢复 idle 验证。所有安装、重载或重启只允许作用于 `gaussdb-local`。
5. A/B/C 全部使用同一容器、同一新 extopt、同一 Join GUC；A/B 分别在 `job` 与 `tpcds_sf1_partial` 上执行，C/D 只在 `job` 上执行；未 arm 时保持不捕获。先完成各 suite 5-10 条 pilot，再扩展到完整集合。
6. 冻结 SQL manifest、表/列/索引定义、统计内容、表大小与 reltuples/relpages、数据库版本及实际会话 GUC。实验前后和每批之间核对；业务写入、统计重采样、DDL 或重要配置漂移使对应配对失效。只读事务无法跨 adviser 多轮会话替代这项冻结要求。
7. 三组一律单 worker，不同时运行 A/B/C；`gaussdb-local` 持久配置已设置 `enable_nestloop=off`、`enable_mergejoin=off`、`enable_index_nestloop=on`、`enable_hashjoin=on`，每批启动前通过 `SHOW` 校验。测试命令不再通过 `-S` 重复设置这些服务端 GUC。
8. 小样本 5-10 条先验证 20MB 日志、参数、summary、完整日志、计划配对、超时清理；另用 300KB pilot 测截断率。确认两种日志模式均可归档后再扩展到 113 条。

### 日志完整性

生产默认 tuning_agent SQL 日志上限仍为 300KB。A/B/C/D 测试包统一用 `-DHINT_ADVISER_SQL_LOG_MAX_BYTES=20971520` 编译为 20MB，生产默认值不变。四组使用相同 20MB 上限，宏值、编译命令和客户端 SHA256 写入 manifest。

所有 benchmark 产物、日志、SQL 副本、指纹和报告统一写本机 `/Users/penggan/data/job-timeout-benchmark-*`，不写 `/data`。容器内临时工作文件使用 `/tmp` 或该主机目录的显式绑定挂载；不得修改其他容器或共享实例。

执行时复用 skill 的 SQL manifest、指纹和结果归档约定，在任务私有 Docker runner adapter 中增加三组参数、断点续跑及日志解析。保存 stdout、stderr、全部迭代 candidate SQL、PRETTY/JSON、summary 与最终交付 SQL；每条完成后立即把容器临时产物复制到宿主机任务目录，复制校验成功后才清理该条容器临时目录。

每条 SQL 完成后检查完整终止标记、最后一轮 candidate/EXPLAIN 配对和是否达到 20MB。缺终止标记、round 数大于 EXPLAIN 数、达到上限或日志写入失败均标记 `log_incomplete=true`，不得判定为缺 Hint 或节点错误；必要时同环境提高到 100MB 诊断重跑，诊断结果不混入性能统计。另用 300KB pilot 测截断率。

## 第一阶段 A/B / Disabled-timeout Equivalence

先对 JOB 113 条和 TPC-DS SF1 103 条逻辑 SQL 分 suite 逐 query 配对执行 A、B；每个 suite 内交替 AB/BA 顺序，避免全部 A 在冷缓存、全部 B 在热缓存。先完成一次全量筛查；有差异的 query 再以 BAAB 等平衡顺序重复，性能敏感/停止阈值附近至少重复 3 次。

### 比较层次

1. 输入：原 SQL、实际 GUC、sampling SQL/结果、初始 JSON/PRETTY 计划与 A-rows。输入不一致先查环境，不进入行为等价判定。
2. 每轮：准确 candidate SQL、规范化 Rows key/value 集合、实际物理计划签名、A/E、changed/继续停止决策。保留 quoted identifier 的大小写与字面内容，只规范无语义空白及有明确定义的 Hint 排序。
3. 最终：迭代次数、停止原因、是否完整 best、最终 Rows/Outline、交付状态、返回码；summary 字段名变化和新增 partial_iterations 不作为行为差异。
4. 性能：总调优 wall time、各阶段耗时、完整候选耗时只做统计比较，不要求浮点时间完全相同。

### 不能把“逐字相同”作为唯一通过标准

2026-09-11 已取消两项会改变 master Rows 行为的策略：Nested Loop 恢复 inner Scan 抑制、inner Join 保留；新 EXACT 恢复跨轮覆盖和同轮 first-wins。仍保留 `Parent Relationship` 的前置 InitPlan 角色修复、按输出值判断 changed、扫描身份与 quoted 名称解析。新的 A/B 应仅允许这些明确修复及计时/best 波动造成的差异。

最终 best/patience 还取决于实测时间，因此同版重复运行也可能选择不同 best。把差异分为：完全等价、已批准修复造成的预期差异、时间噪声/边界预算差异、环境不一致、未解释差异。需要严格隔离超时重构本身时，另做“master + 同样已批准修复”的诊断基线 A'；保留 A 原版结果，不用 A' 偷换基线。

放行 C：所有可比较终止样本无未解释的反馈/计划/交付回归，预期修复均有反例与依据，日志完整；时间噪声通过重复和独立最终计划验证解释。任何确定错误暂停 C。达到全局/外层超时且没有完整证据的 query 标记 inconclusive，不能计入对应 suite 的“全部等价”。

Rows 差异不直接等于 bug：按 skill 的五项 gate，用当前计划的正确 `(K*,V*)` 与 adviser 输出做手动反事实，确认目标可寻址、服务端未明确拒绝、手动 hint 实际修正 E-rows，而 adviser 未修正或值错误。新计划的结构身份不能用旧 plan ID 代替。

## 第二阶段 B/C / Timeout Cost And Benefit

第一阶段放行后，在同一测试窗口重新配对 B/C，不能直接把较早的 B 用作严格性能基线。按 query 交替 BC/CB，建议 3 个重复，记录系统负载；不使用清空全机缓存或重启共享实例的方式制造冷缓存。

指标：

- 调优总耗时中位数、P90/P95、总和；单 query 的 C/B 比值；原计划验证、采样、实际 ANALYZE、取消/drain、replan/proof、收尾阶段耗时。
- COMPLETE/PARTIAL/NO_TRUSTED/ERROR 次数、首个完整 best 时间、有效 Rows key 覆盖、EXACT/LOWER_BOUND/SAMPLING 数量、停止原因。
- 最终交付率、已有完整 best 的保留率、仅非 ANALYZE fallback 的交付率、无交付率，分别报告，不把 rc=1 全部视作崩溃。
- 有效收敛与“Partial 无新输出值而停止”区分，不能以 changed=false 单独证明全部基数已准确。

### 最终调优质量的独立测量

将 B/C 实际交付的 SQL 与原始 SQL 在相同 GUC/数据上单独执行完整 EXPLAIN ANALYZE，固定计划后至少 3 次，比较中位数、结果集和计划签名。用实际交付 SQL，不仅使用调优过程中记录的最小耗时，以免 best 选择噪声偏置结论。

完整验证的单次上限建议先设 900s，为独立验证预算，不修改 300s 调优参数；仍超时记为右删失/未知，不将 900s 当作真实耗时或速度提升。只对双方有完整测量的 paired queries 算几何平均加速比，另外报告覆盖率与全部 timeout/no-delivery 计数，避免幸存者偏差。fallback 0ms/空 best 不参加性能平均，必须独立执行后才能评价。

预先登记性能异常筛查线，例如 C 最终执行中位数比 B 慢 >10% 且 >50ms，进入重复与归因；不凭一次越线判定算法 bug。调优耗时减少和最终执行变快是两项目标，分别汇报，不合成单一分数。

## 第三阶段 D / Same-plan Node Evidence Oracle

### 抽取与计划固定

从 C 的每个实际超时轮次保存准确 candidate SQL（含当前 Rows/Outline），不能使用下一轮 SQL或重新组装的最终 SQL。以 query、candidate SQL hash、GUC/data 指纹、完整物理计划签名去重；同计划不同超时快照仍保留。

对每份 candidate：在相同会话设置下执行一次独立完整 EXPLAIN ANALYZE 作为参照，再以相同 SQL 运行原生 HAPF 10s 取消。两次实际计划必须在 operator、Join Type、relation/alias/schema/index、query block、父子角色、InitPlan/SubPlan、过滤/连接条件与 E-rows 上一致；仅时间、A-rows、内存等执行观测允许变化。不一致则拒绝配对或重新固定并重跑两边，不能强行按 preorder/plan ID 对齐。

主实验 C 保留自适应 effective timeout；D 的主对照通过专用采集器固定 statement_timeout=10000，关闭自适应 best 影响，以回答“同计划在 10s 时”的问题。必要时按 C 实际 effective timeout 再回放一次，结果单独标注。短于 10s 自然完成的计划记为 COMPLETE，不算缺失 snapshot。

### 每节点记录

```text
query / candidate_hash / plan_signature / execution_id / node_identity
node_type / parent_relationship / query_block / targets / index / predicates
E_original / A_partial / A_complete / loops_complete
executed / active / terminal / rescan_count
client_evidence / emitted_Rows_key_value / suppression_reason
snapshot_integrity / plan_match / oracle_verdict
```

使用原始观测 double 值，PRETTY 显示行数可能取整；只拿显示值时标记为显示精度验证。GaussDB 当前 adapter 的 ntuples/A-rows 是累计口径，不再乘 loops；`EXACT` 只在同一 epoch、无 rescan 的可比节点上验证，`A-rows > E-rows` 的 `LOWER_BOUND` 即使 active 或已有 rescan 也按下界验证。0 行事实与最终 Rows 最小 1 的渲染规则分开统计。

### 核心断言

| 快照条件 | 客户端期望 | 对完整参照的验证 |
| --- | --- | --- |
| 已执行、非 active、本地 terminal、无 rescan | EXACT，不论 A 与 E 大小 | 可比节点 A_partial = A_complete；重点 A_partial < E_original |
| 未完成且已执行，A_partial > E_original（active/rescan 均允许） | LOWER_BOUND | A_partial <= A_complete |
| 未完成且 A_partial <= E_original | 不产 evidence | 不能因低于估算就伪造 EXACT；A=E 同样不证明完成 |
| 未执行 | 不产 evidence | 无错误发布 |
| Nested Loop Inner Scan | 即使有本地 evidence也不生成 Scan Rows | 保留 master 行为；wrapper 后仍按角色抑制 Scan |
| Nested Loop Inner Join | active/已发生 rescan 且 A>E 时生成 LOWER_BOUND；A<=E 时无 evidence；terminal 且无 rescan 时可 EXACT | 接受首次 epoch EOF 后、未来 rescan 尚未发生的窄窗口误判风险 |
| Hash MultiExec | 不产 observation/Rows | 其普通子 Scan 仍应有机会提供证据；Hash Join 不排除 |

纠正一个口径：A>E 不是一律 LOWER_BOUND；已证明 terminal 且无 rescan 的节点即使 A>E 也应是 EXACT。active 或已有 rescan 只降低证据强度为 LOWER_BOUND，不应丢弃该下界。小于 E 的终结节点尤其有价值，因为它们无法靠下界规则兜底。

按 `A_complete < E_original`、等于、大于分别分层，再按 `A_partial/E_original`、Scan/Join/辅助节点、Nested Loop 内外侧分层，防止用大量低价值 A>E 样本掩盖 A<E 识别问题。

### 完成状态的独立真值边界

两次运行 A_partial=A_complete 只能证明行数相等，不能证明超时瞬间已经调用 EOF：节点可能刚返回最后一行但父节点尚未再次调用。HAPF terminal 也不能作为评估 HAPF terminal 正确性的标签。

因此分两级报告：

1. 主测试直接验证 EXACT 值、LOWER_BOUND 合法性、false EXACT/invalid bound、inner Scan 抑制和 inner Join master 兼容行为，并统计可用 A<E EXACT 数量。
2. 严格完成识别率/漏报率，需要独立 EOF/epoch oracle：优先对匹配 SDK 的隔离诊断实例，用调试符号/跟踪记录实际 tuple-return/EOF/rescan 事件，再在相同执行的取消点对照 HAPF；或使用能够从执行依赖严格证明子树已耗尽的场景（如已进入 Hash Join probe 阶段时的 build 子树），只对可证明节点统计。此诊断不加入主性能计时，不在共享远端实例上擅自 attach/改内核。

若无独立 oracle，仅给“行数证据正确率”与“结构可证明子集”的识别率，其他节点状态为 unknown，不能宣称全节点 recall=100%。JOB 中 A<E 正样本不足时先报告覆盖缺口，再补独立诊断 SQL（基于 JOB 表但不计入原始 113 条成绩）；不能靠改估算/加 sleep 后的不同计划伪装原 JOB 覆盖。

放行标准：除已接受的“首次 inner epoch EOF、未来 rescan 尚未发生”窗口外，可证明节点零 false EXACT、零非法 lower bound；inner Scan 不泄漏，inner Join 与 master 一致；snapshot/计划映射错误均拒绝。真实完成但 A<E 的节点在可证明子集中能产生 EXACT，漏报按 execution state、rescan、unsupported、无 Rows target 分原因统计。

## 执行控制与时长 / Scheduling And Limits

- 一次只运行一条 adviser/验证查询，按实验组顺序调度；runner 记录客户端 PID、backend PID、application_name 和四项 Join GUC 的实际值。
- 外层进程保护建议 360s，超出后先只取消本次已登记 backend 并等待 idle，再终止客户端；不直接 kill 客户端后留下服务器长查询，不取消其他用户会话。触发外层保护的样本无严格终止证据，标记 inconclusive。
- JOB 113 条 × 300s 的单组理论预算约 9.4h；TPC-DS SF1 103 条 × 300s 的理论预算约 8.6h。A/B 两 suite 串行执行，B/C/D 只使用 JOB；均未计收尾和独立验证。通常大部分查询提前完成，按 pilot 后的实际分布估算，不提前承诺固定完成时长。
- D 组耗时按去重计划数 N 估算，单次约 complete-oracle 耗时 + 10s + 收尾；900s oracle cap 只用于防护，超限不当作完整真值。
- 每批检查数据/统计指纹、负载与磁盘；剩余空间低于约 10GB 或出现明显跨实验干扰时暂停，不自动删除旧成果。保存进度 checkpoint，恢复时核对指纹，不重复计样本。

## 交付物 / Deliverables

独立时间戳目录保存：版本/源码/包/SQL/GUC/data 指纹、三组 run manifest、每 query/round 原始日志和 SQL、规范化 Hint 差异、计划签名、query_comparison.tsv、phase_times.tsv、node_evidence.tsv、未解释差异与五项 gate、最终报告。

报告依次给：环境兼容和覆盖率、A/B 放行结论、B/C 调优耗时与最终质量、D 的 A<E 分层结果、确认错误/预期修复/不确定项。结论使用“已覆盖的完整终止观测中未确认 bug”，不以少量通过样本宣称无潜在问题。

## 执行前需要确认 / Pending Decisions

当前 extopt、master 20MB 包、HAPF 真实取消和四项持久 Join GUC 的旧协议验证只保留为历史证据。2026-09-11 的诊断证明上一候选包在透明父节点提前返回时阻断子 target，并且跨轮 EXACT 保留旧值会在 TPC-DS 054 留下错误 E-rows；这些代码已恢复 master 语义。正确 SF1 库仍为 `tpcds_sf1_partial`。必须用当前成套 package 重新执行 JOB 113 + TPC-DS SF1 103 的完整 A/B。

本文件不授权修改 server ABI 或操作其他实例。用户已授权只操作本机 `gaussdb-local`。代码和文档修订后重新执行 JOB/TPC-DS SF1 A/B；A/B 放行后再启动仅 JOB 的 B/C 与 D。长任务采用每 5 分钟一次的低频检查，不做持续轮询。

## English Summary

Run JOB (113 queries) and TPC-DS scale factor 1 (`tpcds_sf1_partial`, 103 logical queries) in A/B with current master and current worktree with per-round timeout disabled. The earlier `tpcds_sf01` run is invalid because row counts prove it is SF0.1. Run B/C/D only on JOB; C uses the same candidate binary with a 10s cap. All use initial-plan verification, a 300s search budget and external optimization disabled. Freeze identical server/data/GUC/SQL/build inputs; distinguish approved bug fixes and timing noise from regressions before advancing beyond A/B. Repeat paired B/C runs for tuning-time and independently remeasured final-plan quality. Match actual plans before comparing complete and partial node observations, prioritize A<E terminal nodes, and separate cardinality correctness from EOF-state accuracy. Full recall requires independent EOF/epoch ground truth.

Local Docker `gaussdb-local` is running with JOB data and complete TPC-DS SF1 data in `tpcds_sf1_partial`. The previous candidate A/B exposed target-recursion and stale-EXACT regressions. The candidate now restores master target traversal, master EXACT overwrite/first-wins behavior, and master Nested Loop Rows collection while retaining semantic Parent Relationship handling. A fresh full JOB and TPC-DS SF1 A/B is required before B/C/D. `/Users/penggan/data` remains the sole benchmark artifact root.
