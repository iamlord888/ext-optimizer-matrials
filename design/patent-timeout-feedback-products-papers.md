# 超时节点反馈：论文与数据库产品公开资料核查

2026-10-20 补充：针对 IBM US7716215B2、Oracle US11321317B2、Microsoft US7493337B2 的直接相关论文、后续产品实现论文及官方手册，见 [三件专利的论文与产品手册导读](patent-timeout-feedback-related-papers-manuals.md)。以下保留原核查记录。

检索日期：2026-09-29。范围为可取得的论文原文、作者/出版机构页面及数据库产品官方文档；专利权利要求另见 [专利检索笔记](patent-timeout-feedback-patent-landscape.md)。本笔记用于确定交底书的技术边界，不替代指定申请日、司法辖区下的正式新颖性与创造性检索。以下“未见”只表示列出的公开文本未描述，不能推断产品内部一定没有实现。

## 核对问题

1. 是否对真实查询设置时限或由外部取消，并保留取消前的执行事实？
2. 是否有计划节点或算子级实际输出、启动次数、完成/活动状态？
3. 是否将未完成节点的已输出行数解释为下界，或把已完成节点的计数解释为精确值？
4. 是否把这些事实用于同一查询的执行中重优化、未来重复查询，或下一轮**独立候选**的基数修正？
5. 是否将不完整轮次的行数证据与候选完整执行时间分开处理？

## 论文原文

| 工作 | 原文已公开的机制 | 与本方案的实际距离 |
| --- | --- | --- |
| Kabra、DeWitt，*Efficient Mid-Query Re-Optimization of Sub-Optimal Query Execution Plans*，SIGMOD 1998，[原文 PDF](https://www.cs.cmu.edu/~natassa/courses/15-721/papers/reopt.pdf)，DOI [10.1145/276304.276315](https://doi.org/10.1145/276304.276315) | 在计划中设置统计收集点，执行时比较实测与优化器估计，重优化尚未执行的部分；论文 §2-3 讨论收集位置、物化和保留既有工作。 | 已公开执行中实际统计驱动优化，目标是**当前查询剩余部分**，并未以外部单轮时限取消后的节点快照驱动下一轮独立候选。 |
| Stillger 等，*LEO - DB2's LEarning Optimizer*，VLDB 2001，[用户提供原文](../sources/papers/P019.pdf) | 编译期保存计划骨架和估算，执行器累计算子计数，查询完成后保存并分析以修正后续优化；§4.3 已讨论查询结束时部分输入流仍未被完整消费。 | 已公开节点实际行数映射与未来优化，以及“计数完整性不能想当然”；所述保存时点是查询完成后。不能把 LEO 说成采样方案。 |
| Markl 等，*Robust Query Processing through Progressive Optimization*，SIGMOD 2004，[原文 PDF](https://dsf.berkeley.edu/cs286/papers/robust-sigmod2004.pdf)，DOI [10.1145/1007568.1007642](https://doi.org/10.1145/1007568.1007642) | `CHECK` 算子按计划有效基数区间监控实际输出；超出区间即停止原执行阶段、将部分执行取得的基数反馈给优化器，并重规划当前查询。论文 §3.1–3.3 明确区分物化点已完成计数与尚在输出时超过阈值的判断。 | “未完成时输出已超过上界，可证明低估；结束后可确定低输出”及“部分执行反馈重规划”均非本方案独有。其触发来自预置检查点与估算偏差，作用于当前查询的后续阶段。 |
| Chaudhuri 等，*Estimating Progress of Execution for SQL Queries*，SIGMOD 2004，[微软原文 PDF](https://www.microsoft.com/en-us/research/wp-content/uploads/2016/02/progress-1.pdf)，DOI [10.1145/1007568.1007659](https://doi.org/10.1145/1007568.1007659) | §4 将计划流水线区分为 completed、currently executing、not yet started；对每个算子维护上下界，已输出行数 `K_i` 是最终行数的可靠下界，算子终止时上下界相等；还利用算子内部状态细化和传播边界。 | “三种运行状态”“节点前缀行数下界”“已终止节点精确值”“执行器内部状态参与判断”已有明确公开。本文用途是进度估计；§4 末将再调用优化器列为可能的后续工作，未描述超时取消后的独立候选 Rows 回灌。 |
| Wu 等，*Sampling-Based Query Re-Optimization*，SIGMOD 2016，[用户提供原文](../sources/papers/1601.05748v1.pdf) | 在原查询正式执行前，对样本表试跑候选、更新连接选择率并反复重规划；§5.4 可给采样重规划过程设时限，到时选出已有最佳估计计划。 | “迭代重规划”“限时搜索”均已公开。其反馈源是样本试跑，不是到时被取消的真实候选 SQL 的执行器节点观测；固定预算的样本有估计方差，不能概括为必然不准。 |
| Zait 等，*Adaptive Statistics in Oracle 12c*，PVLDB 2017，[原文 PDF](https://vldb.org/pvldb/vol10/p1813-zait.pdf) | 编译期 statistics query 可受时限约束并使用部分结果；主 SQL 的 statistics feedback 则在执行结束后比较节点实际与估算，供随后重优化；adaptive plan 可以在当前执行中选预置分支。 | 三种时点不能混同。尤其不能称 Oracle 全部机制都等主 SQL 结束，也不能把限时统计查询当作取消主 SQL 的部分执行计划。 |
| Trummer 等，*SkinnerDB: Regret-Bounded Query Evaluation via Reinforcement Learning*，2019，[作者预印本](https://arxiv.org/pdf/1901.05152v1) | 将同一查询的执行切成短时间片，根据各片进度调整后续连接顺序。 | 已公开短时中断、进度反馈与查询内调整；不是取消独立候选后按计划节点完成性修正下一轮 Rows 目标。 |
| Marcus 等，*Bao: Making Learned Query Optimization Practical*，SIGMOD 2021；[作者版综述 PDF](https://people.csail.mit.edu/tatbul/publications/bao_sigrec22.pdf) | 外部组件选择候选 hint 集，执行后以计划及完整表现形成经验，再训练选择模型；作者版综述明确说 “Once the query execution is complete” 才加入经验。 | “hint 候选 + 执行反馈”已公开。所核作者版没有描述取消时的节点基数反馈；综述并非 SIGMOD 2021 全文，不能据此断言原系统的所有超时行为。 |
| Yang 等，*Balsa: Learning a Query Optimizer Without Expert Demonstrations*，SIGMOD 2022，[作者原文 PDF](https://zongheng.me/pubs/balsa-sigmod2022.pdf)，§4.3、§8.3.2 | 真实计划探索按前轮运行时间设置每轮 timeout；超时计划提前终止并赋予较大的延迟训练标签，避免坏计划拖慢训练。§8.3.2 的实验显示超时机制缩短达到专家水平的训练时间。 | “给候选计划单轮超时、取消后仍保留一种有用训练信息”已公开；保留的是慢计划的延迟惩罚，不是逐节点完成性/下界基数证据。交底书不能称所有普通超时轮次绝无有益信息。 |
| Pavlopoulou 等，*Revisiting Runtime Dynamic Optimization for Join Queries in Big Data Management Systems*，[作者预印本](https://arxiv.org/pdf/2010.00728)，2020 | AsterixDB 原型在连接阶段之间阻塞、物化中间结果并收集准确统计，再优化当前查询的余下连接；论文 §2、§5 说明各阶段重规划点。 | 再次证明运行期阶段性实际基数和重规划已知；其收集依赖预置完成的物化阶段，不处理外部时限取消后的混合节点状态。 |

## 产品官方文档

| 产品公开接口 | 明确已公开的能力 | 使用时点与边界 |
| --- | --- | --- |
| PostgreSQL [`EXPLAIN ANALYZE`](https://www.postgresql.org/docs/current/sql-explain.html)、[`statement_timeout`](https://www.postgresql.org/docs/current/runtime-config-client.html#GUC-STATEMENT-TIMEOUT)、[`auto_explain`](https://www.postgresql.org/docs/current/auto-explain.html) | 正常 `EXPLAIN ANALYZE` 给节点实际行数，`statement_timeout` 中止语句，`auto_explain` 可向服务器日志写慢查询计划。 | 这些官方接口未明示在被取消的 `EXPLAIN ANALYZE` 响应中交付可用于下一轮的节点快照；具体代码路径另见既有[初步核查](patent-timeout-feedback-prior-art.md)。 |
| MySQL 8.4 [`EXPLAIN FOR CONNECTION`](https://dev.mysql.com/doc/refman/8.4/en/explain-for-connection.html) | 从另一连接取得正在执行语句**实际使用的执行计划**；手册称可排查长时间查询。 | 官方说明其输出等同普通 `EXPLAIN`，不是本次运行的节点实际行数。 |
| TiDB [`EXPLAIN FOR CONNECTION`](https://docs.pingcap.com/tidb/stable/sql-statement-explain/#explain-for-connection)、[统计信息](https://docs.pingcap.com/tidb/stable/statistics/) | 可查询某连接当前或上次 SQL 的执行计划；普通 `EXPLAIN` 有各算子 `estRows`。 | 所核页面未把连接级计划描述为已执行行数或取消后反馈；统计文档的自动 `ANALYZE` 是表统计收集，不能与节点级执行反馈混同。 |
| MariaDB 10.9+ [`SHOW ANALYZE FOR`](https://mariadb.com/docs/server/reference/sql-statements/administrative-sql-statements/show/show-analyze) | **查询尚在运行时**，另一连接可取得 ANALYZE 式计划和截至此时的运行统计。官方慢查询示例同时列 `rows`（估算）、`r_rows`（实测）与 `r_loops`；内侧表的 `r_rows` 按多次循环给出平均值。 | 已公开“长查询不完成也可获得部分节点行数”，还体现累计与单次口径区分；所核接口面向正在运行的语句诊断，未描述取消清理后的保存或自动回灌下一轮独立候选。 |
| Oracle [SQL Monitor 取消后报告](https://docs.oracle.com/en/database/oracle/oracle-database/19/tgsql/monitoring-database-operations.html#GUID-11748F62-BE24-4511-B77B-F28D5E443A0A)、[`V$SQL_PLAN_MONITOR`](https://docs.oracle.com/en/database/oracle/oracle-database/19/refrn/V-SQL_PLAN_MONITOR.html) | 官方示例先取消长查询，再取 `DONE (ERROR)` 的报告，计划行中保留非零 `Rows (Actual)`；监控视图公开 `OUTPUT_ROWS` 与 `STARTS`。 | “取消后取得节点行数”不是新点；视图中的 SQL 状态和算子累计输出本身不等于目标最终基数证明。Oracle 的 statistics feedback 另见论文行。 |
| SQL Server [Live Query Statistics](https://learn.microsoft.com/en-us/sql/relational-databases/performance/live-query-statistics?view=sql-server-ver17)、[`sys.dm_exec_query_profiles`](https://learn.microsoft.com/en-us/sql/relational-databases/system-dynamic-management-objects/sys-dm-exec-query-profiles-transact-sql?view=sql-server-ver17)、[CE feedback](https://learn.microsoft.com/en-us/sql/relational-databases/performance/intelligent-query-processing-cardinality-estimation-feedback?view=sql-server-ver17) | 实时计划有算子输出与进度；DMV 有 `row_count`、`end_of_scan_count`、`rewind_count`、`rebind_count`。SQL Server 2022+ CE feedback 根据计划算子的实际/估算行数偏差，为重复查询测试替代 CE 模型并通过 Query Store hints 持久化。 | 不能将 SQL Server 仅表述为“展示诊断”：它已有节点基数驱动未来执行的产品反馈。官方 CE feedback 文档将**用户取消的验证执行视为回归**；未说明取消现场的未完成算子按精确值/下界回灌下一轮候选。 |
| Amazon Redshift [`SYS_QUERY_DETAIL`](https://docs.aws.amazon.com/redshift/latest/dg/SYS_QUERY_DETAIL.html)、[`SYS_QUERY_HISTORY`](https://docs.aws.amazon.com/redshift/latest/dg/SYS_QUERY_HISTORY.html) | 分步视图公开 `input_rows`、`output_rows`、`is_active`（运行中/完成）、`plan_node_id`、父节点及步骤 ID；历史视图包含运行中与已结束查询，状态值含 `canceled`。 | 已公开“步骤级行数 + 活动/完成状态 + 计划节点映射”和语句取消状态。所核文档未直接证明每条被取消查询的所有步骤数据均保留，也未描述基于这些视图自动修正下一轮计划。 |
| OceanBase Cloud [Execution Profile](https://en.oceanbase.com/docs/common-oceanbase-cloud-10000000001725119) | 官方页面说明实时显示算子执行状态、输出行数、时间与 I/O；列 `Start times` 表示算子重扫次数，图形计划含 Actual Rows。 | “节点状态 + 输出 + 重扫次数”本身已公开，不能作为单独创新点；所核页面把这些信息用于诊断，没有描述取消后按目标完整值/下界更新下一轮独立候选。 |
| Databricks [Adaptive Query Execution](https://docs.databricks.com/aws/en/optimizations/aqe) | shuffle/broadcast 阶段完成后得到运行期 `rowCount`（`isRuntime=true`），对当前查询余下阶段重优化；运行中计划会演化，未执行节点可变。 | 已公开“已完成阶段的精确统计 + 当前查询重规划”；统计获取依赖阶段完成，不是外部期限取消后对混合节点状态分级并启动下一轮独立候选。 |
| Snowflake [Query History / Query Profile](https://docs.snowflake.com/en/user-guide/ui-snowsight-activity) | Query History 包含状态与执行性能；Query Profile 展示算子树及运行统计。官方页面说查询仍运行时详情/画像可能不可用，待完成后可查看。 | 文档没有明示取消执行保留哪些算子值，也未说明节点行数反馈优化器；不能据此推断 Snowflake 内部绝无自适应能力。 |

## 目前可以支持的边界判断

- **不能单独主张**限时执行候选、取消后留有计划行数、执行中可读逐节点统计、节点完成/未完成/未启动三分、已输出行数为下界、完成节点为精确值、实际与估算行数比较、执行反馈优化后续查询、或将慢候选标为不优。上述要素分别见 Balsa、Oracle/MariaDB、Microsoft 2004、LEO/SQL Server CE feedback 等一手资料和独立[专利检索笔记](patent-timeout-feedback-patent-landscape.md)。
- **仍可讨论的具体组合**是：对按外部单轮期限取消的真实候选，在执行器对象失效前取得**原计划**与节点事实；把观测映射到下一轮独立候选中的同一逻辑基数目标，并检查查询语义、调用范围及可能重入的父链；仅将足以证明目标完整的事实记为精确基数，将同口径且已超过估算的部分输出作为提升基数的下界，其余拒绝；仅以正常完成轮次的完整运行时间比较已测最佳方案。应把这些条件作为相互依赖的完整链条，而非分别列为独立发明点。
- **创造性风险较高**：Microsoft 2004 已给运行中节点上下界和完成性，IBM POP 已用中途实际基数重规划；Oracle 的 SQL Monitor 公开取消后节点行数，另一件 Oracle 专利公开部分执行统计用于后续优化。MariaDB/Redshift/OceanBase 又给出了现成状态观测。即便所核材料没有逐字给出上述完整链条，也不能据此认定其组合必然具备创造性。正式权利要求需由申请日之前的完整专利同族、引证和司法辖区检索进一步校准。

## 与当前原型的技术支撑边界

当前[实现独立审计](patent-timeout-feedback-manuscript-audit.md)确认了取消清理前取证、节点证据分类和下一轮合并，但也记录了三项与拟主张的严格证明有关的缺口：本地 `TERMINAL` 且既往 `rescan_count=0` 尚不足以排除父节点**未来首次重入**；累计 `ntuples` 与单次 `plan_rows` 的调用范围尚不能对所有可反馈目标统一证明可比；仅凭查询块和关系集合不足以证明跨候选的谓词、连接类型及参数语义始终相同。交底书第 2.3 节描述的父链充分条件、口径核对和来源可比性应作为需要落实的实施规则，不宜无条件声称当前程序已对所有节点实现。若权利要求依赖这些条件，申请前需要用具体算法和可观察用例支撑。

## 检索方法与限制

使用标题/术语组合检索作者论文、Crossref/OpenAlex 元数据及官方文档：`mid-query re-optimization cardinality`、`query progress operator lower bound`、`timeout learned query optimizer`、`SHOW ANALYZE running query`、`CE feedback actual estimated rows`、`query detail output_rows is_active`、`execution profile operator start times` 等。对关键结论回到原文 PDF 或产品文档核对；作者 PDF 存于临时目录 `.scratch/patent-prior-art-papers/` 以便复核。产品在线文档持续更新，适用版本与公开日需要在确定拟申请的优先权日后再逐项确认。当前核查不覆盖闭源产品内部实现、所有语言的所有专利家族，也不能给出自由实施结论。
