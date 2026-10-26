# 一种基于限时执行部分反馈的查询执行计划迭代优化方法

## 缩略语

| 英文缩略语 | 英文表述 | 中文术语 |
| --- | --- | --- |
| SQL | Structured Query Language | 结构化查询语言 |
| QEP | Query Execution Plan | 查询执行计划 |
| E-rows | Estimated Rows | 执行计划节点的估算输出行数 |
| A-rows | Actual Rows | 执行计划节点的实际输出行数；在取消时的统计快照中指截至采集时的累计返回行数 |
| EOF | End of File / end-of-stream signal | 当前调用已读到数据流结束的信号 |
| LEO | DB2's LEarning Optimizer | DB2 学习型优化器 |
| CE | Cardinality Estimation | 基数估计 |

## 摘要

本发明公开一种基于限时执行部分反馈的查询执行计划迭代优化方法。对同一 SQL 的各轮候选设置执行期限；候选超时取消时，在执行器清理前取得原计划及节点行数、调用状态和 EOF 标志。根据节点是否已返回全部相关行数、是否还会再次执行，以及实际与估算行数的计数范围，将统计分为精确行数、可用行数下界和无反馈。精确行数覆盖已有输入，下界在无精确值时仅提高输入，无反馈时保持原值，合并结果用于下一轮独立规划。正常完成的候选也可提供行数反馈，其完整运行时间用于比较最佳候选并调整后续执行期限。该方法控制慢候选的试运行时间，保留有效统计，为全局调优预算内的后续迭代争取时间。

## 1 相关技术背景以及最接近的现有技术

### 1.1 背景技术

数据库运维和自动调优平台通常需要在限定时间窗口内，为一批慢 SQL 寻找更快的查询执行计划。调优器逐轮生成并试运行候选，依据节点实际行数修正下一轮的基数输入，并使用完整运行时间比较候选性能。某个候选长时间未结束时，会推迟行数反馈和下一轮试运行，使剩余预算不足以继续评估其他候选。直接取消可以结束等待，但常规 `EXPLAIN ANALYZE` 的正常结果路径会被中断；若未保存节点统计，本轮已经取得的行数信息将无法用于后续调优。

优化器根据扫描、过滤、连接等操作的估算输出行数，选择访问路径、连接顺序和连接方法。基数误差会沿计划树传播。例如，连接结果被估算为几十行时，优化器可能选择由其驱动 Nested Loop；实际输出若达到数万行，内侧访问被反复调用，后续扫描、连接和排序的工作量也会增加。执行反馈调优通过比较估算行数与实际行数，修正后续规划的基数输入。

在同一 SQL 的多轮调优中，Rows Hint、Outline 等规划输入可以形成不同候选，各轮具有独立的物理计划和执行上下文。节点行数用于更新基数估算，完整运行时间用于比较已完成候选的性能。候选被取消时，已有节点行数仍可能具有反馈价值，但尚未取得该候选的完整运行时间。

例如，JOB `16b` 在无单轮超时的对照中运行 4 轮后触发全局调优预算停止条件，所交付候选的完整执行时间约为 36 秒。该记录反映了慢候选试运行对有限调优窗口的占用；具体实验条件及结果见第 2.4.5 节。[E1][E2]

取消时的节点统计还存在适用条件。有的节点已返回全部相关行数，有的节点只返回了部分行数，有的节点尚未执行。`Instrumentation.ntuples` 记录的累计返回行数不一定对应单次执行的估算；本地 EOF 也不排除父算子在后续外侧输入到来时再次执行该节点。因此，既需要在清理前保留统计，也需要区分精确行数、可用下界和无反馈，避免将未完成输出或跨多次执行的累计输出作为最终基数。

本说明书所称“部分反馈”，是指整轮候选尚未完整执行时取得的可用节点统计，包括满足判定条件的精确行数和行数下界。部分反馈用于同一 SQL 的下一轮规划，复用的信息为节点行数和执行状态。

![图1 慢候选的执行期限与反馈时点](patent-timeout-feedback-figures/png/fig1-conventional.png)

*图1 持续等待、直接取消及取消时保留反馈三种处理路径。候选的计划结构已经生成，限时取消前产生的合格节点统计可用于下一轮规划。*

### 1.2 现有相关技术一：IBM 的执行反馈与渐进式查询优化

#### 1.2.1 现有技术方案

IBM 的执行反馈研究包括面向后续查询的基数学习，以及面向当前查询的渐进式重新优化。DB2 的 LEO 在计划和执行过程中积累算子计数，查询完成后保存学习信息，用于改进后续查询的基数估计和计划选择。[R1]

渐进式查询处理专利 US7716215B2 则在当前计划中设置 `CHECK` 算子，比较估算基数与执行中已处理的行数。其权利要求 1 根据剩余执行时间阈值和估算误差阈值决定是否暂停当前计划，随后重新优化并重新启动查询。说明书描述，输出越过上界时可在子流尚未结束前确认偏差；根据输出不足判断低于下界时，需要等子流返回 EOF。该技术还可收集检查点或其他计划节点的基数统计。[R20]

《Robust Query Processing through Progressive Optimization》详细介绍上述 POP 方法，《Progressive Optimization in Action》展示其 Db2 研究原型。CHECK 触发后，系统将已形成的中间结果作为临时物化视图或物化查询表提供给优化器，同时反馈实际基数。优化器比较复用中间结果与重新计算的候选计划，按成本选择；临时视图也可指向已有内存对象。重新优化和后续执行在同一事务中继续完成当前 SQL。[R24][R25]

在产品统计接口方面，Db2 的 `section_actuals` 可采集基本算子基数计数，并通过 `EXPLAIN_FROM_ACTIVITY` 查看某次执行的计划及运行统计。该过程的使用说明列明，通过 `WLM_CAPTURE_ACTIVITY_IN_PROGRESS` 取得的运行中活动记录不包含 section actuals。[R29][R30]

#### 1.2.2 与本发明的关系

IBM POP 在计划内检查点触发后，于同一事务中重新优化当前 SQL，利用实际基数，并按成本选择是否复用已物化的中间结果。本发明在外部调优过程中控制各轮独立候选的执行期限，以保存的节点统计关联后续规划。两者的主要差异在于重新优化的触发方式、各轮执行上下文及中间结果复用的参与方式。[R20][R24][R25]

### 1.3 现有相关技术二：Oracle 的自适应计划与统计反馈

#### 1.3.1 现有技术方案

Oracle 专利 US11321317B2 描述自适应子计划选择、运行中重新优化及后续查询统计复用三类处理。Adaptive plan 在决策点根据统计收集器取得的行数、选择率等信息选择预置子计划；in-flight reoptimization 根据运行统计与编译估算的差异、剩余操作和更新后的成本，提前终止未完成计划并启动新计划。专利背景还涉及用户或 DBA 终止运行过久的计划。[R19]

Automatic reoptimization 将完整或部分执行时收集的表、连接、分组和索引统计，用于相同、等价或相关的后续查询。该专利授权权利要求 1 涉及选择保存某个操作的统计，并根据数据库对象和操作，判断统计是否适用于后续非等价查询，再生成第二执行计划。[R19]

Oracle 产品手册中的 Adaptive Query Plans 在当前执行期间，利用 statistics collector 收集行数并选择预置子计划。Statistics Feedback 在执行结束时比较各操作的估算与实际行数，并使用修正值改进后续规划；SQL Plan Directives 按查询表达式保存可复用信息，可供不同 SQL 使用。[R8][R31][R32]

2017 年论文《Adaptive Statistics in Oracle 12c》进一步描述自适应统计及指令的形成、缓存和后续利用。其中第 3.2.2 节说明，优化阶段的统计查询按历史运行信息分配时间预算，到达期限后停止并使用已取得的部分结果。该时间预算约束用于估算基数的统计查询；上述 Statistics Feedback 手册所述流程则在主查询执行结束时处理节点统计。[R3][R31]

在运行观测方面，Oracle SQL Monitor 在取消长查询后仍可显示计划节点的 `Rows (Actual)`。`V$SQL_PLAN_MONITOR.OUTPUT_ROWS` 表示执行开始后的累计输出，可结合 `STARTS` 分析每次启动的平均输出。[R4][R9]

#### 1.3.2 与本发明的关系

Oracle 的统计复用路径按数据库对象、操作和查询表达式组织完整或部分执行取得的信息。本发明重点判断取消时每份节点行数是否可用于基数修正：确认对应的扫描或连接，使实际与估算覆盖相同执行次数，再区分精确行数、可用下界或无反馈。这里需要区分统计与后续查询的相关性，以及该观测能否作为所需基数输入这两个条件。[R19][R31][R32]

### 1.4 现有相关技术三：Microsoft 的查询进度估计与基数反馈

#### 1.4.1 现有技术方案

Microsoft 专利 US7493337B2 对当前查询的算子维护已处理行数和预计总行数的上下界，传播边界变化，修正进度估计。权利要求 27 将算子在某时点已返回的项目数设为下界；权利要求 33 涉及多个算子的上下界维护与传播；权利要求 42 允许用户中止查询。[R21]

相关论文《Estimating Progress of Execution for SQL Queries》以查询整体进度估计为目标，利用各算子的输出计数和上下界修正工作量估算，并说明算子结束时上下界精确已知且相等。2016 年的 SQL Server Live Query Statistics 产品论文明确采用这项早期研究的基数边界技术，同时估计查询整体与单个算子的进度。[R26][R27]

在基数反馈方面，专利 US11334538B2 描述查询执行期间及完成时产生监测事件，收集各计划节点的估算基数与实际基数，并根据差异形成反馈建议，影响后续相同或相似查询的执行。SQL Server 的 CE feedback 产品功能也根据重复查询的估算偏差，为后续规划形成并保存修正。[R22][R14]

SQL Server 的 `sys.dm_exec_query_profiles` 是系统动态管理视图，按算子和执行线程提供累计返回行数、估算行数、扫描结束次数、重复执行计数及关闭时间。Live Query Statistics 利用运行统计显示查询和算子进度。[R5][R6][R27]

| SQL Server 字段或事件 | 含义 |
| --- | --- |
| `row_count`、`estimate_row_count` | 截至当前已返回行数及估算行数 |
| `end_of_scan_count` | 截至当前发生的扫描结束次数 |
| `rebind_count`、`rewind_count` | 参数变化后重新执行或重复使用已有结果相关的计数 |
| `close_time` | 算子关闭时的时间戳 |
| `thread_id` | 区分同一算子的并行执行线程 |

SQL Server 的算子手册说明，物理算子通过 `Open()` 初始化、通过 `GetRow()` 取数、通过 `Close()` 清理并关闭。到达数据流末尾时，`ActualEndOfScans` 增加；Nested Loop 内侧算子可能多次到达末尾。每个物理算子只接收一次 `Close()`，但该调用不保证发生。手册还给出 Top 提前停止向子节点取数的情形，此时扫描节点可以未读完整个表。[R28]

#### 1.4.2 与本发明的关系

Microsoft 的进度估计将节点计数和上下界用于当前查询的工作量与完成比例估算，CE feedback 则通过修正估算模型影响后续计划。本发明将合格节点行数合并为下一轮 Rows Hint 等基数输入，精确行数允许上调或下调，下界在无精确值时仅抬高输入。两者在此处的比较重点是反馈用于何处，以及不同类型行数如何更新规划输入。[R21][R26][R27][R22][R14]

### 1.5 补充相关技术与综合对照

#### 1.5.1 其他相关技术

Sybase 专利 US9213740B2 在计划级别学习候选性能。执行被中止后仍可记录部分 `plancost` 和已耗时间；如果已耗时间超过当前最佳计划，可将该候选记录为应避免的计划，否则可将本次中断视为无效训练执行。其主要依据为候选计划的部分耗时和性能指标。[R23]

Sampling-Based Query Re-Optimization 在正式执行前，对样本运行候选计划，依据连接选择率迭代重规划；采样过程可以设置时间预算，预算到达时返回已有候选中估计成本较好的计划。其估计器具有无偏和强一致性质，有限样本仍存在方差、采样成本及覆盖范围限制。[R2]

Databricks AQE 在当前查询执行期间，利用已完成阶段的统计选择后续执行方式。MariaDB、Redshift、OceanBase 等产品提供执行中的计划或步骤统计，可用于监控、诊断或进度分析。[R18][R15][R16][R17]

#### 1.5.2 综合对照

本发明的整体处理流程为：候选限时试运行，在取消清理前保存节点统计，判定并合并可用行数，修正下一轮独立候选的基数输入；正常完成时取得的完整运行时间另行用于性能评价。以下分别从执行控制与复用对象、统计适用条件，以及反馈用途和更新方式进行对照。

| 相关技术 | 主要处理方式 | 本发明对应的处理 |
| --- | --- | --- |
| IBM | 检查点触发当前查询重优化，反馈实际基数并可选复用物化结果 | 外部期限控制独立候选试运行，以节点统计关联后续规划 |
| Oracle | 按数据库对象、操作或查询表达式匹配和复用执行信息 | 判定取消时的行数与目标、执行次数是否一致，确定可用类型 |
| Microsoft | 节点上下界用于进度估计，估算模型反馈影响后续计划 | 按行数反馈类型规定更新方向和优先级，形成下一轮基数输入 |
| 其他相关技术 | 利用部分耗时评价计划，或在样本上重规划及展示运行统计 | 将真实候选取得的节点统计作为限时迭代的输入 |

### 1.6 本发明要解决的技术问题

本发明要解决的技术问题是：在同一 SQL 的多轮候选调优中，控制单个慢候选的试运行时间；在超时取消时、执行器清理前取得节点行数与执行状态；判断其中哪些统计可作为精确行数或行数下界；将合格统计合并为下一轮基数输入，同时保持完整运行时间的候选评价。

## 2 本发明技术方案的详细阐述（发明内容）

### 2.1 本发明要解决的技术问题

本发明要解决的技术问题是：在全局调优预算内，某些候选计划长时间试运行会挤占后续迭代时间，而直接取消会中断正常执行统计的返回。同时，取消时各节点的执行进度、再次调用关系和计数范围不同，已返回行数不能统一作为最终基数。因此，需要在控制单轮执行时间的同时，及时保存可用节点统计，判定其适用类型，并用于后续候选规划。

### 2.2 本发明的主要发明要点概述

本发明为同一 SQL 的各轮独立候选设置执行期限，在超时取消、执行器清理前取得原计划及节点行数和执行状态；结合扫描或连接对应的目标、相关输出是否全部返回、是否还会再次执行及计数范围，将统计分为精确行数、合格下界和无反馈。精确行数覆盖已有输入，下界在无精确值时仅抬高输入，无反馈时保持原值，合并结果用于下一轮独立规划。正常完成的候选也提供行数反馈，其完整运行时间用于比较最佳候选，并动态调整后续执行期限。

### 2.3 本发明应用的系统架构或场景

如图2所示，系统包括候选生成、限时执行、节点统计采集、行数判定、反馈合并和迭代控制单元。

候选生成单元接收原始 SQL 和已有基数反馈，调用优化器生成第 i 轮候选计划 `P_i`。限时执行单元设置本轮执行期限 `T_i`，使用 `EXPLAIN ANALYZE` 试运行该候选。正常完成时，节点统计进入行数反馈，完整运行时间同时进入最佳候选比较；超时取消时，统计采集单元在执行器清理前读取原计划节点的信息，供行数反馈使用。

行数判定单元确定统计对应的扫描或连接，检查相关行数是否全部返回、是否还会再次执行，以及实际与估算的计数范围。反馈合并单元按反馈类型形成下一轮 Rows Hint 等基数输入。迭代控制单元据此继续生成候选，直到达到全局调优预算、收敛条件或其他停止条件。

上述单元可由数据库内的执行观测模块与外部调优器协作实现，也可集成于数据库服务。各轮候选分别建立执行上下文，通过合并后的节点行数反馈关联。

![图2 候选调优中行数反馈的应用位置](patent-timeout-feedback-figures/png/fig2-system.png)

*图2 第 i 轮候选正常完成与超时取消均可进入行数判定和合并，再生成第 i+1 轮候选；正常完成的完整运行时间同时用于最佳候选比较。*

### 2.4 本发明技术实现的具体实施例

以下实施例分别说明限时试运行、超时统计采集、行数适用判定和跨轮反馈合并，并给出 GaussDB 环境下的调优验证。

#### 2.4.1 实施例一：同一 SQL 的限时试运行与期限调整

本实施例说明如何为同一 SQL 的候选试运行设置期限，并通过已有行数反馈继续生成候选。设第 i 轮候选计划为 `P_i`，本轮实际执行期限为 `T_i`。调优器将原始 SQL 与当前 Rows Hint 等基数输入交给优化器，取得候选计划，并为该轮建立独立执行上下文。

候选通过 `EXPLAIN ANALYZE` 执行。正常完成时，调优器收集节点统计和完整运行时间：节点统计继续参与反馈，完整运行时间用于比较并保留最佳候选。达到执行期限时，调优器取消该轮执行，并按实施例二在执行器清理前取得节点统计。取消时已耗时间单独记录，合格行数继续用于后续规划。

初始单轮上限为可配置的 `T₀`。未取得完整运行时间时使用 `T₀`；得到同一 SQL 的最佳完整运行时间 `t_best` 后，后续上限取：

```text
T_cap = min(T₀, 1.25 × t_best)
```

实际执行期限还受剩余全局调优预算约束，并为取消、统计处理和重新规划预留时间。实验中 `T₀=10 秒`；若最佳完整运行时间为 4 秒，则后续上限为 5 秒。取消候选的已耗时间不用于更新 `t_best`。

调优器按照实施例三、四判定并合并本轮统计，再调用优化器生成下一轮候选。各轮执行上下文分别创建，前轮取得的合格统计通过基数输入影响后轮规划。迭代持续到全局调优预算、收敛条件或其他停止条件满足。

#### 2.4.2 实施例二：超时取消时读取并返回节点统计

本实施例以 GaussDB 普通行执行节点为例，说明统计对象的定位、读取时点及返回方式。

`QueryDesc.planstate` 指向本轮顶层算子的执行状态对象 `PlanState`。沿左右子节点、Append、Subquery、InitPlan、SubPlan 等关系遍历执行计划状态树，保存节点指针。每个 `PlanState` 通过 `plan` 关联静态 `Plan`，通过 `instrument` 关联运行时 `Instrumentation`。[C2]

| 对象或字段 | 读取内容 | 用途 |
| --- | --- | --- |
| `QueryDesc.planstate` | 顶层算子的执行状态对象 | 定位本轮执行计划状态树 |
| `PlanState.plan` → `Plan.plan_node_id` | 节点 ID | 定位本轮物理计划节点 |
| `PlanState.plan` → `Plan.plan_rows` | 估算行数 E | 提供本轮估算 |
| `PlanState.instrument` → `Instrumentation.ntuples` | 累计返回行数 A | 提供截至采集时的实际输出 |
| `PlanState.instrument` → `Instrumentation.nloops` | Loops 计数 | 记录已结束的执行统计循环，用于分析执行次数 |
| `PlanState.instrument` → 调用起始时间、`status` | 调用状态和本地 EOF 标志 | 判断采集时是否仍有调用及是否已读到 EOF |

GaussDB 适配层通过 `ExtoptInstrumentationView` 读取相关字段。`ntuples` 已是累计返回行数，读取时直接作为 A；`nloops` 用于辅助分析执行次数。缺少 `Instrumentation` 或没有运行记录的节点没有可用行数观测；已执行且返回零行的节点则可记录 A=0。

![图3 执行器对象关系与统计来源](patent-timeout-feedback-figures/png/fig3-executor-observation-flow.png)

*图3 `PlanState` 同时关联 `Plan` 和 `Instrumentation`。前者提供节点 ID 和估算行数，后者提供累计返回行数、Loops、调用状态和本地 EOF。*

在 `ExecutorRun` 入口保存执行计划状态树的节点指针。候选运行期间，执行器更新各节点的 `Instrumentation`。期限到达后，系统取消本轮执行，服务端在 `capture_executor_run` 处理取消错误期间，经适配函数 `partial_gaussdb_execution_capture_evidence` 读取尚未释放的节点统计。服务端返回原计划文本与节点观测，随后继续返回原查询取消错误；统计采集失败时保持原取消结果。[C2]

调用状态与 EOF 标志按以下规则计算：

```text
active = (starttime_seconds != 0 || starttime_microseconds != 0)
terminal = (Instrumentation.status != 0 && !active)
```

`active=true` 表示 `get_next` 调用正在进行；`active=false` 表示当前没有正在进行的 `get_next` 调用。`Instrumentation.status != 0` 表示节点在本地读到过 EOF；它与 `active=false` 同时成立时，`terminal=true`，表示本地已读到 EOF 且当前没有调用。计算结果保存在 `ExtoptExecutionEvidence` 中。

| 采集结果 | 表示的执行事实 | 后续处理 |
| --- | --- | --- |
| 缺少统计对象或没有执行记录 | 没有取得实际行数 | 本轮不反馈该节点 |
| `active=true` | 当前调用仍在进行 | 判断累计行数能否形成下界 |
| `terminal=true` | 本地已读到 EOF，当前没有调用 | 继续检查全部相关输出是否结束及是否还会再次执行 |
| `nloops>0` | 至少一个执行统计循环已结束 | 结合父子调用关系确定累计行数覆盖的执行次数 |
| `ntuples` 有效且非负 | 已取得累计返回行数 | 与相同执行次数和统计范围内的估算比较 |

`Instrumentation.running` 与起始时间、`status`、`nloops`、`ntuples` 可共同用于识别节点是否曾执行；是否存在正在进行的调用由上述 `active` 公式确定。

`terminal` 仅表示本地 EOF，后续是否还会再次执行以及统计覆盖范围的检查见实施例三。

适配其他执行器时，可将扫描结束计数、关闭事件及重复执行计数作为辅助状态。状态解释应区分正常读到数据流末尾、父算子提前停止取数以及取消时的资源清理；精确行数须满足实施例三所述的全部相关输出结束条件。

![图4 取消错误处理期间的统计采集时点](patent-timeout-feedback-figures/png/fig4-capture-window.png)

*图4 在执行入口保存节点对象，在取消错误处理期间、执行器清理前读取统计，随后继续返回原取消错误。*

客户端可对完全相同的候选 SQL 运行不带 `ANALYZE` 的 `EXPLAIN`，取得计划文本及 JSON 计划树。该过程只重新规划。核对原执行计划与重新生成计划的节点 ID、算子类型、查询块、估算行数及节点层次后，将超时取得的统计写入对应节点；不一致时拒绝本轮快照。也可由服务端直接返回原计划的结构化信息及节点统计。[C3]

#### 2.4.3 实施例三：节点行数的适用条件与再次执行判定

本实施例说明节点统计在什么条件下可用于基数反馈。一个待修正的扫描或连接记为目标 `k`，由查询块、关系或中间关系、谓词、连接类型、运行参数及数据可见范围确定。物理节点 ID 用于定位本轮节点，同一目标可由不同轮次的物理节点实现。

本轮目标的估算行数为 `E_i(k)`，截至采集时点 `t_i` 的已返回行数为 `A_i(k,t_i)`；若候选继续执行到该目标的全部相关输出结束，最终行数为 `C_i(k)`。先确认统计对应同一目标，并使实际行数与估算覆盖相同执行次数和并行聚合范围；无法确定目标或统一计数范围时，本轮不采用该节点的行数。

随后结合本地 EOF、父子调用关系及统计覆盖范围，判断全部相关行数是否已经返回。只有确认候选若继续执行也不会使该目标再产生输出时，才将观测视为完整行数。一次调用返回 EOF 表示本地已读到数据流末尾；Nested Loop 内侧、参数化子计划等节点仍可能在后续输入到来时再次执行。

因上层提前停止取数或取消清理而停止的节点，若不能确认目标对应结果已全部返回，其行数仍按未完成观测处理。候选正常完成时，未被全部读取的子节点也按相同条件检查。[R1][R28]

图7左侧为 Nested Loop 内侧 Join。其在第一条外侧输入下返回两行并读到本地 EOF，采集时尚未发生重复执行；下一条外侧输入仍可能使其再次执行。两行是首次执行已返回的行数，只有对应目标限定为该次执行，或父算子已无后续调用时，才能作为该目标的精确行数。

图7右侧节点每次执行估算十行，实际执行二十次且每次返回五行。累计实际行数为一百行，单次估算为十行。反馈时应比较单次实际五行与估算十行，或比较累计实际一百行与相同二十次执行的估算二百行。无法取得相同执行次数的统计时，该节点不反馈。

![图7 再次执行与累计行数比较](patent-timeout-feedback-figures/png/fig7-reentry-scope.png)

*图7 本地 EOF 后仍可能再次执行；累计实际行数与单次估算需换算为相同执行次数后比较。*

对于 Hash 的 MultiExec 结果、并行节点、参数化子计划等统计范围不明确的情形，未统一计数范围或未确认后续调用关系时，不采用相应行数反馈。

#### 2.4.4 实施例四：节点行数反馈的分类、合并与下一轮应用

本实施例说明如何将符合适用条件的节点统计分类、合并并提供给下一轮候选。对于同一目标 k，已确认全部相关输出结束时，`A_i=C_i`，发布精确行数 `EXACT(A_i)`；尚不能确认结束，但已返回量在同一范围内只会累加时，`A_i≤C_i`，且仅在 `A_i>E_i` 时发布行数下界 `LOWER_BOUND(A_i)`。未执行、统计不可比，或未确认结束且 `A_i≤E_i` 时，发布 `NONE`。

令 `Valid_i(k)` 表示目标及计数范围一致且观测可用，`Complete_i(k)` 表示已确认该目标全部相关输出结束，则分类函数为：

```text
F_i(k) = EXACT(A_i),       Valid_i(k) 且 Complete_i(k)；
         LOWER_BOUND(A_i), Valid_i(k) 且未确认 Complete_i(k)、输出只增不减且 A_i > E_i；
         NONE,              其他情况。
```

无法确定再次执行关系、参数化调用范围或并行计数的节点不反馈。

![图5 节点行数反馈的判定与合并](patent-timeout-feedback-figures/png/fig5-evidence.png)

*图5 在目标及计数范围一致的前提下判定精确行数、合格下界或无反馈。未确认全部输出结束时，A>E 才用于修正低估，A≤E 不用于确认高估。*

设目标 k 已保存的基数输入为 `B_i(k)`，`HasExact_i(k)` 表示已有有效精确行数。本轮合格反馈按以下规则更新：

```text
B_(i+1)(k) = A_i(k),                 F_i(k) = EXACT(A_i(k))；
             max(B_i(k), A_i(k)),   F_i(k) = LOWER_BOUND(A_i(k)) 且 HasExact_i(k) 为假；
             B_i(k),                F_i(k) = LOWER_BOUND(A_i(k)) 且 HasExact_i(k) 为真，或 F_i(k) = NONE。
```

精确行数允许上调或下调，更新后清除旧下界。无精确值时，多个合格下界取最大值；即使新下界大于已有精确值，也保持该有效精确值。同一轮、同一目标的多个合格精确行数按计划树遍历顺序首个生效，后续轮的合格精确行数可更新前轮精确值。采样值不覆盖已采用的精确行数或下界。[C4]

合并前要求目标语义、运行参数和数据可见范围保持一致。相关条件改变时，旧统计失效，新目标重新建立反馈。输出接口仅接受正整数时，可将原始零行观测编码为接口允许的最小值 1，同时单独保存原始 `EXACT(0)`。[C4]

例如，初始采样值为 5,000。取得合格下界 2,400 后，Hint 仍为 5,000，同时保存下界；随后下界达到 6,400，Hint 提高到 6,400；取得精确行数 7,200 后，更新为 7,200 并清除旧下界。下界是否合格由采集行数 A 与本轮对应估算 E 的比较确定，合并时再与已保存输入 B 比较。

第一轮候选 `P_1` 在超时取消时包含扫描或连接目标 R、J、T。R 已完成其唯一相关调用并读到 EOF，父算子不会再次执行它，估算为 1,000 行、实际返回 120 行，形成 `EXACT(120)`。J 尚未返回全部行数，估算为 1,000 行、已返回 2,400 行；计数可直接比较且返回量只增不减，形成 `LOWER_BOUND(2,400)`。T 尚未执行，没有实际行数观测，形成 `NONE`。

![图6 同一执行树中的三类反馈](patent-timeout-feedback-figures/png/fig6-mixed-states.png)

*图6 同一尚未完整执行的候选可同时提供精确行数、行数下界及无反馈节点。*

调优器合并上述反馈，生成下一轮候选 `P_2`。R 更新为 120；在尚无精确值时，J 取已有输入与 2,400 中的较大值；T 保持原值。若 `P_2` 正常完成，节点统计继续参与行数反馈，完整运行时间同时参与最佳候选比较；若再次超时，则在清理前取得统计并按相同规则反馈。

本轮没有合格统计时，已有基数输入保持不变，迭代控制单元依据剩余预算和停止条件决定是否继续。

#### 2.4.5 实施例五：GaussDB 环境下的调优与效果验证

在同一 GaussDB 数据库上，对 JOB 113 条 SQL 和 JOB-complex 30 条 SQL 比较无单轮超时组与 10 秒部分反馈组。两组均设置 300 秒全局调优搜索预算、`patience-iters=5`，采用相同的采样块数和优化器参数，并交替执行查询。10 秒为可配置的初始单轮上限，后续按最佳完整运行时间动态缩短。两组使用不同代码版本，该对照评价集成方案的整体效果。[E1]

| 数据集 | 查询数 | 正式交付候选数：无单轮超时 / 10 秒部分反馈 | 调优总时长（秒）：无单轮超时 / 10 秒部分反馈 | 降幅 |
| --- | ---: | ---: | ---: | ---: |
| JOB | 113 | 103 / 103 | 2,348 / 1,599 | 31.89% |
| JOB-complex | 30 | 28 / 29 | 1,002 / 583 | 41.84% |

![图8 JOB 与 JOB-complex 调优总时长对比](patent-timeout-feedback-figures/png/fig8-tuning-duration.png)

*图8 秒数取整，降幅由未舍入值计算。*

调优后，原 SQL 与可用优化 SQL 在相同设置下独立运行三次，下表采用预先指定的第 3 次完整执行时间。未正式交付候选的查询通常回填原 SQL。扩展评估另外纳入 JOB `16b`、`18c` 的恢复候选，即根据最后累积的 Rows Hint 恢复、核对后独立执行测量的候选。这两条当时未正式交付。[E2][E3][E4]

| 数据集 | 原始 SQL 总时间（秒） | 无单轮超时组总时间（秒） | 10 秒部分反馈组总时间（秒，扩展评估） | 降幅 |
| --- | ---: | ---: | ---: | ---: |
| JOB | 456 | 190 | 168 | 11.25% |
| JOB-complex | 332 | 48 | 47 | 2.26% |

![图9 JOB 与 JOB-complex 优化后总执行时间对比](patent-timeout-feedback-figures/png/fig9-execution-effect.png)

*图9 第 3 次独立完整执行时间之和。JOB 扩展评估包含两条未正式交付的恢复候选。*

仅采用正式交付候选并将未交付查询回填为原 SQL 时，JOB 10 秒部分反馈组的执行总时间约为 205 秒，无单轮超时组约为 190 秒。JOB-complex 的上述评估不包含恢复候选。调优总时长与最终候选执行时间分别评价，恢复候选的独立测量与正式交付分别记录。

JOB `16b` 的无单轮超时组运行 4 轮后触发全局预算停止条件，交付候选的完整执行时间约为 36.38 秒。10 秒组进行了 11 轮部分反馈迭代，最后累积 Rows Hint 形成的恢复候选，三次独立完整测量分别为 10.89、10.92、10.99 秒。该恢复候选当时未正式交付。[E2][E3][E4]

## 3 本发明关键技术点及有益效果

### 3.1 关键技术点

以下处理按候选限时执行、取消时保留统计、行数判定、跨轮合并及下一轮规划的顺序衔接。

1. **限时取消时保留统计。** 对同一 SQL 的各轮候选设置执行期限；超时取消时，在执行器清理前取得原计划及节点估算行数、已返回行数和执行状态。
2. **判定可反馈的行数。** 确定对应扫描或连接，结合执行状态、再次执行关系及计数范围，判断相关行数是否已全部返回，并区分精确行数、可用下界或无反馈。
3. **按反馈类型跨轮合并。** 精确行数覆盖已有值，允许上调或下调；尚无精确值时，合格下界仅抬高；已有精确值时保持精确值；无反馈时保持原输入。多个下界取最大值。
4. **修正下一轮候选。** 合并结果用于同一 SQL 的下一轮独立规划。正常完成也可提供行数反馈；完整运行时间用于比较最佳候选，并调整后续执行期限。

### 3.2 有益效果

#### 3.2.1 控制单轮试运行时间，保留有效反馈，降低整体调优耗时

候选达到执行期限时取消本轮执行，并在执行器清理前保留可用节点统计，控制慢候选对调优窗口的持续占用。满足判定条件的统计继续用于下一轮基数修正，使已发生的执行尝试仍能提供反馈。

在第 2.4.5 节对照中，JOB 调优总时长从 2,348.17 秒降至 1,599.41 秒，减少 748.76 秒，降幅为 31.89%，约为 32%；JOB-complex 从 1,001.57 秒降至 582.56 秒，减少 419.02 秒，降幅为 41.84%，约为 42%。候选执行期限之外仍包含规划、取消清理和统计处理开销。[E1][E2][E3]

#### 3.2.2 为全局预算内的后续有效迭代争取时间，增加发现更优计划的机会

当合格统计能够改变后续基数输入、且停止条件允许继续时，调优器无需等待当前慢候选完整结束，即可使用反馈生成下一轮候选。节省的等待时间可用于后续规划与试运行，增加在有限调优窗口内发现更优计划的机会。

JOB `16b` 在设置初始 10 秒单轮上限后进行了 11 轮部分反馈迭代，形成独立完整执行约 11 秒的恢复候选；无单轮超时组运行 4 轮后达到预算停止条件，交付候选完整执行约 36 秒。该例反映了后续迭代形成更快候选的过程，恢复候选在该次调优中尚未正式交付。[E2][E3][E4]

实际迭代次数由反馈变化、候选耗时和停止条件共同确定。JOB 总轮次为 569 → 528，JOB-complex 为 166 → 182。候选的最终性能通过完整执行单独评价。

#### 3.2.3 技术处理与有益效果的对应关系

| 技术处理 | 对应效果 |
| --- | --- |
| 单轮执行期限及全局预算约束 | 控制慢候选持续试运行的时间，为后续规划与试运行保留时间 |
| 清理前读取节点行数与执行状态 | 在整轮未完成时保留已经取得的有效统计 |
| 精确行数、合格下界和无反馈判定 | 按实际统计的适用条件限制基数更新方向 |
| 跨轮反馈合并 | 保留同一目标的合格统计，支持后续候选继续利用前轮信息 |
| 行数反馈与完整运行时间分别处理 | 部分反馈推动基数迭代，完整运行时间支持候选性能比较 |

### 3.3 其他实施方式

单轮初始期限和时间倍率可根据调优任务配置。采用第 2.4.1 节实施方式时，初始上限为 10 秒，已完成候选的最佳运行时间乘以 1.25 用于缩短后续上限；实际期限还受可用全局剩余预算约束。

节点统计可以通过消息传输，也可由执行器直接返回结构化计划和统计。行数反馈应用于 Rows Hint 或其他能够修正优化器基数输入的接口。不同部署方式均按照相同的行数判定和合并规则处理统计。

对于不能确定计数范围或后续调用关系的节点，本轮不反馈；对于查询条件、运行参数或数据可见范围发生变化的目标，使旧统计失效并重新建立反馈。

## 参考资料与实现依据

### 现有技术与论文

- [R1] Stillger 等，*LEO - DB2's LEarning Optimizer*，VLDB 2001，[本地论文 PDF](../sources/papers/P019.pdf)。
- [R2] Wu 等，*Sampling-Based Query Re-Optimization*，SIGMOD 2016，[本地论文 PDF](../sources/papers/1601.05748v1.pdf)。
- [R3] Chakkappen 等，*Adaptive Statistics in Oracle 12c*，PVLDB 10(12), 1813-1824, 2017，[论文](https://www.vldb.org/pvldb/vol10/p1813-zait.pdf)，DOI [10.14778/3137765.3137785](https://doi.org/10.14778/3137765.3137785)。参见 PDF 第 8-9 页，第 3.2.2、3.3.1-3.3.2 节。
- [R4] Oracle Database 19c，[Monitoring Database Operations](https://docs.oracle.com/en/database/oracle/oracle-database/19/tgsql/monitoring-database-operations.html#GUID-11748F62-BE24-4511-B77B-F28D5E443A0A)。
- [R5] Microsoft Learn，[Live Query Statistics](https://learn.microsoft.com/en-us/sql/relational-databases/performance/live-query-statistics?view=sql-server-ver17)。
- [R6] Microsoft Learn，[sys.dm_exec_query_profiles](https://learn.microsoft.com/en-us/sql/relational-databases/system-dynamic-management-objects/sys-dm-exec-query-profiles-transact-sql?view=sql-server-ver17)。
- [R7] Microsoft Learn，[Query Profiling Infrastructure](https://learn.microsoft.com/en-us/sql/relational-databases/performance/query-profiling-infrastructure?view=sql-server-ver17)。
- [R8] Oracle Database 12c，[How Adaptive Query Plans Work](https://docs.oracle.com/database/121/TGSQL/tgsql_optcncpt.htm#GUID-A42B8874-95AF-4D5B-B5D9-F0376F91D15E)。
- [R9] Oracle Database 19c，[`V$SQL_PLAN_MONITOR`](https://docs.oracle.com/en/database/oracle/oracle-database/19/refrn/V-SQL_PLAN_MONITOR.html)。
- [R14] Microsoft Learn，[Cardinality Estimation Feedback](https://learn.microsoft.com/en-us/sql/relational-databases/performance/intelligent-query-processing-cardinality-estimation-feedback?view=sql-server-ver17)。
- [R15] MariaDB，[`SHOW ANALYZE FOR`](https://mariadb.com/docs/server/reference/sql-statements/administrative-sql-statements/show/show-analyze)。
- [R16] Amazon Redshift，[`SYS_QUERY_DETAIL`](https://docs.aws.amazon.com/redshift/latest/dg/SYS_QUERY_DETAIL.html) 与 [`SYS_QUERY_HISTORY`](https://docs.aws.amazon.com/redshift/latest/dg/SYS_QUERY_HISTORY.html)。
- [R17] OceanBase Cloud，[Execution Profile](https://en.oceanbase.com/docs/common-oceanbase-cloud-10000000001725119)。
- [R18] Databricks，[Adaptive Query Execution](https://docs.databricks.com/aws/en/optimizations/aqe)。
- [R24] Markl 等，*Robust Query Processing through Progressive Optimization*，SIGMOD 2004，[IBM Research](https://research.ibm.com/publications/robust-query-processing-through-progressive-optimization)，[论文](https://dsf.berkeley.edu/cs286/papers/robust-sigmod2004.pdf)。参见 PDF 第 3 页第 2 节及第 5-6 页第 2.3 节。
- [R25] Raman 等，*Progressive Optimization in Action*，VLDB 2004，[论文](https://www.vldb.org/conf/2004/DEMP22.PDF)。参见 PDF 第 2 页图 1 及流程说明。
- [R26] Chaudhuri、Narasayya、Ramamurthy，*Estimating Progress of Execution for SQL Queries*，SIGMOD 2004，[Microsoft Research](https://www.microsoft.com/en-us/research/publication/estimating-progress-of-execution-for-sql-queries/)，[论文](https://www.microsoft.com/en-us/research/wp-content/uploads/2016/02/progress-1.pdf)。参见 PDF 第 5-6 页第 4.1-4.2 节及第 12 页 Appendix B。
- [R27] Lee 等，*Operator and Query Progress Estimation in Microsoft SQL Server Live Query Statistics*，SIGMOD 2016，[Microsoft Research](https://www.microsoft.com/en-us/research/publication/operator-and-query-progress-estimation-in-microsoft-sql-server-live-query-statistics/)，[论文](https://www.microsoft.com/en-us/research/wp-content/uploads/2016/06/qprog.pdf)。参见 PDF 第 6 页第 4.2 节及第 11 页参考文献 [7]。
- [R28] Microsoft Learn，[Logical and Physical Showplan Operator Reference](https://learn.microsoft.com/en-us/sql/relational-databases/showplan-logical-and-physical-operators-reference?view=sql-server-ver17)。参见 Physical operators、Rebinds, rewinds, and end-of-scans、How to read an execution plan。
- [R29] IBM Db2 12.1.x，[section_actuals - Section actuals configuration parameter](https://www.ibm.com/docs/en/db2/12.1.x?topic=parameters-section-actuals-section-actuals)。
- [R30] IBM Db2 11.5.x，[EXPLAIN_FROM_ACTIVITY procedure](https://www.ibm.com/docs/en/db2/11.5.x?topic=er-explain-from-activity-procedure-explain-statement-using-activity-event-monitor-information)。
- [R31] Oracle Database 12.1，[Reoptimization: Statistics Feedback](https://docs.oracle.com/database/121/TGSQL/tgsql_optcncpt.htm#GUID-739F1F0B-ABE2-44C9-9783-8FA54F4ADB12)。
- [R32] Oracle Database 12.1，[Optimizer Statistics Concepts: SQL Plan Directives](https://docs.oracle.com/database/121/TGSQL/tgsql_statscon.htm)。

### 五件重点专利

- [R19] Oracle，*Leveraging query-specific statistics in non-equivalent queries*，`US11321317B2`，[授权文本](https://patents.google.com/patent/US11321317B2/en)，[本地原件](patent-originals/US11321317B2.pdf)。参见授权 PDF 第 14-20 页的自适应与重新优化说明，以及第 31-32 页的权利要求；权利要求 1 涉及非等价后续查询的统计复用。
- [R20] IBM，*System, method, and computer program product for progressive query processing*，`US7716215B2`，[授权文本](https://patents.google.com/patent/US7716215B2/en)，[本地原件](patent-originals/US7716215B2.pdf)。参见授权 PDF 第 21-26 页、PDF 第 9 页图 7，以及第 27-28 页权利要求 1。
- [R21] Microsoft，*Query progress estimation*，`US7493337B2`，[授权文本](https://patents.google.com/patent/US7493337B2/en)，[本地原件](patent-originals/US7493337B2.pdf)。参见授权 PDF 第 20-21 页、PDF 第 13 页图 12，以及第 24-25 页权利要求 22-33、42。
- [R22] Microsoft，*System and method for cardinality estimation feedback loops in query processing*，`US11334538B2`，[授权文本](https://patents.google.com/patent/US11334538B2/en)，[本地原件](patent-originals/US11334538B2.pdf)。参见节点执行统计与反馈管理说明及权利要求 1、5。
- [R23] Sybase，*System and methodology for automatic tuning of database query optimizer*，`US9213740B2`，[授权文本](https://patents.google.com/patent/US9213740B2/en)，[本地原件](patent-originals/US9213740B2.pdf)。参见中断候选的部分耗时记录说明及权利要求 1。

上述论文页码从论文 PDF 第一页起算，授权专利页码从封面第一页起算。

### 当前实现依据

- [C2] [`capture_executor_run`](../contrib/extopt-gaussdb/extopt_partial_capture.cpp)、[`partial_gaussdb_execution_capture_evidence`](../contrib/extopt-gaussdb/extopt_partial_gaussdb.cpp)：取消时取证和执行器字段来源。
- [C3] [`partial_observation_evidence`](../contrib/hint_adviser/hint_adviser_partial_proof.c)、Rows 目标收集和计划身份核对：证据判定与目标映射。
- [C4] [`rows_hint_merge`](../contrib/hint_adviser/hint_adviser_rows_state.c)、[`feedback_loop_advance`](../contrib/hint_adviser/hint_adviser_feedback_policy.c)：跨轮合并、部分轮次和最佳候选规则。

### 实验记录

- [E1] [实验配置与版本指纹](../data/patience5-ab-20260928/run-manifest.txt)。
- [E2] [逐查询工作簿](../outputs/job_job_complex_tuning_query_summary_patience5_20260928.xlsx)及其输入明细。
- [E3] [独立执行原始记录](../data/patience5-ab-20260928/results/measurements.tsv)、调优记录和事件记录。
- [E4] [恢复候选来源明细](../data/patience5-ab-20260928/results/recovered-current-partial.tsv)及恢复规则。
- [E5] [调优时长图表的可编辑 PPT](patent-timeout-feedback-figures/pptx/fig8-tuning-duration.pptx)和[执行效果图表的可编辑 PPT](patent-timeout-feedback-figures/pptx/fig9-execution-effect.pptx)。

## English Summary

**Title: An Iterative Query Execution Plan Optimization Method Based on Partial Feedback from Time-Limited Execution**

IBM's POP papers describe checkpoint-triggered reoptimization of the current query, reuse of actual cardinalities, and cost-based selection of materialized intermediate results. Oracle's papers and manuals describe feedback and reusable directives, with separate time budgets for optimization-time statistics queries. Microsoft's progress-estimation research uses operator counts, cardinality bounds, and termination facts; the product documentation also describes scan-end counters, repeated execution, and operator closure. The present method applies qualified counts captured on candidate cancellation to the next independent tuning round through explicit qualification and merge rules.

Each candidate for the same SQL runs with an execution deadline. On timeout cancellation, the original plan and operator estimates, returned rows, invocation state, and local EOF information are captured before executor cleanup. Partial feedback refers to usable operator statistics from a candidate whose execution has not completed; it may include exact counts for qualified operators as well as lower bounds.

Feedback qualification identifies the scan or join represented by each observation and requires compatible query semantics and counting ranges. An exact count requires all relevant output to have been returned and no further output to be possible if the candidate continued. An unfinished, monotone count yields a usable lower bound only when it exceeds a comparable estimate. Other observations produce no feedback. Exact counts replace previous inputs and clear bounds. Without an exact count, lower bounds only raise inputs and merge by maximum; an existing valid exact count takes precedence over any new bound. Accepted feedback informs the next independently planned candidate. Normally completed candidates also supply qualified row feedback, while complete runtimes update the best measured candidate and adaptive deadline.

The initial cap T₀ is configurable and was ten seconds in the experiment. Once the same SQL has a best complete runtime t_best, later caps are min(T₀, 1.25 × t_best), further limited by the available global tuning budget with time reserved for cancellation and processing.

The business benefits are shorter slow-candidate trials with usable statistics retained, and more opportunity for useful subsequent iterations. Aggregate tuning time in the integrated workload comparison fell by 31.89% on JOB and 41.84% on JOB-complex. For JOB 16b, four rounds without a deadline for each candidate produced a delivered plan with a complete runtime of about 36 seconds; eleven partial-feedback rounds produced a recovered candidate independently measured at about 11 seconds. The recovered candidate was not formally delivered. Formal delivery results and extended evaluations that include recovered candidates are reported separately.
