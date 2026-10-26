# Microsoft US7493337B2：相关论文与产品手册

初次核查日期：2026-10-20；算子结束机制补充核查：2026-10-22。研究对象为 *Query Progress Estimation*，即 [US7493337B2](https://patents.google.com/patent/US7493337B2/en)。本记录区分早期同一技术工作的论文、后续产品实现论文及官方功能手册。

## 1. 核查结论

存在直接相关论文，而且能够找到从早期研究到 SQL Server 产品功能的公开技术联系：

1. 2004 年论文 *Estimating Progress of Execution for SQL Queries* 的三名作者与该专利三名发明人对应，工作量模型、执行计划流水线、行数上下界和执行反馈修正等机制也对应。
2. 2016 年论文 *Operator and Query Progress Estimation in Microsoft SQL Server Live Query Statistics* 明确介绍 SQL Server 2016 LQS 的设计与实现；其第 4.2 节明确采用参考文献 [7] 的算子基数边界技术，而 [7] 正是上述 2004 年论文。
3. SQL Server 官方手册公开 LQS 界面、运行中逐算子计数及其 profiling 基础设施。产品论文提供算法联系，手册提供用户可使用的功能与接口说明；这组证据不等于已逐项确认该产品实施了 US7493337B2 的全部权利要求。

依据：[专利发明人及正文](https://patents.google.com/patent/US7493337B2/en)、[2004 年作者机构页面](https://www.microsoft.com/en-us/research/publication/estimating-progress-of-execution-for-sql-queries/)、[2016 年作者机构页面](https://www.microsoft.com/en-us/research/publication/operator-and-query-progress-estimation-in-microsoft-sql-server-live-query-statistics/)、[2016 年论文全文](https://www.microsoft.com/en-us/research/wp-content/uploads/2016/06/qprog.pdf)。

## 2. 最直接相关的研究论文

**Estimating Progress of Execution for SQL Queries**，Surajit Chaudhuri、Vivek Narasayya、Ravishankar Ramamurthy，SIGMOD 2004，会议论文集第 803-814 页。

- [Microsoft Research 论文页面](https://www.microsoft.com/en-us/research/publication/estimating-progress-of-execution-for-sql-queries/)
- [Microsoft Research 提供的全文 PDF](https://www.microsoft.com/en-us/research/wp-content/uploads/2016/02/progress-1.pdf)
- [DOI：10.1145/1007568.1007659](https://doi.org/10.1145/1007568.1007659)

论文以算子输出行数或 GetNext 调用建立查询工作量模型，按流水线及其驱动节点估算进度。第 4.2 节在执行中维护基数上下界：已返回量可作为下界，估算超界时修正，边界还可沿计划传播；算子结束时上下界相同。作者页将第三位作者简写为 Ravi Ramamurthy，PDF 署名为 Ravishankar Ramamurthy，与专利署名对应。[论文全文](https://www.microsoft.com/en-us/research/wp-content/uploads/2016/02/progress-1.pdf)、[专利发明人列表](https://patents.google.com/patent/US7493337B2/en)。

最值得阅读的位置为 PDF **Pages 5-6，第 4.1-4.2 节**；PDF 第 12 页 Appendix B 补充各算子的上下界规则。这里的 PDF 页码从论文第一页算起，区别于会议论文集的印刷页码。[论文全文](https://www.microsoft.com/en-us/research/wp-content/uploads/2016/02/progress-1.pdf)。

关联强度：**直接技术对应**，依据为发明人/作者对应及技术机制对应。本记录没有将 Google Patents 的 Similar Documents 列表当作专利审查引用清单，也不凭该列表断言论文被审查员引用。

## 3. 最值得补入的产品实现论文

**Operator and Query Progress Estimation in Microsoft SQL Server Live Query Statistics**，Kukjin Lee 等，SIGMOD 2016。作者包括 Vivek Narasayya、Surajit Chaudhuri 等，Microsoft Research 页面列出完整作者名单。

- [Microsoft Research 论文页面](https://www.microsoft.com/en-us/research/publication/operator-and-query-progress-estimation-in-microsoft-sql-server-live-query-statistics/)
- [Microsoft Research 提供的全文 PDF](https://www.microsoft.com/en-us/research/wp-content/uploads/2016/06/qprog.pdf)
- [DOI：10.1145/2882903.2903728](https://doi.org/10.1145/2882903.2903728)

该论文直接以 SQL Server 2016 LQS 为实现对象。服务端通过 DMV 提供执行计数，客户端计算并显示查询和算子进度；第 4.2 节沿用 2004 年工作的保守上下界技术，同时结合新的在线基数修正及对阻塞、缓冲、并行等执行特征的处理。[论文全文](https://www.microsoft.com/en-us/research/wp-content/uploads/2016/06/qprog.pdf)。

| 阅读位置 | 核查内容 |
| --- | --- |
| PDF Pages 1-2，第 2 节 | LQS 的产品架构、服务端计数与客户端进度计算 |
| PDF Pages 3-4，第 3.1-3.3 节 | GetNext 工作量模型及研究方法向产品扩展时的问题 |
| **PDF Page 6，第 4.2 节** | 明确复用参考文献 [7] 的每算子最坏情况上下界技术；估算越界时修正到最近边界 |
| PDF Page 11，参考文献 [7] | 指向 2004 年的三作者进度估计论文 |

上述页码与章节均根据 [2016 年原文 PDF](https://www.microsoft.com/en-us/research/wp-content/uploads/2016/06/qprog.pdf)核对。关联强度：**有明确文献引用和算法复用证据的产品后续工作**。其最终用途仍为当前查询进度估计，而不是取消候选后更新下一轮 Rows Hint。

## 4. 可直接引用的官方产品手册

| 官方页面 | 已公开的产品能力 | 使用条件与可证明范围 |
| --- | --- | --- |
| [Live Query Statistics](https://learn.microsoft.com/en-us/sql/relational-databases/performance/live-query-statistics?view=sql-server-ver17) | SSMS 显示活动查询的实时执行计划、整体进度、节点返回行数、已耗时间和算子进度；内部使用 `sys.dm_exec_query_profiles` | 主要用于故障诊断；需启用 profiling。页面提醒其有额外开销，尤其在 SQL Server 2014。证明用户可见的运行中统计功能 |
| [`sys.dm_exec_query_profiles`](https://learn.microsoft.com/en-us/sql/relational-databases/system-dynamic-management-objects/sys-dm-exec-query-profiles-transact-sql?view=sql-server-ver17) | 按算子、线程给出 `node_id`、`row_count`、`estimate_row_count`、`end_of_scan_count`、`rewind_count`、`rebind_count`、`close_time` 及活动时间 | 需在查询开始前启用 profiling；并行线程需按目标范围汇总。扫描结束、关闭事件与全部目标行数是否可用于后续反馈分别解释 |
| [Query Profiling Infrastructure](https://learn.microsoft.com/en-us/sql/relational-databases/performance/query-profiling-infrastructure?view=sql-server-ver17) | 解释标准与轻量 profiling、LQS 与 DMV 的统计来源，以及各版本的启用方式 | SQL Server 2016 SP1 至 2017 有轻量 profiling v2；2019 及以后版本有默认启用的 v3。证明采集基础设施的产品化，不直接规定取消后行数反馈合并算法 |
| [Logical and Physical Showplan Operator Reference](https://learn.microsoft.com/en-us/sql/relational-databases/showplan-logical-and-physical-operators-reference?view=sql-server-ver17) | 说明算子的初始化、取数、关闭、扫描结束及重复执行语义 | 明确一次扫描结束与算子生命周期不同；该产品语义说明应与早期论文及专利分别引用 |

这些链接为 Microsoft Learn 官方页面，本文核查时使用 `sql-server-ver17` 视图。不同版本的启用、权限及开销条件应以对应版本手册为准。

### 算子结束机制的补充结论

2004 年论文第 4.2 节利用“算子结束时上下界精确已知”的事实；官方算子手册则进一步描述生命周期与事件语义。因此，不能将资料概括为“Microsoft 没有算子结束信息或识别机制”。论文、生命周期手册及 DMV 字段分别承担算法说明、执行语义说明及观测接口说明，具体事实与引用已集中写入[交底书第 1.4 节：现有相关技术三](patent-timeout-feedback-disclosure-draft.md)。本次仍未据此确认取消现场完整的目标级基数判定和跨轮 Rows Hint 合并流程。

## 5. 放入现有技术材料的建议表述

> Microsoft 的查询进度估计专利与 SIGMOD 2004 的进度估计研究直接相关；SIGMOD 2016 的 SQL Server LQS 产品论文又明确采用了该早期研究的算子基数上下界技术。SQL Server 官方手册公开运行中查询与算子进度、实际返回行数及估算等接口。上述机制主要用于当前查询的进度显示和诊断。

技术对照时可分别使用 [2004 年论文](https://www.microsoft.com/en-us/research/wp-content/uploads/2016/02/progress-1.pdf)说明上下界规则、[2016 年产品论文](https://www.microsoft.com/en-us/research/wp-content/uploads/2016/06/qprog.pdf)说明算法与 LQS 产品的联系、[官方手册](https://learn.microsoft.com/en-us/sql/relational-databases/performance/live-query-statistics?view=sql-server-ver17)说明可用功能。不能仅因 LQS 具备节点行数或扫描结束计数，就推断其实施了我方取消现场精确值/下界/无反馈的全部判定及跨轮 Rows Hint 合并。

## English summary

US7493337B2 and the SIGMOD 2004 paper have matching inventors/authors and closely corresponding progress-estimation mechanisms. The SIGMOD 2016 SQL Server Live Query Statistics paper explicitly reuses the 2004 paper's conservative operator cardinality bounds in Section 4.2 and cites it as reference [7]. Microsoft Learn documents the user-visible LQS functionality, operator/thread counters, profiling infrastructure, and operator lifecycle. The supplementary review distinguishes these sources from a complete cancellation-time feedback qualification and merge procedure. These sources establish a research-to-product technical connection, without establishing that every claim of the patent is implemented by a particular SQL Server version.
