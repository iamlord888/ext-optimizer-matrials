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

### 1.2 现有技术一：执行反馈驱动的基数修正

#### 1.2.1 技术方案

DB2 的 LEO 在计划和执行过程中积累算子计数，查询完成后保存学习信息，用于改进后续查询的优化。Oracle 12c 的 statistics feedback 在执行结束时比较计划操作的估算行数与实际行数，供后续执行重优化；dynamic statistics 则在编译期间通过统计查询采样。SQL Server 的 CE feedback 根据重复或相似查询的估算偏差形成反馈，以 Query Store hint、模型参数等方式影响后续规划。这些技术能够将计划操作的运行统计用于后续基数估计和计划选择。[R1][R3][R14]

#### 1.2.2 与限时候选调优的关系

LEO 和 Oracle statistics feedback 的上述流程在查询完成或完整执行统计可用后形成反馈。对于运行时间较长的候选，反馈取得时点可能晚于单轮试运行期限。Oracle 及 Microsoft 的相关专利还公开了执行中统计和后续查询反馈。限时候选调优需要进一步处理取消时的节点统计，确定其对应的扫描或连接、执行状态与计数范围，再用于下一轮基数修正。[R19][R22]

### 1.3 现有技术二：运行中重优化、部分执行统计与节点反馈

#### 1.3.1 IBM Progressive Optimization（US7716215B2）

IBM 的 Progressive Optimization 在计划中设置 `CHECK` 算子，比较估算基数与执行中已处理的行数。权利要求 1 根据剩余执行时间阈值和估算误差阈值决定是否暂停当前计划，随后重新优化并重新启动查询。说明书还描述：输出越过上界时，可在子流尚未结束前确认偏差；根据输出不足判断低于下界时，则需要等子流返回 EOF。该技术可收集检查点或其他计划节点的基数统计。[R20]

本方法以每轮候选的执行期限触发取消，在执行器清理前读取原计划节点的统计，并根据全部相关行数是否已经返回、是否还可能再次执行及计数范围，判定可用于下一轮独立候选的反馈类型。

#### 1.3.2 Oracle US11321317B2：运行中换计划与后续统计反馈

Oracle 的该专利描述了三类处理。Adaptive plan 在决策点根据统计收集器取得的行数、选择率等信息选择预置子计划；in-flight reoptimization 根据运行统计与编译估算的差异、剩余操作和更新后的成本，提前终止未完成计划并启动新计划；automatic reoptimization 将完整或部分执行时收集的表、连接、分组和索引统计用于相同、等价或相关的后续查询。其背景还涉及用户或 DBA 终止运行过久的计划。[R19]

本方法对同一 SQL 的第 i 轮候选设置执行期限，在取消后将合格节点行数用于第 i+1 轮独立规划。行数判定包含本地 EOF、后续再次执行的可能性以及累计行数与估算行数的比较范围，并据此形成精确行数、行数下界或无反馈。

#### 1.3.3 Microsoft US11334538B2：节点级 CE feedback

该专利描述了查询执行期间及完成时产生监测事件，并收集各计划节点的估算基数与实际基数。反馈管理器根据基数差异形成建议，影响后续相同或相似查询的执行。[R22]

本方法进一步以单轮期限取消为统计采集时点，在执行器清理前保留节点信息，并按节点统计的适用条件限制下一轮基数的更新方向。

#### 1.3.4 Microsoft US7493337B2：算子上下界与查询进度估计

该专利对当前查询的算子维护已处理行数和预计总行数的上下界，传播边界变化，修正进度估计。权利要求 27 将算子在某时点已返回的项目数设为下界；权利要求 33 涉及多个算子的上下界维护与传播；权利要求 42 允许用户中止查询。[R21]

本方法使用与估算可直接比较的已返回行数形成下界，在未取得精确行数时仅提高下一轮基数输入；下界与精确行数按不同规则跨轮合并，用于后续独立候选的规划。

### 1.4 现有技术三：取消候选评价与运行统计

#### 1.4.1 Sybase US9213740B2

该专利在计划级别学习候选性能。即使执行被中止，也可记录部分 `plancost` 和已耗时间；如果已耗时间超过当前最佳计划，可将该候选记录为应避免的计划，否则可将本次中断视为无效训练执行。其主要依据为候选计划的部分耗时和性能指标。[R23]

本方法采集取消时的节点行数和执行状态，用于下一轮基数修正；候选的部分耗时与完整运行时间分别记录，只有完整运行时间用于更新最佳候选和后续执行期限。

#### 1.4.2 数据库产品的运行观测

Oracle SQL Monitor 在取消长查询后仍可显示计划节点的 `Rows (Actual)`；`V$SQL_PLAN_MONITOR.OUTPUT_ROWS` 表示执行开始后的累计输出，可结合 `STARTS` 分析每次启动的平均输出。Microsoft Live Query Statistics 和 `sys.dm_exec_query_profiles` 提供算子级 `row_count`、`end_of_scan_count`、`rewind_count`、`rebind_count` 及估算行数。MariaDB、Redshift、OceanBase 等产品也提供执行中的计划或步骤统计。[R4][R5][R6][R9][R15][R16][R17]

上述观测可用于监控、诊断或进度分析。将其用于后续规划时，还需确定实际行数对应的执行次数，并检查扫描结束后是否仍可能再次执行。

#### 1.4.3 采样式重优化及当前查询自适应执行

Sampling-Based Query Re-Optimization 在正式执行前，对样本运行候选计划，依据连接选择率迭代重规划；采样过程可以设置时间预算，预算到达时返回已有候选中估计成本较好的计划。其估计器具有无偏和强一致性质，有限样本仍存在方差、采样成本及覆盖范围限制。[R2]

Oracle adaptive plan、Databricks AQE 等技术在当前查询执行中利用统计收集器或已完成阶段的统计选择后续子计划。限时部分反馈则从真实候选试运行取得节点统计，在本轮取消后用于下一轮独立规划。[R3][R8][R18]

### 1.5 技术方案对照

| 技术环节 | 上述现有技术的处理方式 | 本方法的处理方式 |
| --- | --- | --- |
| 触发条件 | 查询完成、计划内检查点、运行统计决策或人工中止 | 每轮候选达到配置或动态调整的执行期限时取消 |
| 统计采集 | 运行中或执行后的节点统计、候选部分耗时 | 取消时在执行器清理前保存原计划及节点统计 |
| 行数判断 | 基数偏差、检查点阈值或进度上下界 | 检查相关输出是否全部返回、是否还会再次执行及实际与估算的计数范围 |
| 统计应用 | 当前查询换计划、后续查询优化或进度展示 | 按精确行数、合格下界或无反馈，分级合并下一轮基数输入 |
| 候选评价 | 完整或部分计划性能信息 | 完整运行时间用于最佳候选比较和期限调整；部分行数用于后续规划 |

### 1.6 本发明要解决的技术问题

本发明要解决的技术问题是：在同一 SQL 的多轮候选调优中，控制单个慢候选的试运行时间；在超时取消时、执行器清理前取得节点行数与执行状态；判断其中哪些统计可作为精确行数或行数下界；将合格统计合并为下一轮基数输入，同时保持完整运行时间的候选评价。

## 2 本发明技术方案的详细阐述（发明内容）

### 2.1 技术方案的基本定义

第 i 轮候选计划记为 `P_i`，单轮执行期限记为 `T_i`，统计采集时点记为 `t_i`。正常完成时采集该轮执行统计，超时取消时在执行器清理前采集统计快照。

可反馈的基数目标记为 `k`，表示下一轮需要修正的一个扫描或连接。目标由查询块、关系或中间关系、谓词、连接类型、运行参数及数据可见范围确定；行数比较还要求实际统计与估算覆盖相同执行次数和并行聚合范围。物理节点 ID 用于定位本轮节点，同一基数目标可由不同轮次的物理节点实现。

对目标 k，本轮可直接比较的估算行数为 `E_i(k)`，截至采集时已返回行数为 `A_i(k,t_i)`。若候选继续执行到该目标的全部相关输出结束，其最终行数为 `C_i(k)`。满足目标与计数范围一致的前提时，按以下规则分类：

- 全部相关行数已经返回，且候选若继续执行也不会使该目标再次输出时，`A_i=C_i`，形成精确行数 `EXACT(A_i)`。
- 尚不能确认全部相关行数已经返回，但已返回量只会在同一统计范围内累加时，`A_i≤C_i`。若 `A_i>E_i`，形成可用于修正低估的行数下界 `LOWER_BOUND(A_i)`。
- 未执行、不能确定对应目标、计数范围无法统一，或未确认结束且 `A_i≤E_i` 时，形成 `NONE`，保持已有基数输入。

本地 EOF 表示当前调用已读到数据流结束；它与目标的全部相关输出结束分别判定。候选正常完成时，因提前停止等原因未被全部读取的子节点，也按照相同条件判断其行数能否使用。[R1]

### 2.2 主要技术处理

本方法对正常完成和超时取消两类结果分别处理：

1. **行数反馈。** 两类结果均可提供节点行数。依据执行状态、再次执行的可能性及计数范围，判定精确行数、合格下界或无反馈，合并后用于同一 SQL 的下一轮独立规划。
2. **完整运行时间。** 正常完成时记录完整运行时间，用于比较最佳候选和调整后续执行期限。超时取消时记录已耗时间，保留本轮可用行数反馈。

部分反馈描述整轮执行尚未完成时取得的统计，其中满足判定条件的已完成节点可提供精确行数，未完成节点可提供行数下界。

### 2.3 本发明的技术方案

#### 2.3.1 系统架构与应用场景

如图2所示，系统包括候选生成、限时执行、节点统计采集、行数判定、反馈合并和迭代控制单元。

候选生成单元接收原始 SQL 和已有基数反馈，调用优化器生成 `P_i`。限时执行单元设置 `T_i`，使用 `EXPLAIN ANALYZE` 试运行该候选。正常完成时，节点统计进入行数反馈，完整运行时间同时进入最佳候选比较；超时取消时，统计采集单元在执行器清理前读取原计划节点的信息，供行数反馈使用。

行数判定单元确定统计对应的扫描或连接，检查相关行数是否全部返回、是否还会再次执行，以及实际与估算的计数范围。反馈合并单元按反馈类型形成下一轮 Rows Hint 等基数输入。迭代控制单元据此继续生成候选，直到达到全局调优预算、收敛条件或其他停止条件。

上述单元可由数据库内的执行观测模块与外部调优器协作实现，也可集成于数据库服务。各轮候选分别建立执行上下文，通过合并后的节点行数反馈关联。

![图2 候选调优中行数反馈的应用位置](patent-timeout-feedback-figures/png/fig2-system.png)

*图2 第 i 轮候选正常完成与超时取消均可进入行数判定和合并，再生成第 i+1 轮候选；正常完成的完整运行时间同时用于最佳候选比较。*

#### 2.3.2 执行器对象与统计来源

以 GaussDB 普通行执行节点为例，`QueryDesc.planstate` 指向本轮顶层算子的执行状态对象 `PlanState`。沿左右子节点、Append、Subquery、InitPlan、SubPlan 等关系遍历执行计划状态树，保存节点指针。每个 `PlanState` 通过 `plan` 关联静态 `Plan`，通过 `instrument` 关联运行时 `Instrumentation`。[C2]

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

#### 2.3.3 超时统计采集与状态解释

在 `ExecutorRun` 入口保存执行计划状态树的节点指针。候选运行期间，执行器更新各节点的 `Instrumentation`。期限到达后，系统取消本轮执行，在取消错误继续传播、执行器资源尚未释放时读取统计，返回原计划文本与节点观测，然后继续返回查询取消错误。统计采集失败时保持原取消结果。[C2]

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

Nested Loop 内侧、参数化 SubPlan 等节点在一次调用返回 EOF 后，仍可能因下一条外侧输入再次执行。行数反馈需结合父算子的调用关系及统计覆盖范围，判断相应扫描或连接是否已返回全部结果。

![图4 取消错误处理期间的统计采集时点](patent-timeout-feedback-figures/png/fig4-capture-window.png)

*图4 在执行入口保存节点对象，在取消错误处理期间、执行器清理前读取统计，随后继续返回原取消错误。*

#### 2.3.4 核心方法流程

方法包括以下步骤：

1. **生成候选并设置执行期限。** 原始 SQL 与已有基数反馈共同进入优化器，生成本轮候选 `P_i`，建立独立执行上下文并设置 `T_i`。
2. **取得本轮节点统计。** 候选正常完成时收集常规执行统计和完整运行时间；超时取消时，在执行器清理前返回原计划、节点估算行数、累计返回行数、Loops 和执行状态。
3. **确定反馈目标及计数范围。** 确定统计对应的扫描或连接，检查查询块、关系、谓词、连接类型及运行参数是否与待更新目标一致；实际与估算行数按相同执行次数及并行范围比较。无法确定目标或统一计数范围时，该节点不反馈。
4. **判定精确行数。** 结合本地 EOF 及父子调用关系，检查该目标的全部相关行数是否已经返回，以及候选若继续执行是否还会使其产生输出。满足全部输出结束条件时，形成 `EXACT(A_i)`。
5. **判定行数下界。** 尚不能确认全部输出结束时，若已返回量只会在相同目标和计数范围内累加，且 `A_i>E_i`，形成 `LOWER_BOUND(A_i)`。若 `A_i≤E_i`，本轮不以该部分行数修正高估。
6. **合并节点行数反馈。** 精确行数更新已有值并清除旧下界；下界在尚无精确值时只提高已有输入，多个下界取最大值；已有精确值时保持精确值；无反馈时保持原值。
7. **更新候选评价并继续迭代。** 仅用完整运行时间更新最佳候选和后续期限；将合并后的基数输入交给同一 SQL 的下一轮独立规划，直至满足停止条件。

初始单轮上限为可配置的 `T₀`。未取得完整运行时间时使用 `T₀`；得到同一 SQL 的最佳完整运行时间 `t_best` 后，后续上限取：

```text
T_cap = min(T₀, 1.25 × t_best)
```

实际执行期限还受剩余全局调优预算约束，并为取消、统计处理和重新规划预留时间。实验中 `T₀=10 秒`；若最佳完整运行时间为 4 秒，则后续上限为 5 秒。取消候选的已耗时间不用于更新 `t_best`。

令 `Valid_i(k)` 表示目标及计数范围一致且观测可用，`Complete_i(k)` 表示已确认该目标全部相关输出结束，则分类函数为：

```text
F_i(k) = EXACT(A_i),       Valid_i(k) 且 Complete_i(k)；
         LOWER_BOUND(A_i), Valid_i(k) 且未确认 Complete_i(k)、输出只增不减且 A_i > E_i；
         NONE,              其他情况。
```

无法确定再次执行关系、参数化调用范围或并行计数的节点不反馈。

![图5 节点行数反馈的判定与合并](patent-timeout-feedback-figures/png/fig5-evidence.png)

*图5 在目标及计数范围一致的前提下判定精确行数、合格下界或无反馈。未确认全部输出结束时，A>E 才用于修正低估，A≤E 不用于确认高估。*

#### 2.3.5 节点行数反馈的判定与合并

设目标 k 已保存的基数输入为 `B_i(k)`，`HasExact_i(k)` 表示已有有效精确行数。本轮合格反馈按以下规则更新：

```text
B_(i+1)(k) = A_i(k),                 F_i(k) = EXACT(A_i(k))；
             max(B_i(k), A_i(k)),   F_i(k) = LOWER_BOUND(A_i(k)) 且 HasExact_i(k) 为假；
             B_i(k),                F_i(k) = LOWER_BOUND(A_i(k)) 且 HasExact_i(k) 为真，或 F_i(k) = NONE。
```

精确行数允许上调或下调，更新后清除旧下界。无精确值时，多个合格下界取最大值；即使新下界大于已有精确值，也保持该有效精确值。同一轮、同一目标的多个合格精确行数按计划树遍历顺序首个生效，后续轮的合格精确行数可更新前轮精确值。采样值不覆盖已采用的精确行数或下界。[C4]

合并前要求目标语义、运行参数和数据可见范围保持一致。相关条件改变时，旧统计失效，新目标重新建立反馈。输出接口仅接受正整数时，可将原始零行观测编码为接口允许的最小值 1，同时单独保存原始 `EXACT(0)`。[C4]

例如，初始采样值为 5,000。取得合格下界 2,400 后，Hint 仍为 5,000，同时保存下界；随后下界达到 6,400，Hint 提高到 6,400；取得精确行数 7,200 后，更新为 7,200 并清除旧下界。下界是否合格由采集行数 A 与本轮对应估算 E 的比较确定，合并时再与已保存输入 B 比较。

### 2.4 本发明的实施例

#### 2.4.1 同一取消时点的三类节点统计

候选计划包含扫描或连接目标 R、J、T。R 已完成其唯一相关调用并读到 EOF，父算子不会再次执行它，估算为 1,000 行、实际返回 120 行，形成 `EXACT(120)`。J 尚未返回全部行数，估算为 1,000 行、已返回 2,400 行；计数可直接比较且返回量只增不减，形成 `LOWER_BOUND(2,400)`。T 尚未执行，没有实际行数观测，形成 `NONE`。

![图6 同一执行树中的三类反馈](patent-timeout-feedback-figures/png/fig6-mixed-states.png)

*图6 同一尚未完整执行的候选可同时提供精确行数、行数下界及无反馈节点。*

#### 2.4.2 再次执行与累计行数的判断

图7左侧为 Nested Loop 内侧 Join。其在第一条外侧输入下返回两行并读到本地 EOF，采集时尚未发生重复执行；下一条外侧输入仍可能使其再次执行。两行是首次执行已返回的行数，只有对应目标限定为该次执行，或父算子已无后续调用时，才能作为该目标的精确行数。

图7右侧节点每次执行估算十行，实际执行二十次且每次返回五行。累计实际行数为一百行，单次估算为十行。反馈时应比较单次实际五行与估算十行，或比较累计实际一百行与相同二十次执行的估算二百行。无法取得相同执行次数的统计时，该节点不反馈。

![图7 再次执行与累计行数比较](patent-timeout-feedback-figures/png/fig7-reentry-scope.png)

*图7 本地 EOF 后仍可能再次执行；累计实际行数与单次估算需换算为相同执行次数后比较。*

#### 2.4.3 多轮候选迭代

第一轮候选 `P_1` 超时取消，得到 R 的 `EXACT(120)`、J 的 `LOWER_BOUND(2,400)` 及 T 的 `NONE`。下一轮中，R 更新为 120；在尚无精确值时，J 取已有输入与 2,400 中的较大值；T 保持原值。若 `P_2` 正常完成，节点统计继续参与行数反馈，完整运行时间同时参与最佳候选比较；若再次超时，则在清理前取得统计并按相同规则反馈。

本轮没有合格统计时，已有基数输入保持不变，迭代控制单元依据剩余预算和停止条件决定是否继续。

#### 2.4.4 GaussDB 节点统计采集实施例

服务端通过 `capture_executor_run` 在查询取消错误处理中输出统计快照，然后重新返回原取消错误。`partial_gaussdb_execution_capture_evidence` 从 `PlanState.instrument` 读取起始时间、`status`、`running`、`ntuples` 和 `nloops`，按第 2.3.3 节公式计算 `active` 和 `terminal`，记录累计返回行数和 Loops。[C2]

客户端可对完全相同的候选 SQL 运行不带 `ANALYZE` 的 `EXPLAIN`，取得计划文本及 JSON 计划树。该过程只重新规划。核对原执行计划与重新生成计划的节点 ID、算子类型、查询块、估算行数及节点层次后，将超时取得的统计写入对应节点；不一致时拒绝本轮快照。也可由服务端直接返回原计划的结构化信息及节点统计。[C3]

上述字段用于提供节点的局部调用状态和累计计数。精确行数与行数下界的应用条件按第 2.3.4 节确定。对于 Hash 的 MultiExec 结果、并行节点、参数化子计划等统计范围不明确的情形，未统一计数范围或未确认后续调用关系时，不采用相应行数反馈。

#### 2.4.5 JOB 与 JOB-complex 性能对照

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

1. **限时取消时保留统计。** 对同一 SQL 的各轮候选设置执行期限；超时取消时，在执行器清理前取得原计划及节点估算行数、已返回行数和执行状态。
2. **判定可反馈的行数。** 确定对应扫描或连接，检查执行状态、是否还会再次执行以及实际与估算的计数范围，区分精确行数、可用下界或无反馈。
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

单轮初始期限和时间倍率可根据调优任务配置。采用第 2.3.4 节实施方式时，初始上限为 10 秒，已完成候选的最佳运行时间乘以 1.25 用于缩短后续上限；实际期限还受可用全局剩余预算约束。

节点统计可以通过消息传输，也可由执行器直接返回结构化计划和统计。行数反馈应用于 Rows Hint 或其他能够修正优化器基数输入的接口。不同部署方式均按照相同的行数判定和合并规则处理统计。

对于不能确定计数范围或后续调用关系的节点，本轮不反馈；对于查询条件、运行参数或数据可见范围发生变化的目标，使旧统计失效并重新建立反馈。

## 参考资料与实现依据

### 现有技术与论文

- [R1] Stillger 等，*LEO - DB2's LEarning Optimizer*，VLDB 2001，[本地论文 PDF](../sources/papers/P019.pdf)。
- [R2] Wu 等，*Sampling-Based Query Re-Optimization*，SIGMOD 2016，[本地论文 PDF](../sources/papers/1601.05748v1.pdf)。
- [R3] Zait 等，*Adaptive Statistics in Oracle 12c*，PVLDB 10(12), 2017，[论文](https://vldb.org/pvldb/vol10/p1813-zait.pdf)。
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

### 五件重点专利

- [R19] Oracle，*Leveraging query-specific statistics in non-equivalent queries*，`US11321317B2`；较早公开申请 `US20140095475A1`，[授权文本](https://patents.google.com/patent/US11321317B2/en)，[本地原件](patent-originals/US11321317B2.pdf)。参见说明书 `[0009]`、`[0037]`、`[0083]`、`[0084]`、`[0087]`、`[0105]`–`[0107]` 及权利要求 1。
- [R20] IBM，*System, method, and computer program product for progressive query processing*，`US7716215B2`；较早公开申请 `US20050097078A1`，[授权文本](https://patents.google.com/patent/US7716215B2/en)，[本地原件](patent-originals/US7716215B2.pdf)。参见说明书 `p-0040`、`p-0065`、`p-0088` 及权利要求 1。
- [R21] Microsoft，*Query progress estimation*，`US7493337B2`；较早公开申请 `US20050222965A1`，[授权文本](https://patents.google.com/patent/US7493337B2/en)，[本地原件](patent-originals/US7493337B2.pdf)。参见权利要求 22、23、27、33、42 及说明书 `p-0089`、`p-0091`。
- [R22] Microsoft，*System and method for cardinality estimation feedback loops in query processing*，`US11334538B2`；较早公开申请 `US20200379963A1`，[授权文本](https://patents.google.com/patent/US11334538B2/en)，[本地原件](patent-originals/US11334538B2.pdf)。参见说明书 `[0024]`、`[0026]`、`[0028]` 及权利要求 1、5。
- [R23] Sybase，*System and methodology for automatic tuning of database query optimizer*，`US9213740B2`；较早公开申请 `US20090100004A1`，[授权文本](https://patents.google.com/patent/US9213740B2/en)，[本地原件](patent-originals/US9213740B2.pdf)。参见说明书 `[0130]`、`[0194]` 及权利要求 1。

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

Each candidate for the same SQL runs with an execution deadline. On timeout cancellation, the original plan and operator estimates, returned rows, invocation state, and local EOF information are captured before executor cleanup. Partial feedback refers to usable operator statistics from a candidate whose execution has not completed; it may include exact counts for qualified operators as well as lower bounds.

Feedback qualification identifies the scan or join represented by each observation and requires compatible query semantics and counting ranges. An exact count requires all relevant output to have been returned and no further output to be possible if the candidate continued. An unfinished, monotone count yields a usable lower bound only when it exceeds a comparable estimate. Other observations produce no feedback. Exact counts replace previous inputs and clear bounds. Without an exact count, lower bounds only raise inputs and merge by maximum; an existing valid exact count takes precedence over any new bound. Accepted feedback informs the next independently planned candidate. Normally completed candidates also supply qualified row feedback, while complete runtimes update the best measured candidate and adaptive deadline.

The initial cap T₀ is configurable and was ten seconds in the experiment. Once the same SQL has a best complete runtime t_best, later caps are min(T₀, 1.25 × t_best), further limited by the available global tuning budget with time reserved for cancellation and processing.

The business benefits are shorter slow-candidate trials with usable statistics retained, and more opportunity for useful subsequent iterations. Aggregate tuning time in the integrated workload comparison fell by 31.89% on JOB and 41.84% on JOB-complex. For JOB 16b, four rounds without a deadline for each candidate produced a delivered plan with a complete runtime of about 36 seconds; eleven partial-feedback rounds produced a recovered candidate independently measured at about 11 seconds. The recovered candidate was not formally delivered. Formal delivery results and extended evaluations that include recovered candidates are reported separately.
