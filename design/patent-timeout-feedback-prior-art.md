# 超时执行反馈机制：公开现有技术初步核查

> 本文件保留 2026-09-28 的初步核查记录。保护边界请以 2026-09-29 的[公开专利检索笔记](patent-timeout-feedback-patent-landscape.md)和[论文及产品核查](patent-timeout-feedback-products-papers.md)为准；后续检索补入了 IBM/Oracle/Microsoft 的部分执行与基数反馈专利、Microsoft 查询进度上下界论文，以及 MariaDB、Redshift、OceanBase 和 SQL Server CE feedback 等产品能力。

核查日期：2026-09-28。本笔记只依据公开的一手资料，不评价内部实现，也不构成专利新颖性或自由实施法律意见。这里的 `ANALYZE` 指 `EXPLAIN ANALYZE` 的执行分析选项；PostgreSQL 独立的 `ANALYZE` 命令另行注明。

## 最重要的结论

1. **“取消查询后仍获得含节点实际行数的部分执行计划”已有公开实例。** Oracle Database 19c 官方手册直接演示：查询久不结束时先 `Cancel the query`，再调用 `DBMS_SQL_MONITOR.REPORT_SQL_MONITOR`；报告显示 `DONE (ERROR)`，其 `SQL Plan Monitoring Details` 含每个节点的 `Rows (Estim)`、`Rows (Actual)`。因此不能把上述宽泛结果单独当作待主张的发明点。[O3]
2. **算子级实时行数也非空白。** SQL Server Live Query Statistics 和 `sys.dm_exec_query_profiles` 公开算子级 `row_count`、`estimate_row_count`、`end_of_scan_count`；Query Store 又明确区分客户端取消的执行。[M1][M2][M3] 但所查文档没有直接证明“取消后的各节点统计按完整精确值、已观测下界、未开始等语义分类，随后自动形成下一轮计划修正”的完整链条。
3. **跨次或执行中利用实际统计修正计划亦已有公开技术。** IBM LEO 原文摘要明确说监测计划每步的实际基数，与估算比较，并在未来相似查询优化中使用修正，甚至可触发执行中重优化。[P1] SkinnerDB 论文原文明确说将执行切为短时间片，根据进度选择下一时间片的连接顺序。[P2] 差异不能只写“使用反馈继续优化”；需要落到**中断节点的证据语义、可用于修正的条件，以及与下一轮计划的对应规则**，并与内部实现逐项核实。

## 核查维度与证据矩阵

`A`：超时或取消后保留执行反馈；`B`：计划节点级已观测行数；`C`：逐节点判定完整值与部分值的不同证据语义；`D`：该证据用于下一轮计划修正。表中“未证实”仅指列出的公开资料没有证明，**不表示该技术不存在**。

| 公开方案 | A | B | C | D | 来源边界 |
| --- | --- | --- | --- | --- | --- |
| PostgreSQL 标准 `EXPLAIN ANALYZE` + `statement_timeout` | 超时中止，但所查标准输出链路未见中止后的计划返回 | 正常完成时有 | 未证实 | 未证实 | 文档与 PostgreSQL 17 `ExplainOnePlan` 调用顺序 [G1][G2][G3] |
| Oracle 19c Real-Time SQL Monitoring | **已证实取消后可取报告** | **已证实，含实际行数** | 未证实明确的逐节点完整/下界判定；节点有 `STARTS`、`OUTPUT_ROWS` | 未证实该报告自动进入下一轮优化 | 官方取消示例、监控视图定义 [O1][O2][O3] |
| SQL Server Live Query Statistics / DMV / Query Store | 实时监控和取消执行记录分别已证实；所查资料未证实取消后保留同一份实时节点快照 | **执行中已证实** | DMV 有 `end_of_scan_count`，但所查资料未证实完整/下界分类算法 | 未证实来自取消节点数据的下一轮修正 | [M1][M2][M3] |
| IBM LEO | 未证实超时/取消专用路径 | 已证实监测每步实际基数 | 未证实中断节点证据分类 | **已证实未来优化使用修正**，摘要还称可触发执行中重优化 | IBM 发布的论文原文摘要 [P1] |
| SkinnerDB | 有时间片中断/切换，不等于客户请求超时后返回 `EXPLAIN` | 测量各时间片进度，不是 `EXPLAIN` 节点行数报告 | 未证实相同分类 | **已证实在同一查询内改变连接顺序** | arXiv 作者论文 PDF [P2] |

## 官方数据库资料

### PostgreSQL：默认输出与取消边界

- [G1] [`EXPLAIN` 官方文档](https://www.postgresql.org/docs/current/sql-explain.html)：`ANALYZE` “causes the statement to be actually executed”并把各计划节点的实际耗时、实际输出行数加入显示。文档同时允许 `FORMAT JSON` 等机器可解析格式。其描述针对正常生成的 `EXPLAIN` 结果，未说明取消后仍返回计划。
- [G2] [`statement_timeout` 官方文档](https://www.postgresql.org/docs/current/runtime-config-client.html#GUC-STATEMENT-TIMEOUT)：原文为 “Abort any statement that takes more than the specified amount of time.” [`pg_cancel_backend` 文档](https://www.postgresql.org/docs/current/functions-admin.html)说明取消指定会话当前查询。
- [G3] [PostgreSQL 17 `ExplainOnePlan` 源码](https://github.com/postgres/postgres/blob/REL_17_STABLE/src/backend/commands/explain.c#L604-L721)：函数注释为 “execute it if needed, and then print EXPLAIN output”；`ExecutorRun`、`ExecutorFinish` 在 `ExplainPrintPlan` 之前。由这个调用顺序可得：如超时错误在 `ExecutorRun` 中向外抛出，正常打印路径不可达。这是**该版本标准代码路径的推论**，不能外推到所有数据库分支或扩展。
- [G4] [`auto_explain` PostgreSQL 17 源码](https://github.com/postgres/postgres/blob/REL_17_STABLE/contrib/auto_explain/auto_explain.c#L364-L443)：计划打印在 `explain_ExecutorEnd` hook 中；官方[`auto_explain` 文档](https://www.postgresql.org/docs/current/auto-explain.html)说它将慢查询执行计划写入**服务器日志**。这与将中断后的部分计划以查询响应交付调用者是不同接口。
- [G5] [`pg_stat_statements` 文档](https://www.postgresql.org/docs/current/pgstatstatements.html)：列 `calls`、`total_exec_time`、`rows` 是语句维度；文档特别注明 planning/execution 统计“updated at their respective end phase, and only for successful operations”。因此不能把该视图说成中断节点实际行数的直接来源。
- [G6] [`Progress Reporting` 文档](https://www.postgresql.org/docs/current/progress-reporting.html)：列出的支持命令是独立的 `ANALYZE`、`CLUSTER`、`CREATE INDEX`、`VACUUM`、`COPY`、`BASE_BACKUP`，并不包括运行中的普通 `SELECT`/`EXPLAIN ANALYZE SELECT`。

### Oracle：最接近的取消后节点报告

- [O1] [`V$SQL_MONITOR` 官方定义](https://docs.oracle.com/en/database/oracle/oracle-database/19/refrn/V-SQL_MONITOR.html)：每次 SQL 执行有独立监控条目；执行时一般约每秒刷新；结束后至少保留一分钟。状态包括 `EXECUTING`、`DONE (ERROR)`、`DONE (FIRST N ROWS)`、`DONE (ALL ROWS)`。这些是**语句级**状态。
- [O2] [`V$SQL_PLAN_MONITOR` 官方定义](https://docs.oracle.com/en/database/oracle/oracle-database/19/refrn/V-SQL_PLAN_MONITOR.html)：每行对应一个计划操作，执行期间通常每秒刷新；`OUTPUT_ROWS` 是“Number of rows produced by this operation since the execution started”；还有 `STARTS`、`PLAN_LINE_ID`。它已公开计划节点级已观测行数，但该列定义没有称其在被取消时一定代表最终完整基数。
- [O3] [《Monitoring Database Operations》“Reporting on a Simple Database Operation: Scenario”](https://docs.oracle.com/en/database/oracle/oracle-database/19/tgsql/monitoring-database-operations.html#GUID-11748F62-BE24-4511-B77B-F28D5E443A0A)：官方场景先说 “The query is not completing”，第 1 步为 “Cancel the query”，第 2 步为获取 `DBMS_SQL_MONITOR.REPORT_SQL_MONITOR` 文本报告；其示例显示 `Status: DONE (ERROR)`、`SQL Plan Monitoring Details`、`Rows (Estim)` 和 `Rows (Actual)`，部分节点实际输出为 0，其他节点已有非零值。**该实例直接排除了“取消即完全丢失计划级执行反馈”的笼统背景陈述。**

### Microsoft SQL Server：实时节点统计与取消记录

- [M1] [Live Query Statistics 官方文档](https://learn.microsoft.com/en-us/sql/relational-databases/performance/live-query-statistics?view=sql-server-ver17)：原文称 live plan 提供算子级 “number of rows produced, elapsed time, operator progress”；可“不必等待查询完成”即实时查看，并可用于排查 “timeouts”。文档没有说这些节点统计在客户端取消后仍以同一报告返回。
- [M2] [`sys.dm_exec_query_profiles` 官方文档](https://learn.microsoft.com/en-us/sql/relational-databases/system-dynamic-management-objects/sys-dm-exec-query-profiles-transact-sql?view=sql-server-ver17)：`row_count` 是 “Number of rows returned by the operator so far”；`estimate_row_count` 为估计行数；`end_of_scan_count` 是 “Number of end of scans so far”。官方示例在**另一会话**按 `node_id` 汇总正在运行查询的已观测行数并与估计数比较。`end_of_scan_count` 是可能相关的完成线索，但不能仅凭本列定义断言它已完成本问题所需的逐节点精确/下界判定。
- [M3] [`sys.query_store_runtime_stats` 官方文档](https://learn.microsoft.com/en-us/sql/relational-databases/system-catalog-views/sys-query-store-runtime-stats-transact-sql?view=sql-server-ver17)：`execution_type=3` 为 “Client initiated aborted execution”，`4` 为 “Exception aborted execution”；记录按 `plan_id`、执行类型、时间区间组织，并有 `last_rowcount` 等**计划执行维度**字段。文档没有把它描述成已取消执行的逐节点反馈。

## 学术原文

- [P1] Volker Markl、Guy M. Lohman、Vijayshankar Raman，[*LEO: An autonomic query optimizer for DB2*](https://research.ibm.com/publications/leo-an-autonomic-query-optimizer-for-db2)，*IBM Systems Journal* 42(1), 2003, DOI [`10.1147/sj.421.0098`](https://doi.org/10.1147/sj.421.0098)。IBM 发布的**论文原文摘要**说：监测查询执行，将 QEP 每一步的估计基数与实际基数比较，计算修正用于“future optimizations of similar queries”；估计错误也可以触发 “reoptimization of a query in mid-execution”。目前核对的是作者机构发布的原文摘要，**未取得付费论文全文**；摘要不支持推断它如何处理取消节点。
- [P2] Immanuel Trummer 等，[*SkinnerDB: Regret-Bounded Query Evaluation via Reinforcement Learning*](https://arxiv.org/pdf/1901.05152v1)，arXiv:1901.05152v1，2019，PDF 原文第 1、4、7 页。摘要明确说“不维护数据统计量，也不使用成本或基数模型”，而是把查询执行划为大量短时间片，不同时间片尝试不同连接顺序，根据每片进度选取后续顺序。第 7 页还讨论某个数据批次在 timeout 前未处理完时中间结果可能丢失。它是**执行中自适应连接顺序**的公开方案，不能被概括成“超时返回带逐节点行数的 EXPLAIN 报告”。
- [P3，待核] Markl 等 *Progressive Optimization in a Database Management System* 以及 Kabra/DeWitt 的 mid-query reoptimization 原文全文，本轮未获得可可靠引用的原文链接和页码，故不写具体方案结论。后续应核对检查点的完整性证明、实际基数采集和重优化触发条件，再补入正式检索报告。

## 专利公开文本检索状态

本轮在 Google Patents 以英文组合词 `"explain analyze" timeout` 作初步检索，结果列表返回 8 条，其中出现 `CN114610588A`（*Database performance analysis method and device, electronic equipment and storage medium*，列表显示公开日 2022-06-10）。随后直接打开该公开文本及扩展查询时服务返回 503 / 反自动化页面，**未取得其权利要求和实施例**，因此不将它认定为最接近专利，也不据此写任何方案差异。检索范围只覆盖一次关键词列表，远不足以证明没有更近专利。

正式申请前建议由专利检索人员以中文及英文同义词在 CNIPA、WIPO、Espacenet、Google Patents 和主要数据库厂商专利族中交叉检索：`查询超时/取消/中断`、`部分执行计划`、`节点实际行数/基数`、`完整节点/未完成节点/下界`、`执行反馈/下一轮重优化`；逐篇核查**公开日、独立权利要求、具体实施例、同族与引用链**。现阶段不得断言任何“首创”或“未有公开”。

## 对交底书的直接约束

背景宜如实写成：标准 PostgreSQL `EXPLAIN ANALYZE` 在超时错误发生于 `ExecutorRun` 时不能经正常路径输出执行后计划；但 Oracle 已公开“取消后取计划节点实际行数”，Microsoft 已公开执行中算子级行数和取消记录，LEO/SkinnerDB 已公开执行反馈驱动优化。需要结合内部代码和真实测试证明：哪些节点在中断时可判为**完整观测**、哪些只提供**下界或其他受限证据**、该判断如何避免错误反馈，以及什么规则把合格证据对应到下一轮计划。公开资料中未证实完整链条，**这只是待专利检索与技术验证的候选区别，不是新颖性结论**。

## 结合当前实现的交底书初步思路

建议题目：**一种基于超时执行节点分级证据的查询计划反馈优化方法、装置及系统**。核心表述是“中断现场的节点观测 -> 同一物理计划的身份校验 -> 精确值/下界/不可用判定 -> 下一轮基数修正”，而不是单独的“超时返回部分计划”。

### 1. 技术背景与最接近现有技术

可从候选 SQL 的执行预算说起：标准 PostgreSQL 的 `ExplainOnePlan` 先执行 `ExecutorRun`，再调用 `ExplainPrintPlan`，所以执行阶段抛出的取消错误无法沿正常路径产出执行后计划。[G3] 当前 adviser 的普通反馈以完整 `EXPLAIN (ANALYZE, OUTLINE ON, BLOCKNAME ON)` 结果和总执行时间为输入；超时后若直接把未完成节点的计数当成最终基数，可能产生错误 Rows Hint。现有技术一宜选 Oracle Real-Time SQL Monitoring：其取消后报告已包含节点实际行数，缺点应准确落在**该官方方案未说明哪些节点计数可当作最终基数，也未说明如何将这些计数校验后自动注入下一轮 hint 调优**，而非声称它无法取得超时节点信息。[O3]

### 2. 技术问题、架构与方法

- **技术问题**：在候选 SQL 因时间预算而中断、执行对象即将清理时，如何取得节点观测，并只把可证明适用的行数用于下一轮基数修正，同时保留原取消错误与完整候选的交付语义。
- **系统架构**：数据库侧扩展负责在 `ExecutorRun` 取消异常展开前采集；客户端负责分帧接收、恢复连接、对同一候选 SQL 重规划和节点证据判定；既有 Rows Hint/外置优化器消费合格反馈。当前服务端先收集 `PlanState` 指针，在取消时读取 `Instrumentation`，发出快照后重新抛出原错误；`EXPLAIN ANALYZE` 本身仍返回 `57014`，不是一个成功的完整结果。见 [`extopt_partial_capture.cpp`](../contrib/extopt-gaussdb/extopt_partial_capture.cpp)、[`extopt_partial_gaussdb.cpp`](../contrib/extopt-gaussdb/extopt_partial_gaussdb.cpp) 与 [`hint_adviser_connection.c`](../contrib/hint_adviser/hint_adviser_connection.c)。
- **方法流程**：为一轮候选分配 execution ID 和执行时限；遇取消时在执行对象存活期输出原计划 PRETTY 与已执行节点的状态、重扫次数、实际/估算行数；客户端确认 `57014`、drain 完毕和快照完整，再对**同一候选 SQL**做非执行式 PRETTY/JSON `EXPLAIN`；比较节点 ID、拓扑、operation、query block 与显示精度内的估算行数，全部匹配后才发布事实。对应 [`hint_adviser_feedback_round.c`](../contrib/hint_adviser/hint_adviser_feedback_round.c)、[`hint_adviser_partial_snapshot.c`](../contrib/hint_adviser/hint_adviser_partial_snapshot.c) 和 [`hint_adviser_partial_proof.c`](../contrib/hint_adviser/hint_adviser_partial_proof.c)。
- **节点证据规则**：当前实现中 `TERMINAL` 且既往重扫次数为零时记为 `EXACT`；其它已执行节点若 `A_rows > E_rows`，记为 `LOWER_BOUND`；其余不发布行数证据。`EXACT` 能使用已完成但 `A < E` 的观测；未完成的 `A < E` 不能据此证明高估。Rows Hint 合并时精确值覆盖较弱观测，下界取已证明的较大值，但不覆盖已有精确值。见 [`hint_adviser_partial_proof.c`](../contrib/hint_adviser/hint_adviser_partial_proof.c) 与 [`hint_adviser_rows_state.c`](../contrib/hint_adviser/hint_adviser_rows_state.c)。
- **候选结果隔离**：`PARTIAL` 只可能更新下一轮 Rows Hint；只有完整执行的 `COMPLETE` 可更新实测执行时间和最佳候选。部分轮次仅在超时阈值足以证明未达到目标改善比例时计入 patience。见 [`hint_adviser_feedback_policy.c`](../contrib/hint_adviser/hint_adviser_feedback_policy.c) 和 [`hint_adviser_feedback_round.c`](../contrib/hint_adviser/hint_adviser_feedback_round.c)。既有非 ANALYZE fallback 是独立交付路径，不应表述为“任何部分轮次后均不会交付 SQL”。

### 3. 建议的保护层级与效果证据

**主权利要求候选**：在被取消的候选查询仍保有执行器状态时取得逐节点执行观测；校验这些观测与同一候选的物理计划身份；按节点完成与行数关系产生不同可信度的基数证据；仅将合格证据用于下一轮候选计划修正。要把上述**组合及依赖关系**写清楚，不能仅罗列监控、反馈和超时三个已知功能。

**从属层级候选**：原错误语义保留及有限时长的快照传输；execution ID 与完整分帧；原计划和重规划的结构映射；`EXACT`/`LOWER_BOUND`/不可用的判断条件及跨轮合并优先级；不完整运行时间不参加最优候选比较。这些是当前代码中的实施方式，具体接口名、NOTICE、PRETTY/JSON、8 MiB 等宜放从属实施例，不宜把主权利要求限死在某一数据库产品或消息格式。

**有益效果应分别举证**：有效节点反馈覆盖率、错误映射拒绝率、可验证的 `EXACT`/下界正确性、与无单轮超时对照的调优总时间和最终计划表现，以及不完整轮次不污染 best 的交付结果。当前测试脚本已有真实取消、结构映射、Rows 合并和交付边界用例，但全量 JOB/TPC-DS 对照仍被项目记录列为待验收；不能先写确定的性能提升百分比。见 [`topology_test.py`](../contrib/hint_adviser/t/topology_test.py)、[`feedback_integration.py`](../contrib/hint_adviser/t/feedback_integration.py) 和 [`JOB超时反馈对照测试方案.md`](JOB超时反馈对照测试方案.md)。

**现有实现的限定**：数据库适配层只覆盖指定普通行执行节点，`Hash` MultiExec 等不直接产出该节点观测；无 `Instrumentation` 的节点跳过。`TERMINAL` 是当前执行 epoch 的本地 EOF，不能单独证明未来绝不会重扫；Nested Loop 内侧 Scan 反馈被抑制，内侧 Join 仍有首次 EOF 后未来重扫的窗口。正式交底书应如实界定该边界；若要主张“所有完整节点都是全局精确基数”，需先用独立 EOF/重扫证据及实现加强支持。见 [`extopt_partial_gaussdb.cpp`](../contrib/extopt-gaussdb/extopt_partial_gaussdb.cpp)、[`hint_adviser_rows_target.c`](../contrib/hint_adviser/hint_adviser_rows_target.c) 和 [`JOB超时反馈对照测试方案.md`](JOB超时反馈对照测试方案.md)。

## 执行反馈、采样与超时边界的补充核查

以下只按各资料明确公开的机制比较。论文未说明的取消异常路径，不据此推断其内部一定没有实现。与专利保护范围有关的“采样不准”也应改为可证明的条件性表述：样本估计有抽样误差，所需精度取决于样本大小及数据分布；不能笼统断言采样结果必然不准确。

| 方案 | 反馈来源与使用时点 | 对“原查询必须执行完成”的判断 | 与本方案可讨论的区别 |
| --- | --- | --- | --- |
| IBM DB2 LEO [N1] | 监测执行中 QEP 各步骤的实际基数，与估计比较，用修正量优化未来类似查询；摘要还明确说估算错误可触发 **mid-execution reoptimization**。摘要没有把 LEO 描述为采样方案。 | **不能声称必须完整执行**：作者摘要明确留下执行中重优化路径；未核到其取消后节点快照及部分值的证明规则。 | 本方案针对由时间预算取消的候选执行，将中断时节点观测分级、校验后供下一轮候选使用；不可将“执行反馈用于重优化”本身说成区别。 |
| Oracle 12c dynamic statistics / dynamic sampling [N2] | 优化编译/解析期间运行递归 SQL，对表块小样本取统计量；Oracle 12.1 将早期的 dynamic sampling 称为 dynamic statistics，其中 dynamic sampling 是其一种来源。 | **不要求原 SQL 完整执行**：发生在正式执行前。 | 可比较其样本估计与真实执行中断观测的数据来源，但不能称 Oracle adaptive statistics 整体为采样。 |
| Oracle 12c statistics feedback（旧称 cardinality feedback）[N3] | 第一轮执行结束时，比较各操作估计基数与实际行数；差异显著时保存修正值，下次执行重优化。 | 官方流程明确写 **at the end of execution**，但文档未说明取消执行也可形成这类反馈；宜限定为“所查官方流程未提供超时中断时的节点证据分级与回灌”。 | 可将“长时间执行等不到首轮结束后的反馈”作为此分支的具体问题；不宜推广到 Oracle 所有自适应机制。 |
| Oracle 12c adaptive plans [N4] | 执行中由 statistics collector 收集行数并缓存一部分行，在预置子计划之间选用执行分支，例如 nested loops 与 hash join。Oracle 文档同时说明此机制不支持执行中改变 join order。 | **不要求完整执行后才调整**；但调整限于预置子计划，不等于取消后构造下一轮候选。 | 比较“本轮执行中的预置分支切换”与“中断后基于证据修正下一轮候选计划”，不能说 Oracle 没有运行时行数或执行中调整。 |
| Wu 等《Sampling-Based Query Re-Optimization》[N5] | 正式执行前，让优化器生成的候选计划在**样本表**上试跑；以样本 join selectivity 校验基数，反复规划直到收敛。论文第 2 页明确定位为 compile-time reoptimization，并指出可在正式执行前发现估算错误。 | **不要求原 SQL 完整执行**；所查原文没有针对原 SQL 超时/取消后的节点快照、完整值/下界分类。 | 可比较“预执行样本推断”与“真实执行达到预算后利用已观测节点事实”；该论文也已公开多轮基数反馈，故不能单独主张迭代重优化。 |

**可写进背景的句子：**“现有技术既有在编译期通过样本推断基数的方案，也有执行结束后利用实际基数反馈下次优化的方案，以及执行中有限的计划调整。对于受时间预算约束而被取消的候选 SQL，如何在原执行器清理前保留节点观测、区分可作为最终值与仅能作为下界的行数、核对其计划身份并安全用于下一轮候选，仍需具体机制。”其中末句是本方案拟解决的**具体技术问题**，不能替代正式专利检索所得的新颖性结论。

**不应写的句子：**“LEO 和 Oracle adaptive statistics 都依赖采样”“现有反馈调优必须等待完整 SQL 执行结束”“采样得到的基数不准确”“已有方案均不能在执行期间改变计划”“首次实现超时后取得节点 A-rows”。它们分别与 [N1]、[N2-N4]、[N5]、[N1/N4]、[O3] 所示事实冲突或超出证据。

**本节一手来源：**

- [N1] IBM Research，Markl、Lohman、Raman，[*LEO: An autonomic query optimizer for DB2*](https://research.ibm.com/publications/leo-an-autonomic-query-optimizer-for-db2)，2003，作者机构发布的原文摘要。摘要明确写 “future optimizations of similar queries” 与 “reoptimization of a query in mid-execution”。本轮未获得论文全文，故不推断 LEO 对异常取消与节点重扫的处理。
- [N2] Oracle Database 12.1，[*Controlling Dynamic Statistics*](https://docs.oracle.com/database/121/TGSQL/tgsql_astat.htm#GUID-F1C666DC-5450-4F92-83AA-607327DB5FD8)；同版[*Query Optimizer Concepts: Dynamic Statistics*](https://docs.oracle.com/database/121/TGSQL/tgsql_optcncpt.htm#GUID-2D218B35-A443-41CB-855C-CF41ADB802EE)。前者明确写 “during parsing” 扫描小样本表块，且 12.1 前称 dynamic sampling；后者把 dynamic sampling 称为 dynamic statistics 的一种。
- [N3] Oracle Database 12.1，[*Automatic Reoptimization*](https://docs.oracle.com/database/121/TGSQL/tgsql_optcncpt.htm#GUID-D23F8328-A4CF-4211-8C9B-79B826F34A32) 与 [*Reoptimization: Statistics Feedback*](https://docs.oracle.com/database/121/TGSQL/tgsql_optcncpt.htm#GUID-739F1F0B-ABE2-44C9-9783-8FA54F4ADB12)：前者写首次执行结束后决定下次是否重优化；后者写在执行结束时按节点对比估算与实际行数，下次执行使用修正值。
- [N4] Oracle Database 12.1，[*How Adaptive Query Plans Work*](https://docs.oracle.com/database/121/TGSQL/tgsql_optcncpt.htm#GUID-A42B8874-95AF-4D5B-B5D9-F0376F91D15E) 及同页 *Automatic Reoptimization*：前者说明运行时统计收集和预置子计划切换；后者说明执行中 join order 不在此机制的调整范围。
- [N5] Wentao Wu、Jeffrey F. Naughton、Harneet Singh，[*Sampling-Based Query Re-Optimization*](https://arxiv.org/pdf/1601.05748)，SIGMOD 2016，DOI [`10.1145/2882903.2882914`](https://doi.org/10.1145/2882903.2882914)。核对作者原文 PDF 第 1-2 页：第 1 页明确称 “before the system even begins executing the chosen query plan”；第 2 页给出样本表 join selectivity 公式和迭代算法，并将其与执行中重优化区分。论文指出样本越大估算越准确，不能据此断言所有采样都不准确。
