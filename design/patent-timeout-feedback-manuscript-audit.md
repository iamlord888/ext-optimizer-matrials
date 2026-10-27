# 限时执行部分反馈交底书：文献、机制与文稿核对

本笔记记录[交底书正文](patent-timeout-feedback-disclosure-draft.md)的文献依据、文稿一致性和当前代码与方案的差距，不代替专利新颖性检索。核对范围包括用户提供的两篇论文、Oracle 12c 原论文、指定解析博客、所列 Oracle/Microsoft 官方文档及当前仓库源码。以下的“未见公开”仅指所列资料没有描述该步骤，不能推断产品内部绝不存在该能力。第三、四节保留初稿审计时的改写依据；当前待实现条件见第五节；2026-10-14 的逐页核对和正文清理见第六节；2026-10-22 的文献及算子结束机制修订见第七节；2026-10-26 的组织结构、全文语言及现有技术重点修订见第八至十三节。这里的审阅意见不属于交底书正文。

## 一、可直接据以写作的事实

| 编号 | 可证事实及其准确边界 | 证据 |
| --- | --- | --- |
| L1 | LEO 将计划骨架及估算留在编译侧，执行器为 section operator 累加计数；论文明确说计数在查询完成后保存。分析过程可以离线运行，也可以在查询完成时在线增量运行，修正用于后续查询优化。不能将 LEO 概括为采样方案。 | [LEO 原文](../sources/papers/P019.pdf)，PDF 第 1 页摘要、第 3 页 §3.1、第 5 页 §4.1–4.3；尤其第 5 页 “saved after the query completes” 和 “as queries complete execution”。 |
| L2 | LEO 没有实现正在运行的 QEP 中途变更；论文明确说 “does not (yet) address ... changing in mid-stream the QEP of a running query”。这不等于数据库自适应优化研究整体都必须等查询结束。 | [LEO 原文](../sources/papers/P019.pdf)，PDF 第 3 页 §2–3。 |
| L3 | LEO 已认识到“查询正常结束”不保证每条算子输入流都被完整消费。对 merge join 的 implicit early out，另一侧剩余行没有被请求和计数；SORT/TEMP 等物化屏障可确保该输入流先被完整扫描。此论证针对该类输入流，不能扩大为“所有节点只有经过 SORT/TEMP 才可信”。 | [LEO 原文](../sources/papers/P019.pdf)，PDF 第 6 页 §4.3 “implicit early out”。 |
| S1 | Wu 等的方案在原查询正式执行**前**，对样本表运行候选计划、验证连接选择率并反复重规划；迭代并不是本方案单独拥有的概念。论文 §2.1 使用的抽样连接选择率估计器被引用为无偏且强一致，样本增大时趋于准确。 | [Sampling-Based Query Re-Optimization](../sources/papers/1601.05748v1.pdf)，PDF 第 1 页摘要/§1、第 2 页 §2.1–2.2、第 3 页 Algorithm 1。 |
| S2 | 论文第 11 页允许对**采样重规划过程**设置 timeout threshold，并在达到预算时依据采样修正后的估计成本返回迄今候选中最好的计划；它没有描述取消真实候选 SQL 后提取执行器节点状态。第 12 页又明确指出 “sampling is by no means perfect”，因此可讨论有限样本不确定性，不能写“采样不准确”作为普遍结论。 | [Sampling-Based Query Re-Optimization](../sources/papers/1601.05748v1.pdf)，PDF 第 11 页 §5.4、第 12 页 §6–7。 |
| O1 | Oracle 12.1 的 dynamic statistics 可在解析时执行递归 SQL 扫描随机表块样本；statistics feedback 的官方流程在首次执行结束时将估算与各操作实际行数比较，供后续执行重优化。两者是不同阶段。Oracle adaptive plans 则可在当前执行中让 statistics collector 帮助选择预置子计划，不能说 Oracle adaptive statistics 整体依赖查询完成。 | [Oracle Dynamic Statistics](https://docs.oracle.com/database/121/TGSQL/tgsql_astat.htm#GUID-F1C666DC-5450-4F92-83AA-607327DB5FD8)；[Oracle Adaptive Plans](https://docs.oracle.com/database/121/TGSQL/tgsql_optcncpt.htm#GUID-A42B8874-95AF-4D5B-B5D9-F0376F91D15E)；[Oracle Statistics Feedback](https://docs.oracle.com/database/121/TGSQL/tgsql_optcncpt.htm#GUID-739F1F0B-ABE2-44C9-9783-8FA54F4ADB12)，其中原文为 “At the end of execution”。 |
| O2 | Oracle 原论文明确说明：编译期 statistics query 在时间预算到达后停止并使用已取得的部分结果；同一论文的 §3.3.1 说明主 SQL 执行结束后，执行器按计划节点检查基数偏差以建立后续使用的指令。两条路径作用于不同的查询与时点。指定[知乎解析](https://zhuanlan.zhihu.com/p/640241574)仍无法直接读取正文，但这两项技术事实已由原论文独立核实，不依赖博客转述。 | [*Adaptive Statistics in Oracle 12c*](https://vldb.org/pvldb/vol10/p1813-zait.pdf)，PDF 第 5 页 §3.1、第 8 页 §3.2.2、§3.3.1；指定博客请求返回 403。 |
| O3 | Oracle 19c 官方示例先取消未结束查询，再调用 SQL Monitor 报告；示例状态为 `DONE (ERROR)`，计划行中既有零也有非零 `Rows (Actual)`。故“已有技术一取消后拿不到任何节点行数”明显错误。`V$SQL_PLAN_MONITOR.OUTPUT_ROWS` 是执行开始后累计的算子输出，文档说明可按 `STARTS` 计算每次启动的平均输出；其 `STATUS` 字段被定义为 **SQL execution status**，并非“该计划节点已完成且以后不会重扫”的明示字段。 | [Oracle 取消后报告](https://docs.oracle.com/en/database/oracle/oracle-database/19/tgsql/monitoring-database-operations.html#GUID-11748F62-BE24-4511-B77B-F28D5E443A0A)；[`V$SQL_PLAN_MONITOR`](https://docs.oracle.com/en/database/oracle/oracle-database/19/refrn/V-SQL_PLAN_MONITOR.html) 的 `STATUS`、`STARTS`、`OUTPUT_ROWS` 定义。 |
| M1 | Microsoft Live Query Statistics 公开实时总体进度、算子输出行数和算子进度，用于排查慢查询及超时，底层使用 `sys.dm_exec_query_profiles`。DMV 公开每算子/线程的 `row_count`（至今输出）、`end_of_scan_count`（至今扫描结束次数）、`rewind_count`、`rebind_count` 和 `estimate_row_count`。所以“首次有节点进度或扫描结束计数”不可主张。字段定义本身也没有证明节点未来不会再次执行。 | [Live Query Statistics](https://learn.microsoft.com/en-us/sql/relational-databases/performance/live-query-statistics?view=sql-server-ver17) 的正文和使用场景；[`sys.dm_exec_query_profiles`](https://learn.microsoft.com/en-us/sql/relational-databases/system-dynamic-management-objects/sys-dm-exec-query-profiles-transact-sql?view=sql-server-ver17) 的列定义。 |
| M2 | Microsoft 官方说明标准 profiling 收集行数、CPU、I/O；轻量级 profiling 的不同版本仍可收集行数，并通过 DMV 或事件暴露运行统计。所查文档关注观测和诊断，未写“取消候选后将节点计数分为完整值/下界、再回灌独立下一轮候选”的方法；这只能作为**文档对比**，不能断言 SQL Server 产品没有任何反馈优化。 | [Query Profiling Infrastructure](https://learn.microsoft.com/en-us/sql/relational-databases/performance/query-profiling-infrastructure?view=sql-server-ver17)，标准/轻量级两节及 usage guidance；[Live Query Statistics](https://learn.microsoft.com/en-us/sql/relational-databases/performance/live-query-statistics?view=sql-server-ver17)。 |

## 二、当前实现真正提供的事实

| 编号 | 当前代码行为及限制 | 证据 |
| --- | --- | --- |
| C1 | `capture_executor_run` 在原 `ExecutorRun` 的查询取消异常处理中尝试输出快照，然后重新抛出原错误。因此这是**取消时的带节点观测快照**，不是让被取消的 `EXPLAIN ANALYZE` 成为成功返回的完整执行计划。 | [`extopt_partial_capture.cpp`](../contrib/extopt-gaussdb/extopt_partial_capture.cpp#L616)，616–660 行。 |
| C2 | 适配层从 `PlanState.instrument` 读取 `running`、起始时间、`status`、`ntuples`、`nloops`；代码注释说 `status` 在行执行器返回 EOF 时设置，`instr_end()` 在一个执行时期关闭时增加 `nloops`。代码把 `!active && status` 设为本地 `terminal`，把累计 `ntuples` 直接设为 `A`，并把整数化 `nloops` 命名为 `rescan_count`。该变量更准确的概念是**已关闭的 instrumentation 时期数**，不应在专利中直接等同于“未来重扫次数”。 | [`extopt_partial_gaussdb.cpp`](../contrib/extopt-gaussdb/extopt_partial_gaussdb.cpp#L52)，52–74、579–623 行。 |
| C3 | 已运行且受支持的节点才进入快照；缺 `Instrumentation` 的节点被省略。客户端的分类函数：`TERMINAL && rescan_count == 0` 记 `EXACT`；否则若 `A > E` 记 `LOWER_BOUND`；其余不发布。这是**当前分类规则**，不是对所有拓扑下最终基数正确性的数学证明。 | [`extopt_partial_capture.cpp`](../contrib/extopt-gaussdb/extopt_partial_capture.cpp#L528)，528–553 行；[`hint_adviser_partial_proof.c`](../contrib/hint_adviser/hint_adviser_partial_proof.c#L10)，10–25 行。 |
| C4 | 快照的节点事实并非无条件使用：客户端重新规划同一候选，并比较计划节点、拓扑、操作文本和估算行数；映射矛盾整轮拒绝。Rows 目标收集只接受可寻址 Scan/Join 目标，但 Nested Loop 内侧仅抑制 Scan 类反馈，符合条件的内侧 Join 仍可发布。 | [`hint_adviser_partial_proof.c`](../contrib/hint_adviser/hint_adviser_partial_proof.c#L91)，91–189 行；[`hint_adviser_rows_target.c`](../contrib/hint_adviser/hint_adviser_rows_target.c#L515)，515–596 行。 |
| C5 | 跨轮合并时已有 `EXACT` 不被新下界覆盖，下界合并取已证明较大值；部分轮次不更新候选的完整耗时与 best，完整轮次才更新。 | [`hint_adviser_rows_state.c`](../contrib/hint_adviser/hint_adviser_rows_state.c#L135)，135–181 行；[`hint_adviser_feedback_policy.c`](../contrib/hint_adviser/hint_adviser_feedback_policy.c#L31)，31–87 行。 |
| C6 | `rows_output_value` 将原始行数至少提升至 1 后按 `%.0f` 输出；`rows_apply_exact` 另存原始 `observed_rows`。因此 `EXACT(0)` 可作为内部证据，但当前 Rows Hint 数值为 1。 | [`hint_adviser_rows_state.c`](../contrib/hint_adviser/hint_adviser_rows_state.c#L29)，29–35、55–68 行。 |
| C7 | 同一 key 的新 `EXACT` 在跨轮时会覆盖已有 `EXACT`；同轮重复 `EXACT` 则先到者优先。代码没有验证这两轮的数据版本或目标谓词语义是否仍可比，也没有在跨轮精确值冲突时拒绝覆盖。 | [`hint_adviser_rows_state.c`](../contrib/hint_adviser/hint_adviser_rows_state.c#L55)，55–101 行。 |
| C8 | 持久化 `RowsHintKey` 由查询块和目标关系列表组成；`PlanNode` 保存若干扫描谓词文本，但没有独立的连接语义类型字段。因而同 key 本身尚不能证明跨候选节点的谓词范围、内/外/半连接语义和相关参数完全相同。 | [`hint_adviser_rows_private.h`](../contrib/hint_adviser/hint_adviser_rows_private.h#L26)，26–30 行；[`hint_adviser_rows_key.c`](../contrib/hint_adviser/hint_adviser_rows_key.c#L160)，160–181 行；[`hint_adviser_plan.h`](../contrib/hint_adviser/hint_adviser_plan.h#L26)，26–64 行。 |

## 三、历史初稿的改写依据

1. **把“局部 EOF”与“完整目标基数”分开。** [初稿 2.3.3](patent-timeout-feedback-disclosure-draft.md)虽然写了“目标可用”，但给出 `TERMINAL && rescan_count == 0` 后又称可形成 `EXACT`，读者很容易理解为这两个条件已经证明全程完成。当前源码没有证明父节点未来不再首次重扫；尤其 Nested Loop 内侧 Join 仍能通过目标收集。[C2–C4] 建议全文统一称“本地执行时期完成”，只有另有**不再重入或计数口径独立于重入**的条件时才写“目标基数精确”。
2. **下界需要同口径前提。** `A` 来自累计 `ntuples`，`E` 来自 `Plan.plan_rows`。[C2][C3] 对一次执行时期，已输出量随执行单调增加，`A > E` 可以否定该时期的低估；对已多次执行的节点，累计 `A` 可能大于单次计划 `E`，即使每次实际输出都小于 `E`。若 Rows Hint 目标按单次基数解释，原规则不能无条件证明 `A` 是该目标的下界。初稿 2.1、2.3.3、3.2 的“已证明下界”均须加**相同目标、相同执行次数/聚合口径**的限定，或明确只对单时期、可比目标应用。
3. **LEO 的“完整性”讨论要精确引用。** [初稿 1.2.1](patent-timeout-feedback-disclosure-draft.md)可保留，但将“只有 SORT、TEMP 等……才可作为完整事实”改成针对 merge-join 另一侧输入流的叙述。LEO 已处理**正常查询完成后仍存在不完整算子计数**，所以本方案不能把“发现不完整节点”本身写成突破。[L3]
4. **采样方案应承认其统计性质。** [初稿 1.3](patent-timeout-feedback-disclosure-draft.md)目前写有限样本方差，方向正确；还应明确该论文 §2.1 的估计器无偏、强一致，它的 limitation 是有限样本/代价/覆盖范围，不是笼统“不准确”。其 timeout 限的是**优化期采样迭代**，且论文已提出按估计成本返回当前最好候选；不能写为没有超时预算。[S1][S2]
5. **Oracle 的两个阶段不能混为一个“超时反馈”功能。** 初稿须区分编译期 statistics query 的限时部分结果与主 SQL 执行结束后的反馈指令。[O1][O2] Oracle 原论文现已核实，可直接支持这一区分；指定博客不可读不再阻碍该事实的引用。
6. **监控现有技术应按公开内容对比。** Oracle 官方取消实例已给节点实际行数；Microsoft DMV 已给结束扫描与重扫相关计数。[O3][M1] 主稿可陈述“这些资料公开观测与诊断，但所查页面未描述把取消时的节点观测经目标语义判定后回灌到下一轮独立候选”；不要推出厂商“不具备”该能力。[M2]
7. **名词应区分计划和观测。** 超时没有生成一棵“只执行了一部分的计划树”；物理计划结构已生成，只有节点运行信息是不完整的。[C1][C4] “超时返回部分执行计划”适合通俗表述，技术定义宜写“保留原计划结构及其部分节点运行观测”。
8. **问题定位按两道门槛写。** 第一门槛是时间：完整执行反馈可能超出候选预算；第二门槛是语义：取消时看见的节点累计数不必等于该基数目标的最终值。直接取消并丢弃观测与无条件使用所有观测分别损失信息或引入错误。[L1][L3][O3][C2–C4] 这样才能从已有工作自然推出所需机制，而不是以“现有技术都没有反馈”为前提。

## 四、历史初稿的章节重组依据

先给出一个具体查询和候选试运行：优化器为多个中间关系估计基数，实际低估可能让候选采用代价很高的连接顺序；候选试运行到预定时限仍未结束。说明运行中的计划有不同状态的节点，并用一个**本地 EOF 但未来可能重扫**的节点与一个**尚未 EOF 但输出已超过估算**的节点并列，直接引出为什么 `A-rows` 不能不加区分地反馈。这比先介绍 `NOTICE`、协议和适配结构更符合论文的问题驱动顺序。

随后用三个正交维度安排相关工作：**观测来源**（真实执行或样本）、**观测时点**（编译期、运行中、正常结束后、取消时）、**观测语义与去向**（诊断显示、当前计划切换、以后类似查询的估算修正、下一轮独立候选）。LEO 放在“执行结束后学习”及“不完整计数已有先例”两处；Wu 等放在“预执行样本迭代”；Oracle adaptive plans 放在“运行中预置分支切换”，statistics feedback 放在“结束后修正”；Oracle SQL Monitor 和 Microsoft profiling 放在“节点级可观测性”。各技术不能被压成一个“必须等全 SQL 完成”的类别。[L1–L3][S1–S2][O1][O3][M1]

技术问题可严格表达为：给定候选计划 `P_i`、执行时限 `τ`、取消时刻各节点的观测 `S_v = (id_v, E_v, A_v, activity_v, eof_v, epochs_v)`，以及下一轮需要修正的基数目标 `k`，确定哪些 `S_v` 能与 `k` 同口径比较，并输出 `EXACT`、`LOWER_BOUND` 或无反馈。这里 `EXACT` 的必要条件是观测覆盖目标的**全部相关执行**；`LOWER_BOUND` 的必要条件是已输出量对同一目标单调且计数口径一致。当前代码实现了取消前取证、三类分类、计划身份核验和下一轮合并，但这些必要条件在“未来首次重扫”和“跨多次执行累计行数”上尚未完全由分类函数证明。[C1–C5]

方案章节宜按**取证时点 → 对象/字段 → 状态语义 → 基数目标语义 → 证据分级 → 下一轮消费**推进。结构图必须把 `QueryDesc → PlanState → Plan` 与 `PlanState → Instrumentation` 两条关系分开，字段图注明 `ntuples` 为累计值、`nloops` 为已关闭执行时期、`status` 为本地 EOF，并用虚线画出父节点可能再次调用该节点。这样图片才能解释 `EXACT` 的附加前提，而非仅把源码字段排列在框里。[C1–C4]

## 五、当前稿件与原型的剩余差距

| 条件 | 当前证据 | 对交底书和投稿的影响 |
| --- | --- | --- |
| 目标级完成性 | 原型仅以本地 `TERMINAL && rescan_count == 0` 判 `EXACT`，未沿父链排除未来首次重入；Nested Loop 内侧 Join 仍可进入 Rows 目标。[C2–C4] | 正文的父链充分条件是待落实的方案规则，不能宣称当前代码已普遍保证目标级精确。 |
| 下界计数口径 | `ntuples` 是累计值，`plan_rows` 可能表示单次调用；原型在 `A>E` 前未做分次归一。[C2][C3] | 多次调用节点的下界可能不成立；实现需取分次计数、证明目标为累计范围，或保守跳过。 |
| 目标语义等价 | 持久化 Rows key 只编码查询块与目标关系，未独立编码连接类型、谓词范围或相关参数。[C8] | 正文的目标 `k` 比当前 key 更强；跨候选复用前须证明这些语义一致，或限定为可验证的目标子集。 |
| 跨轮证据一致性 | 新 `EXACT` 可跨轮覆盖旧值，缺少数据版本与谓词语义一致性检查。[C7] | 正文要求的冲突拒绝与数据变化失效尚属方案条件，不能当作当前实现行为。 |
| 证据与 Hint 数值 | 原始 `EXACT(0)` 保留，但 Rows Hint 至少输出 1。[C6] | 可称零行是精确观测，不能称现有优化器实际消费零行提示。 |
| 效果验证 | [2026-09-28 对照](../data/patience5-ab-20260928/run-manifest.txt)覆盖 JOB 113 条、JOB-complex 30 条；286 次调优、1,224 次独立执行均完成。逐查询[工作簿](../outputs/job_job_complex_tuning_query_summary_patience5_20260928.xlsx)与[原始执行记录](../data/patience5-ab-20260928/results/measurements.tsv)支持两个套件的总调优时长下降。JOB 的执行时间总和下降仅在纳入 `16b/18c` 未交付恢复候选的扩展评估中成立；正式交付限定口径反而较 master 高 7.89%。 | 现有结果可支撑固定配置下的系统级观察，不足以证明逐查询无退化、节点级证据普遍正确，或把收益单独归因于 10 秒参数；直接投稿仍需针对这些问题的实现验证与消融分析。 |

## 六、2026-10-14 交底书与 PPT 文稿核对及正文清理

本轮逐段阅读交底书、七页中文 PPT 文稿及英文对照，使用前述已核对的文献记录，并通过 CodeGraph 读取当前代码的统计采集、状态计算、分类、合并和期限计算逻辑。未重新进行专利新颖性检索。正文修改范围为标题、技术事实、实施条件、实验说明和有益效果；撰稿建议、创造性判断及实现缺口说明留在本记录中。

### 6.1 逐页对应关系

| PPT 页 | 交底书对应位置 | 本轮核对及修正 |
| --- | --- | --- |
| 名称 | 正文标题、摘要、英文摘要 | 统一为“一种基于限时执行部分反馈的查询执行计划迭代优化方法”；不再使用旧名称中的“超时执行节点”“分级反馈”“方法及系统” |
| 第 1 页：背景 | 1.1、1.6、3.2 | 明确慢候选等待和直接取消导致的信息损失；“部分”描述整轮执行未完成，仍可含合格精确行数 |
| 第 2 页：限时迭代 | 2.2、2.3.1、2.3.4 | 正常完成和超时取消均可供行数反馈；只有完整运行时间更新最佳候选及期限；初始 10 秒为实验配置，后续上限为 min(T₀, 1.25 × t_best)，受剩余全局预算约束 |
| 第 3 页：统计来源 | 2.3.2、2.3.3、2.4.4 | 逐一说明 QueryDesc.planstate、PlanState.plan、PlanState.instrument、plan_node_id、plan_rows、ntuples、nloops；active 由起始时间计算，running 只辅助判断执行痕迹 |
| 第 4 页：后续调用与计数 | 2.1、2.3.4、2.4.2 | 本地 EOF 与全部相关输出结束分开；检查的是候选若继续执行是否仍可能输出，避免“已经取消所以以后不会调用”的循环解释；累计 100 与单次 10 不能直接比较 |
| 第 5 页：判定与合并 | 2.3.4、2.3.5、2.4.1、2.4.3 | 页标题改为“节点行数反馈的判定与合并”；定义 A 为采集时点行数，覆盖正常完成与取消；公式补上已有精确值时任何新下界都不覆盖；加入与 PPT 一致的 5,000 → 6,400 → 7,200 示例 |
| 第 6 页：拟保护点 | 3.1、3.3 | 统一为统计保留、行数判定、跨轮合并、下一轮修正四项；重新规划与原计划匹配只在 2.4.4 作为实施例说明，不列入关键技术点 |
| 第 7 页：业务价值和验证 | 2.4.5、3.2、英文摘要 | JOB 调优耗时下降 31.89%（约 32%），JOB-complex 下降 41.84%（约 42%）；16b 的 4 轮/约 36 秒与 11 轮/约 11 秒保持一致，后者明确是未正式交付的恢复候选；190/168 秒为扩展评估，205 秒为正式交付回填后的结果 |

### 6.2 移出或改写的批注性内容

| 原文类型 | 原文示例 | 处理方式 |
| --- | --- | --- |
| 对撰稿人的保护策略建议 | “它们是本发明的技术基础，不能作为本案单独的区别点。” | 改为现有技术能够以节点统计修正后续基数的事实，保留文献引用 |
| 创造性及首创性讨论 | “均不能单独作为本案发明点”“本案不应把任一单点手段写成首创” | 正文删除；先前文献重合判断仍保留在本记录及重合分析报告 |
| 对现有技术描述的纠错提醒 | “不能将微软概括为……”“不能把这一局限表述为……” | 改为对应专利实际公开的机制与使用场景 |
| 检索方法及权利要求拼接评价 | “所核文本”“不能机械拼成一个已公开的完整实施例” | 正文保留专利标识和机制事实，检索判断留在分析材料 |
| 面向实现者的待办意见 | “若采用严格的目标级保护范围，应将……落实为实现条件” | 移出正文，正文以行数适用条件及不反馈分支表达；实现缺口见第五节和 6.3 |
| 权利要求写作指导 | “以下内容用于形成权利要求撰写时的技术层次”“保护论证应落实到……” | 改为“关键技术点”和“其他实施方式”，不在交底书中讨论授权或创造性判断 |
| 对实验结论的撰稿提醒 | “不能宣称已普遍提高最终交付质量” | 改为正式交付与扩展评估的具体结果及评价方法，保留恢复候选身份 |
| 易引起歧义的术语 | “时期数”“重入”“口径”“完成性” | 正文改为 Loops 计数、再次执行、计数范围、全部相关行数是否返回等具体表述 |

文献编号 [R]、实现来源 [C]、实验记录 [E] 用于定位依据，仍保留在参考资料中；其作用为引用，并非撰稿批注。

### 6.3 方案条件与当前代码的区别

这些差距仍须保留，正文清理不代表代码已经补齐：

- `partial_observation_evidence` 仍按 `TERMINAL && rescan_count == 0` 分类为 EXACT，未完整证明父算子不会在未来首次再次执行目标。正文和 PPT 的严格条件是技术方案条件，不能据此宣称原型已对所有节点保证正确。
- `ntuples` 为累计行数，当前分类的 `A>E` 未对所有重复执行、参数化或并行节点统一计数范围。本文方案要求先统一范围或不反馈。
- 当前 Rows key 仅按查询块和关系列表标识目标，未全面核对连接类型、谓词、参数及数据可见范围。新精确值跨轮覆盖及旧统计失效需依赖这些条件，不能将其描述为现有代码已具备的完整校验。
- 已确认 `active = (starttime_seconds != 0 || starttime_microseconds != 0)`、`terminal = (status != 0 && !active)`。`running` 非零不能替代 active 判定；Loops 记录已结束的统计循环，不能证明将来不会再次执行。
- 已确认 `rows_apply_lower_bound` 在已有 EXACT 时直接保持该精确值，即使新下界更大也只给诊断；同轮 EXACT 首个生效，后续轮 EXACT 可覆盖；原始 EXACT(0) 与输出 Hint 最小值 1 分别保存。
- 原型可在 SAMPLING 状态下另存一个小于当前采样值的下界。`rows_apply_sampling` 只检查证据类型，在这一状态下可能再次刷新采样值；是否始终保留该已存下界的约束需单独验证。正文中的“采样不覆盖已采用下界”不能扩大为所有内部采样刷新路径均已证明满足下界约束。
- 已确认 `feedback_effective_timeout` 使用完整最佳时间的 1.25 倍、配置上限及扣除收尾预留后的剩余预算。代码按毫秒取整，并为正期限保留至少 1 毫秒；PPT 和正文使用连续时间公式描述主体规则。
- JOB 16b 的 11 秒仅为恢复候选独立测量。实测总轮次不保证增加；CPU、I/O、内存和生产并发效果尚未独立测量，因此未列为本次交底书的量化收益。

### 6.4 英文对照

The disclosure and seven-slide manuscript now use the agreed title and the same feedback rules. Both completed and cancelled candidates can supply qualified row feedback; only complete runtimes update the best measured candidate and adaptive deadline. The merge formula explicitly preserves an existing valid exact count against every new lower bound. Drafting guidance and novelty assessments were removed from the disclosure and retained in this audit record. Implementation gaps remain documented here; the rewrite does not establish that the prototype implements every proposed qualification condition. The approximately eleven-second JOB 16b result remains a separately measured recovered candidate, not a formal delivery.

## 七、2026-10-22 最新论文与产品手册成果回写

### 7.1 交底书修改范围

| 位置 | 修改内容及依据 |
| --- | --- |
| 1.3.1 IBM POP | 补入完整方法论文与演示论文的关系，以及同一 SQL 内重新优化、实际基数反馈和可选中间结果复用；正文参考资料 [R24][R25] |
| 1.3.2 Oracle | 分别陈述非等价后续查询的授权主题、Statistics Feedback 和 SQL Plan Directives；将论文的限时统计查询放在 1.4.3，避免与用户主 SQL 混用 |
| 1.3.4 Microsoft | 补入 2004 论文对算子结束事实的使用，以及 2016 LQS 论文对早期上下界技术的明确引用；正文参考资料 [R26][R27] |
| 1.4.2 产品观测 | 补入 Db2 的统计接口、SQL Server 的关闭时间及生命周期手册，区分产品接口、扫描结束和目标行数反馈；正文参考资料 [R28]-[R30] |
| 1.5 方案对照 | 承认已有扫描结束、重复执行、关闭事件、精确上下界和部分执行反馈，比较其与取消时统计采集、适用判定和跨轮合并的数据关系 |
| 2.1、2.3.3、2.3.4、3.1 | 将上层提前停止、取消清理导致停止的情形纳入行数适用说明，保持本地 EOF 与目标全部输出结束的区分 |
| 参考资料及英文摘要 | 新增九项论文/手册依据；三件主要专利改用已核验的 PDF 页码，清除把 HTML 元素编号当作 PDF 说明书段号的引用 |

正文继续采用技术说明写法，不写“不能作为区别点”“应当怎样撰写权利要求”等批注。核查结论和实现限制在本记录中保存。

### 7.2 需要持续保持的事实区分

- **公开产品语义与反馈判定不是同一层。** 新增的 [SQL Server 算子手册](https://learn.microsoft.com/en-us/sql/relational-databases/showplan-logical-and-physical-operators-reference?view=sql-server-ver17)使“未见任何结束识别机制”这一概括不成立。本文只保留“所核资料未展示完整取消现场判定及跨轮合并流程”的有限结论。
- **不同年代和类别分别引用。** 现行产品手册不能直接当作 2004 年论文或 2009 年授权专利逐项记载相同细节的证据，也不能仅凭产品论文证明某产品实施了整件专利的全部权利要求。正文分别列出这些来源，不作授权判断。
- **IBM 数据与统计分别处理。** POP 可反馈实际基数，并按成本选择是否复用临时物化结果；不能简化成只复用数据、不反馈基数。两篇论文属于同一工作的方法说明和原型演示。
- **Oracle 限时对象分别处理。** 2017 论文第 3.2.2 节的限时对象是优化阶段统计查询；专利及手册中的主查询反馈、自适应子计划和后续查询复用是另外的处理路径。
- **方案条件与原型边界分别保存。** 本轮依据文献完善正文条件，原型尚需落实的再次执行、累计计数及跨轮语义等条件仍见第五节和 6.3。本轮文稿修订不代表这些代码差距已解决。
- **PPT 文稿与实验结论核对。** 七页中英文文稿原有的精确行数/下界、正常完成与取消、期限公式及 16b 恢复候选身份继续适用。新增内容为交底书的现有技术和适用条件细化，没有改变既有实验数据或将约 11 秒候选改为正式交付。

### 7.3 English summary

The disclosure now includes the verified POP papers, Oracle's statistics-query budget and feedback paths, and Microsoft's progress-estimation papers and operator-lifecycle manual. It acknowledges existing operator termination information and separates that information from cancellation-time feedback qualification and merging across tuning rounds. The patent body contains factual technical descriptions, while literature assessments and prototype gaps remain in this audit record. Publication dates, research prototypes, product interfaces, and individual patent claims are treated separately.

## 八、2026-10-26 按 IBM、Oracle、Microsoft 重组现有技术

按照用户要求，交底书不再以“执行反馈”“运行中重优化”“运行观测”分别拆分同一厂商的技术，而是在同一节集中陈述对应专利、论文、产品功能及与本发明的关系。

| 现行章节 | 集中纳入的内容 |
| --- | --- |
| 1.2 现有相关技术一：IBM | LEO、US7716215B2、两篇 POP 论文、section_actuals 与 EXPLAIN_FROM_ACTIVITY |
| 1.3 现有相关技术二：Oracle | US11321317B2、自适应计划、主查询统计反馈、SQL Plan Directives、限时统计查询与 SQL Monitor |
| 1.4 现有相关技术三：Microsoft | US7493337B2、2004 与 2016 论文、US11334538B2/CE feedback、LQS、DMV 字段及算子生命周期 |
| 1.5 补充相关技术与综合对照 | Sybase、采样重优化、Databricks 及其他产品观测；综合表也按厂商排列 |

三节统一使用“现有技术方案—与本发明的关系”结构。改写前后第一章使用的 27 个文献引用编号相同。第 2 章的结构调整见第九节，实验数据、参考文献和英文摘要继续保留。专项核查文件中的现行章节指向同步更新；第六、七节的旧章节号作为有日期的历史记录保留。

English: The related-art chapter now groups IBM, Oracle, and Microsoft into the three principal sections. Each section combines its patents, papers, product interfaces, and relationship to the proposed method. Other work remains supplementary. The literature citation set is preserved; the accompanying Chapter 2 restructuring is recorded below.

## 九、2026-10-26 按技术要点重组具体实施例

按用户追加要求，第 2 章改为以下顺序：第 2.1 节用一段话说明技术问题，第 2.2 节用一段话概括主要发明要点；第 2.3 节介绍系统架构或应用场景；第 2.4 节将各技术要点组织为连续编号的具体实施例。

| 该轮实施例（后续调整见第十一节） | 集中说明的内容 |
| --- | --- |
| 2.4.1 实施例一：限时试运行与期限调整 | 候选生成、正常完成与取消两条结果路径、初始上限、1.25 倍期限规则、全局预算和继续迭代 |
| 2.4.2 实施例二：超时统计采集与返回 | QueryDesc/PlanState/Plan/Instrumentation 字段来源、清理前采集、active/terminal 公式、原取消错误保留、GaussDB 及客户端读取方式 |
| 2.4.3 实施例三：行数适用与再次执行判定 | 目标 k、A/E/C、语义及执行次数一致、本地 EOF、后续调用、提前停止及累计计数反例 |
| 2.4.4 实施例四：分类、合并与下一轮应用 | EXACT/LOWER_BOUND/NONE、精确值优先、下界取最大值、同轮与跨轮规则、零行编码及两组更新案例 |
| 2.4.5 实施例五：GaussDB 调优与效果验证 | 原 JOB/JOB-complex 对照方法、两类评估、调优收益及未正式交付恢复候选的说明 |

原来分散的对象定义、统计采集、核心流程及补充小例子分别归入相应实施例。合并公式、执行状态公式、期限公式、原始性能表及恢复候选身份保留；第 3.3 节的期限说明更新为引用现行第 2.4.1 节。方案条件与原型仍有的差距继续保存在第五节和 6.3，不以结构重组代替实现验证。

English: Chapter 2 now begins with two short paragraphs covering the problem and the principal inventive approach, followed by system architecture and five numbered embodiments. The runtime formulas, classification and merge conditions, performance evidence, and recovered-candidate distinction are retained. Section references are updated without adding implementation claims.

## 十、2026-10-26 精简三家技术与本发明关系的重复内容

三处均采用“与本发明的关系”标题。原先各节反复描述取消、行数分类、合并和下一轮规划，本轮将整体流程集中到第 1.5.2 节，并分别保留直接关联该厂商技术的比较重点。

| 厂商 | 关系章节的比较重点 |
| --- | --- |
| IBM | CHECK 驱动的当前查询重优化与外部限时候选迭代：比较触发方式、执行上下文和中间结果复用；保留 IBM 同时利用实际基数的事实 |
| Oracle | 对象、操作或表达式相关性与节点观测可用性：比较统计的匹配条件，以及行数覆盖的目标和执行次数 |
| Microsoft | 查询进度和估算模型反馈与下一轮行数输入：比较反馈去向、更新方向及精确值/下界的合并优先级 |

综合表按这三个维度对应陈述。现有技术方案部分及第 2 章起的实现、公式、实验、参考资料和英文摘要保持上一发布版本的内容；该轮只精简第一章的关系说明和综合对照，并沿用第九节的结构。效果验证的后续移位见第十一节。

English: The three relationship sections now focus respectively on execution control and result reuse, statistical matching and row-count qualification, and feedback destination and merge policy. The common workflow is stated once in the comparison section. The technical embodiments and evidence remain those of the preceding published version.

## 十一、2026-10-26 将实验验证移入创新点与技术效果章节

按用户要求，原第 2.4.5 节的 GaussDB/JOB/JOB-complex 对照验证不再作为技术实施例。第 2.4 节现只保留四个技术实施例；第 3 章改为“本发明的主要创新点及其对应的技术效果”，按主要创新点、对应技术效果和效果验证组织。

| 原位置 | 现行位置与处理 |
| --- | --- |
| 2.4.5 GaussDB 调优与效果验证 | 完整移至 3.3 技术效果验证，保留实验条件、表格、图 8/9、正式交付与恢复候选的区别 |
| 3.1 关键技术点、3.2 有益效果 | 分别调整为主要创新点、对应的技术效果，并保留技术处理与效果的对应表 |
| 原 3.3 其他实施方式 | 可配置期限和倍率归入实施例一；消息/结构化统计接口归入实施例二；其他基数输入接口归入实施例四；原有不反馈及失效条件已在实施例三、四说明 |

背景及效果说明中的实验章节引用统一改为第 3.3 节。验证内容按原文迁移，统计值、实验方法、31.89%/41.84% 降幅及约 11 秒恢复候选未正式交付的说明保留。原始参考资料及英文摘要保持一致。

English: Four technical embodiments remain in Chapter 2. The complete GaussDB workload evaluation is now in Section 3.3 under the invention's principal innovations and corresponding technical effects. Optional implementation variations are folded into their relevant embodiments, while measurements and evaluation distinctions are preserved.

## 十二、2026-10-26 全文中文表达复核

本轮通读交底书的摘要、背景、三家现有技术、系统架构、四个实施例、创新点与技术效果、实验说明、图注、表格及实现依据，集中处理省略对象关系、动作主体不清、代码式短语和指代不明等问题。通过 CodeGraph 核对 GaussDB 适配文件中的对象定义，确认 `plan` 与 `instrument` 都是执行状态对象中的指针成员，分别指向计划信息和执行统计。

| 问题类型及原表达 | 修改方式 |
| --- | --- |
| 对象关系省略：“每个 PlanState 通过 plan 关联静态 Plan，通过 instrument 关联运行时 Instrumentation” | 明确 PlanState 包含两个指针，分别说明所指对象及其保存的数据；字段表按“数据所在对象—读取字段—字段含义”组织 |
| 把数据当作动作主体：“完整运行时间更新最佳候选”“节点统计进入行数反馈” | 说明由调优器比较完整运行时间、选择最佳计划；节点统计交由行数判定单元处理 |
| 计数含义不完整：“实际与估算覆盖相同执行次数” | 分别写明实际行数和估算行数，要求二者对应相同执行次数及并行汇总范围 |
| 更新对象模糊：“精确行数覆盖已有输入”“行数下界只抬高” | 明确更新的是调优器保存的规划输入值；新精确值替换旧值，尚无精确值时下界才提高该值 |
| 不同阶段的数值混淆 | 分别说明计划估算 E 和调优器保存的值 B，解释判断下界是否可用与判断 Hint 是否变化是两个步骤 |
| 动作与结果混写：“形成独立完整执行约 11 秒的恢复候选” | 分开说明依据累计 Hint 生成候选、进行独立完整执行测量，以及原调优过程中未正式交付 |
| 统计说明不直观：“未交付查询回填原 SQL” | 明确在计算执行时间时，对未交付查询使用原始 SQL 的执行时间 |
| 裸露英文功能名和压缩短语 | 在正文首次使用时先说明中文含义，再保留英文标准术语；补全调用、比较和使用统计的主体 |

PPT 中英文文稿第 3 页同步明确指针与对象的关系；中文第 4-5 页同步改写相同执行次数和更新规则的表述。此处更新的是 Markdown 文稿。

本轮保留三家现有技术的分节方式、四个技术实施例及第 3.3 节效果验证的位置。执行期限、状态判定、行数分类和合并公式保持原文；实验表中的数据及恢复候选身份保留。英文摘要核对后保留，其概括的技术规则与修订后的中文一致。原型尚未落实的条件继续见第五节和 6.3。

English: The Chinese disclosure was reviewed throughout for explicit object ownership, pointer targets, action subjects, counting ranges and evaluation procedures. The corresponding slide-manuscript wording was aligned. The structure, formulas, measurements, reference identifiers and recovered-candidate distinction are retained; this language revision does not change implementation behavior.

## 十三、2026-10-26 调整 Oracle 与 Microsoft 的对照重点

按用户要求，现有相关技术二改为突出单条查询执行过程中的重新优化，以及保存完整或部分执行信息供后续查询规划使用。关系说明据此对照本发明在全局预算内组织的多轮独立候选调优，不再以统计匹配条件作为主要比较线索。

现有相关技术三集中于 US7493337B2、2004 年查询进度论文、2016 年 LQS 产品论文及相关统计接口。移除交底书中的 US11334538B2/CE feedback 段落及对应参考项 [R22]、[R14]，关系说明只比较当前查询进度估计与后续候选计划优化的不同用途。综合表、章节标题和英文概述同步调整。

为保持三节各自的重点，IBM 关系说明仅保留 CHECK 触发与物化中间结果复用的对照，并承认其同样使用实际基数。Oracle 比较查询内调整、后续执行信息利用与外部多轮调优；Microsoft 比较进度估计与下一轮规划。

本轮是交底书选题和叙述范围调整，原独立核查报告、US11334538B2 原件及其检索记录保留，不据此推断 Microsoft 没有基数反馈能力。代码实现、四个实施例、公式、实验数据和恢复候选身份保留。

English: The Oracle comparison now covers in-flight reoptimization and stored execution information for later queries. The Microsoft related-art section is limited to progress estimation and associated LQS interfaces, contrasting that purpose with next-round plan optimization. CE-feedback material is omitted from the disclosure while its independent research evidence is preserved.

## 十四、2026-10-27 按统计采集的核心过程简化实施例二

实施例二围绕“找到节点统计—清理前保存—识别调用状态和本地EOF—交由后续行数判定”组织，统一使用数据库执行器和调优器两个已在系统架构中说明的对象。不再以服务端、客户端、适配层的分工作为叙述主线。按用户后续要求，正文采用连续自然段，不使用编号列举，中文与英文之间不添加空格。

正文删除具体函数名、字段清单、active/terminal 的字段计算式、通信和错误处理细节，以及客户端重新规划并核对计划的实现过程。保留 GaussDB 的简短对象示例，说明 PlanState 中的指针分别指向计划信息和执行统计；实现依据仍见 [C2]、[C3] 所列源码，图 3、图 4 继续展示具体来源与采集时点。

技术边界保留：统计在取消清理前取得，本地 EOF 与当前无调用不能证明后续不会再次执行，缺少统计不等于零行，正常完成也可提供行数反馈。实施例三、四继续负责行数适用条件、分类和合并。本轮只调整实施例二及其图注，其他实施例、期限与合并公式、实验数据及恢复候选身份不变。

English: Embodiment 2 now explains how to locate node statistics, preserve them before cleanup, distinguish invocation state from local EOF, and pass the observations to feedback qualification. A short GaussDB example remains, while function names, field formulas, transport details and client-side plan reconstruction are omitted from the main narrative. The technical limits and the other embodiments are unchanged.

## 来源说明

- 两篇 PDF 已按其本地文件的 **PDF 页码**逐页核对，论文内印刷页号可能不同。
- Oracle 原论文及 Oracle/Microsoft 官方页面均已直接核对；指定知乎文章当前无法读取正文，[O2] 的技术事实已改由原论文支撑。
- 仓库源码为当前工作树状态；本笔记不据此推断数据库厂商未公开的内部实现，也不作专利新颖性结论。
