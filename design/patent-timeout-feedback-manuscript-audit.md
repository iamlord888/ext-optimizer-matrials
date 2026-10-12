# 超时执行反馈交底书：文献与机制独立审计

本笔记记录[交底书正文](patent-timeout-feedback-disclosure-draft.md)的文献依据和当前代码与方案的差距，不代替专利新颖性检索。核对范围包括用户提供的两篇论文、Oracle 12c 原论文、指定解析博客、所列 Oracle/Microsoft 官方文档及当前仓库源码。以下的“未见公开”仅指所列资料没有描述该步骤，不能推断产品内部绝不存在该能力。第三、四节保留初稿审计时的改写依据；当前待实现条件见第五节。

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

## 来源说明

- 两篇 PDF 已按其本地文件的 **PDF 页码**逐页核对，论文内印刷页号可能不同。
- Oracle 原论文及 Oracle/Microsoft 官方页面均已直接核对；指定知乎文章当前无法读取正文，[O2] 的技术事实已改由原论文支撑。
- 仓库源码为当前工作树状态；本笔记不据此推断数据库厂商未公开的内部实现，也不作专利新颖性结论。
