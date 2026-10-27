# 一种基于限时执行部分反馈的查询执行计划迭代优化方法

## 缩略语

| 英文缩略语 | 英文表述 | 中文术语 |
| --- | --- | --- |
| SQL | Structured Query Language | 结构化查询语言 |
| QEP | Query Execution Plan | 查询执行计划 |
| E-rows | Estimated Rows | 执行计划节点的估算输出行数 |
| A-rows | Actual Rows | 执行计划节点的实际输出行数；在取消时的统计快照中指截至采集时的累计返回行数 |
| EOF | End of File / end-of-stream signal | 当前调用已读到数据流结束的信号 |
| LEO | DB2's LEarning Optimizer | DB2学习型优化器 |
| CE | Cardinality Estimation | 基数估计 |

## 摘要

本发明公开一种基于限时执行部分反馈的查询执行计划迭代优化方法。调优工具为同一SQL的每轮候选计划设置期限。超时取消时，在执行器清理前保存本轮实际执行的计划及节点行数和执行状态。根据节点是否已返回全部相关行数、是否还会再次执行，以及实际行数与估算行数能否直接比较，判定采用精确行数、可用下界或不生成反馈。对仍然适用的统计，精确行数替换已有值；尚无精确值时，下界只用于提高该值；无反馈时保持原值。合并结果用于下一轮规划。正常完成的计划也可提供行数反馈，其完整运行时间用于选择最佳计划和调整后续期限。该方法在控制单轮运行时间的同时，保留有效节点统计，为全局预算内的后续迭代提供依据。

## 1 相关技术背景以及最接近的现有技术

### 1.1 背景技术

数据库运维和自动调优平台通常需要在限定时间内，为一批慢SQL寻找运行更快的执行计划。基于执行反馈的调优工具逐轮生成并试运行候选计划，根据节点实际返回的行数修正后续规划输入，并通过完整运行时间比较候选性能。在顺序试运行的调优过程中，某个计划长时间未结束，会推迟该SQL后续候选的试运行。直接取消虽然能够结束等待，却可能中断`EXPLAIN ANALYZE`返回执行统计的过程；如果未提前保存，本轮已经取得的节点行数就无法用于后续调优。

优化器根据扫描、过滤、连接等操作的估算输出行数，选择访问路径、连接顺序和连接方法。节点行数的估算误差可能影响上层节点的估算和计划选择。例如，连接结果被估算为几十行时，优化器可能将其作为嵌套循环连接（Nested Loop）的外侧输入；实际结果若达到数万行，内侧访问就会被反复调用，后续扫描、连接和排序的工作量也随之增加。执行反馈调优通过比较估算行数与实际行数，修正后续规划使用的基数值。

同一SQL的不同轮次可以使用Rows Hint、Outline等不同规划输入，各轮分别创建物理执行计划和执行上下文。计划被取消时，虽然无法取得完整运行时间，此前产生的部分节点统计仍可能用于后续规划。

例如，在JOB `16b`的无单轮超时对照中，调优工具运行4轮后触发全局预算停止条件，结束该SQL的调优。所交付计划的完整执行时间约为36秒。该记录反映了慢候选计划对有限调优时间的占用，实验条件及结果见第3.3节。[E1][E2]

从取消时取得的节点统计中生成反馈前，还需要核对行数的含义。有的节点已返回全部相关行数，有的节点只返回了一部分，有的节点尚未执行。`Instrumentation`中的`ntuples`字段记录累计返回行数，该值不一定能与单次执行的估算直接比较。节点本次调用读到EOF，也不表示父算子之后不会再次调用它。因此，调优工具既要保存已有统计，也要判断这些统计能够作为精确行数、行数下界，还是不应使用。

本说明书所称“部分反馈”，是指本轮候选计划尚未执行完毕时取得、且满足适用条件的节点统计。其中，已确认全部相关行数均已返回的节点可以提供精确行数；尚未返回全部结果，但已返回行数与估算行数能够在相同执行次数和汇总范围内比较、且满足本方案反馈条件的节点，可以提供行数下界。采集到部分统计并不表示这些统计均可用于修正基数。

![图1慢候选的执行期限与反馈时点](patent-timeout-feedback-figures/png/fig1-conventional.png)

*图1展示持续等待、直接取消，以及取消时保存统计三种处理方式。候选计划在试运行前已经生成，取消前取得的可用节点行数可用于下一轮规划。*

### 1.2 现有相关技术一：IBM的执行反馈与渐进式查询优化

#### 1.2.1 现有技术方案

IBM的执行反馈研究包括两类处理：根据此前的执行结果改进后续查询，以及在当前查询执行期间重新优化计划。DB2的学习型优化器LEO在计划生成和执行过程中记录算子计数，查询完成后保存学习信息，用于改进后续查询的基数估计和计划选择。[R1]

渐进式查询处理专利US7716215B2在执行计划中设置`CHECK`算子，将估算基数与执行中已处理的行数进行比较。权利要求1根据剩余执行时间阈值和估算误差阈值，决定是否暂停当前计划、重新优化并执行新计划。说明书还描述：如果已输出行数超过上界，可以在子数据流结束前确认偏差；如果要根据输出不足判断行数低于下界，则需要等子数据流返回EOF。该方案还可收集检查点或其他计划节点的基数统计。[R20]

《Robust Query Processing through Progressive Optimization》详细介绍了上述渐进式查询优化（POP）方法，《Progressive Optimization in Action》展示了其Db2研究原型。CHECK触发重新优化后，系统将已形成的中间结果作为临时物化视图或物化查询表提供给优化器，同时提供实际基数。优化器比较“复用已有结果”和“重新计算结果”两类计划，按成本选择。复用时可以继续使用内存中已有的数据，不要求将全部中间结果写入磁盘。重新优化和新计划的执行都在同一事务中进行，以完成当前SQL。[R24][R25]

Db2产品也提供运行统计采集接口。通过配置参数`section_actuals`启用采集后，可以记录基本算子基数计数；记录相应执行活动后，调用`EXPLAIN_FROM_ACTIVITY`过程，查看活动事件监控器保存的计划及统计。该过程的使用说明指出，通过`WLM_CAPTURE_ACTIVITY_IN_PROGRESS`取得的运行中活动记录不包含section actuals。[R29][R30]

#### 1.2.2 与本发明的关系

IBM POP由计划中的CHECK条件触发重新优化。优化器既利用实际基数，也可按成本选择复用已经物化的中间结果。本发明达到单轮执行期限时取消慢候选计划，并在执行器清理前保存节点行数和执行状态，供后续规划使用。[R20][R24][R25]

### 1.3 现有相关技术二：Oracle的执行中重新优化与执行信息复用

#### 1.3.1 现有技术方案

Oracle专利US11321317B2将执行统计用于当前查询的计划调整，以及后续查询的规划优化。在当前查询执行期间，自适应计划（Adaptive Plan）根据统计收集器取得的行数、选择率等信息，选择预先准备的子计划。执行中重新优化（In-Flight Reoptimization）则根据实际统计与编译期估算的差异，结合剩余操作和重新计算的成本，提前终止未完成计划并启动新计划。专利背景还讨论了用户或DBA终止运行过久的计划的情况。[R19]

自动重新优化（Automatic Reoptimization）可以利用首次完整或部分执行取得的表、连接、分组和索引统计，改进同一或等价查询的后续执行。该专利的授权权利要求1还涉及选择保存某个操作的统计，根据数据库对象和操作判断其是否适用于后续非等价查询，再生成第二执行计划。[R19]

Oracle产品手册对相关功能也有说明。自适应查询计划（Adaptive Query Plans）在当前执行期间收集行数，并选择预置子计划。统计反馈（Statistics Feedback）在执行结束时比较各操作的估算行数与实际行数，随后使用修正值改进规划。SQL计划指令（SQL Plan Directives）按查询表达式保存可复用信息，使不同SQL能够使用这些信息。[R8][R31][R32]

2017年论文《Adaptive Statistics in Oracle 12c》进一步介绍了自适应统计和计划指令的生成、缓存及使用。第3.2.2节说明，系统根据历史运行信息，为优化期间执行的统计查询分配时间预算；到达期限后，停止该统计查询，并使用已经取得的部分结果。这里受限时控制的是用于估算基数的统计查询，Statistics Feedback手册所述流程则在主查询执行结束后处理节点统计。[R3][R31]

Oracle SQL Monitor还可以在长查询被取消后显示计划节点的`Rows (Actual)`。其中，`V$SQL_PLAN_MONITOR`视图的`OUTPUT_ROWS`字段表示执行开始后的累计输出行数，结合`STARTS`字段可以分析每次启动的平均输出行数。[R4][R9]

#### 1.3.2 与本发明的关系

Oracle的相关技术一方面根据运行统计，对单条查询尚未完成的计划进行重新优化；另一方面保存完整或部分执行取得的信息，供后续相同、等价或相关查询规划使用。本发明将同一SQL的调优组织为全局预算内的多轮独立候选试运行，对各轮设置执行期限，并将取消时取得的可用节点行数经判定和合并后用于后续规划。完整运行时间另行用于比较候选性能。[R19][R31][R32]

### 1.4 现有相关技术三：Microsoft的查询进度估计

#### 1.4.1 现有技术方案

Microsoft专利US7493337B2关注查询执行进度的估计。其方法分别记录各算子已经返回的行数，维护各算子最终输出行数的上下界，并传播边界变化，以修正进度估计。权利要求27将某时点已返回的数据项数量设为下界；权利要求33涉及多个算子的上下界维护与传播；权利要求42涉及显示查询进度并允许用户启动或中止查询的系统。[R21]

相关论文《Estimating Progress of Execution for SQL Queries》以查询整体进度估计为目标，根据算子输出计数和基数上下界，修正预计总工作量。论文说明，算子结束时，其输出行数的上下界精确已知且相等。2016年的SQL Server Live Query Statistics产品论文明确采用了这一早期研究的基数边界技术，并同时估计查询整体和单个算子的进度。[R26][R27]

SQL Server提供系统动态管理视图`sys.dm_exec_query_profiles`，按算子和执行线程返回累计行数、估算行数、扫描结束次数、重复执行计数及关闭时间。实时查询统计功能（Live Query Statistics）使用这些统计信息显示查询和算子的执行进度。[R5][R6][R27]

| SQL Server字段 | 字段含义 |
| --- | --- |
| `row_count`、`estimate_row_count` | 算子截至当前已返回的行数及其估算行数 |
| `end_of_scan_count` | 截至当前发生的扫描结束次数 |
| `rebind_count` | 节点重新绑定的次数；相关参数变化时，需要重新计算 |
| `rewind_count` | 节点再次从头读取的次数；参数未变时，可能复用已有结果 |
| `close_time` | 算子关闭时的时间戳 |
| `thread_id` | 同一算子中不同执行线程的标识 |

SQL Server的算子手册说明，物理算子通过`Open()`初始化，通过`GetRow()`取得数据，并通过`Close()`清理和关闭。算子到达数据流末尾时，`ActualEndOfScans`增加；Nested Loop内侧算子可能多次到达末尾。每个物理算子只接收一次`Close()`调用，但不保证一定发生该调用。手册还以Top为例，说明父算子取得足够行数后可以停止向子节点取数，因此子扫描节点不一定读完整个表。[R28]

#### 1.4.2 与本发明的关系

Microsoft将算子已返回行数、预计总行数及其上下界用于查询进度估计，帮助用户判断当前查询已经完成多少工作，以及应继续执行还是中止。本发明将满足条件的节点行数用于修正下一轮候选的基数输入，使优化器能够重新选择访问路径、连接顺序或连接方法。[R21][R26][R27]

### 1.5 补充相关技术与综合对照

#### 1.5.1 其他相关技术

Sybase专利US9213740B2在计划级别学习候选性能。即使执行被中止，系统仍可记录部分`plancost`和已耗时间。如果已耗时间超过当前最佳计划的运行时间，可将该候选记录为应避免的计划；否则，可以将本次中断视为无效训练执行。该方案主要依据计划的部分耗时和性能指标进行判断。[R23]

Sampling-Based Query Re-Optimization在正式执行前，对样本运行候选计划，根据连接选择率迭代重新规划。采样过程可以设置时间预算；到达预算时，返回已有候选中估计成本较好的计划。其估计器具有无偏和强一致性质，但有限样本仍存在方差、采样成本及覆盖范围限制。[R2]

Databricks AQE在查询执行期间，根据已完成阶段的统计选择后续执行方式。MariaDB、Redshift、OceanBase等产品也提供执行中的计划或步骤统计，用于监控、诊断或进度分析。[R18][R15][R16][R17]

#### 1.5.2 综合对照

本发明对候选计划进行限时试运行，在超时取消、执行器清理前保存节点统计，判定并合并可用行数，再将合并结果用于下一轮独立规划。正常完成时取得的完整运行时间，另行用于比较各计划的性能。

| 相关技术 | 主要处理方式 | 本发明对应的处理 |
| --- | --- | --- |
| IBM | 检查点触发当前查询重优化，利用实际基数并可选复用物化结果 | 外部调优工具控制各轮试运行期限，将保存的节点统计用于后续规划 |
| Oracle | 对当前查询执行中的计划进行重新优化，或保存执行信息供后续查询使用 | 将同一SQL的调优组织为预算内的多轮候选试运行 |
| Microsoft | 根据节点输出行数及其上下界，估计当前查询和算子的执行进度 | 将可用节点行数交给优化器，生成下一轮候选计划 |
| 其他相关技术 | 利用部分耗时评价计划，或对样本重规划及显示运行统计 | 使用候选计划实际执行时产生的节点统计，继续进行限时迭代 |

### 1.6 本发明要解决的技术问题

本发明要解决的技术问题是：在同一SQL的多轮调优中，限制单个慢候选计划的试运行时间；在超时取消、执行器清理前取得节点行数和执行状态；判断哪些行数能够作为精确值或下界使用；将可用行数合并为下一轮规划的基数输入，同时继续使用完整运行时间评价已完成计划的性能。

## 2 本发明技术方案的详细阐述（发明内容）

### 2.1 本发明要解决的技术问题

本发明要解决的技术问题是：在全局调优预算有限的情况下，某些候选计划长时间运行，会挤占后续迭代的时间；直接取消又可能导致节点执行统计无法正常返回。同时，取消时取得的行数可能来自尚未结束的执行，也可能是多次调用的累计结果，不能全部视为最终基数。因此，需要在限制单轮执行时间的同时，保存已有节点统计，判断其中哪些行数可用，并将可用行数用于后续规划。

### 2.2 本发明的主要发明要点概述

本发明为同一SQL的每轮候选计划设置执行期限。计划超时取消时，调优工具在执行器清理前取得本轮实际执行的计划，以及各节点的返回行数和执行状态。调优工具核对行数对应的扫描或连接、执行次数和统计范围：确认全部相关行数已返回时采用精确行数；尚未全部返回，但可比较的已返回行数超过本轮估算、且只会随执行继续累加时，采用行数下界；其他情况不生成反馈。对含义和统计范围一致、且统计仍然适用的反馈目标，精确行数替换已保存的值；尚无精确值时，下界仅用于提高该值；没有可用反馈时保持原值。合并结果用于下一轮独立规划。正常完成的计划也可提供行数反馈，完整运行时间用于选择最佳计划和调整后续执行期限。

### 2.3 本发明应用的系统架构或场景

如图2所示，系统包括候选生成、限时执行、节点统计采集、行数判定、反馈合并和迭代控制六个单元。

在本实施方式中，节点统计采集单元在数据库执行器中读取本轮节点对象，并在取消清理前保存统计。调优工具接收这些统计，组织行数判定、反馈合并以及后续候选的生成与试运行。

候选生成单元接收原始SQL和已保存的基数反馈，调用优化器生成第i轮候选计划`P_i`。限时执行单元为该轮设置期限`T_i`，并通过`EXPLAIN ANALYZE`试运行计划。计划正常完成时，节点统计交由行数判定单元处理，完整运行时间用于比较已完成计划的性能。计划超时取消时，统计采集单元在执行器清理前读取节点行数和执行状态，再交由行数判定单元处理。

行数判定单元确认统计对应的扫描或连接，检查行数是否已全部返回、节点是否还会再次执行，以及实际行数与估算行数能否直接比较。反馈合并单元根据判定结果，更新调优工具保存的行数值，并生成下一轮使用的Rows Hint等规划输入。迭代控制单元据此生成新计划，直到触发全局预算、收敛或其他停止条件。

系统可以由数据库内的执行统计模块与外部调优工具共同实现，也可以集成在数据库服务中。每轮分别创建执行上下文，前轮取得的统计通过下一轮的规划输入继续发挥作用。

![图2候选调优中行数反馈的应用位置](patent-timeout-feedback-figures/png/fig2-system.png)

*图2第i轮计划正常完成或超时取消后，都可以提供节点行数。调优工具判定并合并这些行数，再生成第i+1轮计划；完整运行时间仅由正常完成的计划提供。*

### 2.4 本发明技术实现的具体实施例

以下四个实施例依次说明单轮执行期限、节点统计采集、行数适用条件，以及反馈的分类与合并。

#### 2.4.1 实施例一：同一SQL的限时试运行与期限调整

本实施例说明如何控制每轮试运行的时间，并根据已有反馈继续生成计划。设第i轮候选计划为`P_i`，该轮的实际执行期限为`T_i`。调优工具将原始SQL和当前Rows Hint等规划输入交给优化器，取得候选计划，并为该轮创建独立执行上下文。

调优工具通过`EXPLAIN ANALYZE`执行候选计划。计划正常完成时，收集节点统计和完整运行时间：节点统计继续用于行数反馈，完整运行时间用于比较各计划的性能。达到期限时，取消本轮执行，并按实施例二在执行器清理前取得节点统计。该轮已经消耗的时间单独记录，其中满足条件的行数仍可用于后续规划。

初始单轮执行上限为可配置的`T₀`。尚未在反馈迭代中取得正常完成并保留的候选计划时，使用`T₀`；取得这些候选中的最佳完整运行时间`t_best`后，后续上限按下式计算：

```text
T_cap = min(T₀, 1.25 × t_best)
```

实际执行期限还受剩余全局调优预算约束，并为取消、统计处理和重新规划预留时间。实验中`T₀=10秒`；如果最佳完整运行时间为4秒，则后续执行上限为5秒。取消时已耗的时间不作为完整运行时间，也不用于更新`t_best`。

调优工具按照实施例三、四判定并合并本轮统计，再调用优化器生成下一轮计划。各轮分别创建执行上下文，前轮统计通过基数输入影响后轮规划。达到全局预算、收敛或其他停止条件时，结束迭代。在其他实施方式中，单轮初始期限和时间倍率可以根据调优任务配置。

#### 2.4.2 实施例二：超时取消时保存节点行数与执行状态

本实施例以GaussDB的普通行执行节点为例，说明如何在超时取消时保存节点统计。数据库在本轮执行开始时，从`QueryDesc`的`planstate`字段取得最上层算子的执行状态对象`PlanState`，再沿子节点关系遍历执行计划状态树，保存各算子的`PlanState`指针，供取消时读取。

每个`PlanState`中的`plan`指针指向该算子的计划信息对象`Plan`，其中`plan_node_id`保存节点ID，`plan_rows`保存估算行数E。`instrument`指针指向执行统计对象`Instrumentation`，其中`ntuples`保存截至采集时的累计返回行数A，`nloops`记录已结束的执行循环数，用于辅助判断统计涉及的执行次数。由此，可以取得同一计划节点的估算行数与实际返回行数。[C2]

计划运行期间，执行器持续更新这些统计。发生超时取消时，统计采集单元在执行器清理、节点对象被释放之前，根据预先保存的节点指针读取各节点的统计，将累计返回行数、执行循环计数、调用状态和本地EOF标志，与本轮实际执行的计划一并保存并返回调优工具。随后继续原有查询取消处理，无需等待候选计划完整执行。

调用状态根据执行统计中的调用起始时间判断。起始时间的秒、微秒分量至少有一个非零时，记为`active=true`，表示仍有取下一行数据的调用（`get_next`）正在进行；两者均为零时，记为`active=false`。`Instrumentation`的`status`字段非零，表示该节点在本地读到过EOF；该字段非零且`active=false`时，记为`terminal=true`，表示本地已读到EOF，且当前没有取数调用正在进行。本地EOF并不保证该节点以后不会再次执行，是否已返回全部相关行数，按实施例三进一步判断。[C2]

没有执行统计对象或没有执行记录的节点，本轮不提供实际行数；已经执行但没有输出的节点，可以记录A=0。例如，某扫描节点估算为1000行，超时取消时已返回2400行，且取数调用尚未结束，则保存该节点的E=1000、A=2400和`active=true`。2400尚不能作为最终行数，后续在满足实施例三的条件时，可以作为行数下界使用。

调优工具根据保存的计划与节点统计，按照实施例三、四判定并合并可用行数，再用于下一轮规划。正常完成的计划也可采用相同的行数判定和反馈合并过程。

![图3执行器对象关系与统计来源](patent-timeout-feedback-figures/png/fig3-executor-observation-flow.png)

*图3以GaussDB为例，说明同一计划节点的估算行数和执行统计分别保存在何处。*

![图4取消错误处理期间的统计采集时点](patent-timeout-feedback-figures/png/fig4-capture-window.png)

*图4统计采集发生在超时取消处理过程中、执行器清理之前，保存统计后继续原有取消流程。*

#### 2.4.3 实施例三：节点行数的适用条件与再次执行判定

本实施例说明哪些节点行数可以用于基数反馈。将下一轮需要修正行数估算的一个扫描结果或连接结果称为反馈目标，记为`k`。确定目标时，需要同时考虑查询块、关系或中间关系、谓词、连接类型、运行参数和数据可见范围。节点ID用于找到本轮的物理节点；不同轮次可以使用不同物理节点来计算同一反馈目标。

对目标k，先确认实际行数与估算行数对应相同的执行次数和并行汇总范围。两者可以比较时，将本轮估算记为`E_i(k)`，截至采集时点`t_i`的已返回行数记为`A_i(k,t_i)`。如果计划继续执行到该目标在限定范围内的全部调用结束，最终返回行数记为`C_i(k)`。无法确定目标或统一计数范围时，本轮不使用该节点的行数。

调优工具根据本地EOF、父子调用关系和统计覆盖范围，判断该目标的全部相关行数是否已经返回。只有确认计划若继续执行，与该目标对应的算子也不会再产生输出时，才能将已有行数作为精确值。一次调用返回EOF只能说明本次已读到数据流末尾，Nested Loop内侧算子或参数化子计划仍可能在后续输入到来时再次执行。对可能随外侧输入或相关参数变化而再次执行的节点，调优工具继续检查驱动它的父算子是否还会提供输入；无法排除后续相关调用时，不采用精确行数。此前尚未发生重复执行，不能代替这项判断。

父算子达到Limit等要求而提前停止取数，或者取消清理导致节点停止时，如果不能确认该目标的结果已全部返回，就仍将其作为未完成统计处理。即使整条查询正常完成，未被全部读取的子节点也需要按相同条件检查。只有实际行数与估算行数能够比较、且已返回量仍是同一目标最终行数的下界时，才继续判断是否采用下界；不能确认这些条件时，本轮不使用该统计。[R1][R28]

图7左侧以Nested Loop的内侧Join节点为例。该节点处理第一条外侧输入时返回两行，然后读到本地EOF；采集时还没有发生重复执行，但下一条外侧输入仍可能使它再次运行。只有当反馈目标限定为这一次调用，或者已经确认父算子不会再调用它时，才能将这两行作为该目标的精确行数。

图7右侧的节点每次执行估算十行，实际执行二十次，每次返回五行。累计实际行数是一百行，不能直接与单次估算十行比较。应比较单次实际五行与估算十行，或比较累计实际一百行与二十次执行的估算二百行。如果无法取得相同执行次数下的统计，本轮不反馈该节点的行数。

![图7再次执行与累计行数比较](patent-timeout-feedback-figures/png/fig7-reentry-scope.png)

*图7左侧说明读到本地EOF后仍可能再次执行；右侧说明实际行数与估算行数必须对应相同执行次数，才能进行比较。*

对于通过`MultiExec`接口提供结果的Hash节点、并行节点和参数化子计划，如果无法明确统计范围或后续调用关系，也不采用相应行数反馈。

#### 2.4.4 实施例四：节点行数反馈的分类、合并与下一轮应用

本实施例说明如何将可用的节点行数分类、合并，再用于下一轮候选计划。设调优工具已为反馈目标k保存的规划输入值为`B_i(k)`。本轮计划的估算行数记为`E_i(k)`，采集时已返回行数记为`A_i`。调优工具将合并后的值写入Rows Hint，供下一轮规划使用，也可以使用其他能够修正基数的优化器接口。E与B分别表示实际计划中的估算和调优工具保存的输入，判断反馈类型时比较A与E，合并反馈时比较新反馈与B。

对同一目标k，确认全部相关行数已返回时，`A_i=C_i`，记录精确行数`EXACT(A_i)`；尚不能确认结果已全部返回，但已返回量在同一计数范围内只会累加时，`A_i≤C_i`，且只有在`A_i>E_i`时记录行数下界`LOWER_BOUND(A_i)`。节点未执行、统计无法比较，或者未确认结束且`A_i≤E_i`时，不生成行数反馈，记为`NONE`。

令`Valid_i(k)`表示本轮统计与反馈目标对应，且行数可以在相同计数范围内比较；`Complete_i(k)`表示已确认该目标的全部相关行数已返回。分类函数如下：

```text
F_i(k) = EXACT(A_i),       Valid_i(k) 且 Complete_i(k)；
         LOWER_BOUND(A_i), Valid_i(k) 且未确认 Complete_i(k)、输出只增不减且 A_i > E_i；
         NONE,              其他情况。
```

无法确认统计对应的反馈目标，或无法统一参数化调用范围、执行次数及并行计数时，分类结果为`NONE`。

![图5节点行数反馈的判定与合并](patent-timeout-feedback-figures/png/fig5-evidence.png)

*图5先确认统计对应同一目标和计数范围，再判断行数类型。未确认全部行数已返回时，A>E才能用于修正低估，A≤E不能用于确认高估。*

跨轮合并仅针对含义、运行参数、数据可见范围和统计范围一致的反馈目标，且此前保存的统计仍然适用。相关条件发生变化时，不再使用原统计，并为变化后的目标重新收集反馈。若同一目标出现精确行数低于已有有效下界，或新下界高于仍适用的精确行数，则相应统计不满足本实施例的合并条件，在重新确认目标与统计范围前不作为本轮合并输入。

`HasExact_i(k)`表示是否已经保存了适用的精确行数。通过上述适用条件核对后，本轮按下式更新：

```text
B_(i+1)(k) = A_i(k),                 F_i(k) = EXACT(A_i(k))；
             max(B_i(k), A_i(k)),   F_i(k) = LOWER_BOUND(A_i(k)) 且 HasExact_i(k) 为假；
             B_i(k),                F_i(k) = LOWER_BOUND(A_i(k)) 且 HasExact_i(k) 为真，或 F_i(k) = NONE。
```

取得适用的精确行数时，用它替换已保存的值，同时清除旧下界。精确行数可以提高偏低的原估算，也可以降低偏高的原估算。尚无精确值时，多个可用下界取最大值，且不降低当前保存值；已有仍然适用的精确值时，下界不替换该精确值。采样结果不覆盖已经确认并采用的精确行数或下界。[C4]

例如，先前采样得到5000行，后来取得精确行数1200时，可以将保存值降低到1200。先前已经取得的有效下界若为6400，则同一目标在相同统计范围内的精确行数不能是4800；这类矛盾按前述适用条件处理。若输出接口只接受正整数，则仅在生成Hint时将零行转换为接口允许的最小值1，同时保存原始精确值`EXACT(0)`。[C4]

以完整的分类和更新过程为例，设初始保存值B为5000，本轮该目标的估算行数E也为5000。节点尚未返回全部结果且A为2400时，由于A≤E，本轮不生成反馈，保存值仍为5000。后续试运行中，若同一目标的E仍为5000，且已返回行数达到6400并满足执行次数和统计范围一致等条件，则记录下界6400，并将保存值提高到6400。此后确认该目标的全部相关行数均已返回，取得精确行数7200时，将保存值更新为7200。

假设第一轮候选计划`P_1`中，需要修正行数的三个扫描或连接结果分别记为R、J、T。R对应的节点已完成唯一相关调用并读到EOF，父算子不会再调用它；估算为1,000行，实际返回120行，因此记录`EXACT(120)`。J对应的节点尚未返回全部结果，估算为1,000行，已返回2,400行；在计数范围一致、返回量只增不减的条件下，记录`LOWER_BOUND(2,400)`。T对应的节点尚未执行，没有实际行数，因此不反馈，记为`NONE`。

![图6同一执行树中的三类反馈](patent-timeout-feedback-figures/png/fig6-mixed-states.png)

*图6同一计划尚未执行完毕时，不同节点可以分别提供精确行数、行数下界，或者没有可用反馈。*

调优工具合并这些反馈，再生成第二轮计划`P_2`。目标R使用行数120；目标J尚无精确值时，使用已保存值与2,400中的较大值；目标T保持原值。如果`P_2`正常完成，节点统计继续用于反馈，完整运行时间同时用于性能比较；如果再次超时，则在清理前采集统计，并重复上述处理。

本轮没有可用统计时，保留已有规划输入，由迭代控制单元根据剩余预算和停止条件决定是否继续。

## 3 本发明的主要创新点及其对应的技术效果

### 3.1 主要创新点

本发明将限时试运行、取消时统计采集、行数判定、跨轮合并和下一轮规划结合，用于同一SQL的迭代调优。

为每轮候选计划设置执行期限；计划超时取消时，在执行器清理前保存本轮实际执行的计划，以及节点估算行数、累计返回行数和执行状态，使尚未完成的试运行仍可提供后续分析依据。

结合统计所对应的扫描或连接、父子调用关系和计数范围，检查全部相关行数是否已返回，以及实际行数与估算行数能否直接比较，据此区分精确行数、可用下界或无反馈。

在同一反馈目标及统计仍然适用的前提下，用精确行数替换已保存的估算值；尚无精确值时，仅通过有效下界提高原值，多个下界取最大值；没有可用反馈时保持原值。

将合并后的行数值用于下一轮独立规划。正常完成与超时取消均可提供满足条件的行数反馈，完整运行时间另行用于比较候选性能和调整后续执行期限。

### 3.2 对应的技术效果

#### 3.2.1 控制单轮试运行时间，保留有效反馈，降低整体调优耗时

候选计划达到期限后，调优工具取消本轮执行，并在执行器清理前保存可用节点统计，从而限制单个慢计划持续占用调优时间。满足条件的行数继续用于下一轮基数修正，使本次试运行已经取得的信息仍能发挥作用。

在第3.3节所述的历史对照配置下，JOB的113条查询汇总调优时长从2348.17秒降至1599.41秒，减少748.76秒，降幅为31.89%，约为32%；JOB-complex的30条查询从1001.57秒降至582.56秒，减少419.02秒，降幅为41.84%，约为42%。差额和降幅均根据未舍入的数据计算。这些数值反映该批版本和配置下观察到的整体调优耗时变化。单轮期限限制的是候选计划的试运行，整轮调优还包括规划、取消清理和统计处理等开销。[E1][E2][E3]

#### 3.2.2 为全局预算内的后续有效迭代争取时间，增加发现更优计划的机会

当本轮取得的可用统计能够改变下一轮规划输入，且停止条件允许继续时，调优工具可以在当前慢计划未执行完毕的情况下，使用已有反馈生成下一轮计划。减少等待所节省的时间可用于后续规划与试运行，增加在有限调优时间内发现更优计划的机会。

以JOB`16b`为例，无单轮执行期限组进行了4轮反馈后触发全局预算停止条件，正式交付计划的独立完整执行时间约为36秒；初始单轮期限为10秒的组进行了11轮部分反馈迭代，随后因收敛停止。根据该组累计Rows Hint恢复的候选，在后续独立完整执行中的耗时约为11秒，但该候选在原调优过程中未正式交付。该例表明，部分反馈可以支持后续迭代形成更快的候选计划，其最终交付仍需完整执行评价。[E2][E3][E4]

实际迭代次数由反馈是否发生变化、候选计划的耗时及停止条件共同决定。在本次对照中，JOB的总轮次从569变为528，JOB-complex从166变为182。最终计划的性能仍需通过完整执行另行评价。

#### 3.2.3 主要创新点与技术效果的对应关系

| 技术处理 | 对应效果 |
| --- | --- |
| 设置单轮期限，并考虑全局剩余预算 | 限制单个慢计划的试运行时间，为后续规划和试运行保留时间 |
| 在清理前读取节点行数和执行状态 | 在本轮计划尚未执行完毕时，保存已经取得的有效统计 |
| 分别判定精确行数、可用下界和无反馈 | 根据统计能够说明的事实，限定规划输入的更新方式 |
| 合并不同轮次的行数反馈 | 保留同一反馈目标的可用信息，供后续规划继续使用 |
| 分别处理行数反馈和完整运行时间 | 使用部分行数修正下一轮规划，使用完整运行时间比较已完成计划的性能 |

### 3.3 技术效果验证：GaussDB环境下的调优对照

在同一GaussDB数据库上，选取JOB的113条SQL和JOB-complex的30条SQL，比较无单轮超时组与10秒部分反馈组。两组均设置300秒全局调优搜索预算、`patience-iters=5`和1000个初始采样块，关闭外置优化器，并使用相同的优化器参数、交替执行查询。10秒表示可配置的初始单轮上限，后续上限根据反馈迭代中已保留候选的最佳完整运行时间动态缩短。两组使用不同代码版本，因此该对照比较的是两个版本及其配置下的整体效果。[E1]

| 数据集 | 查询数 | 正式交付候选数：无单轮超时 / 10秒部分反馈 | 调优总时长（秒）：无单轮超时 / 10秒部分反馈 | 降幅 |
| --- | ---: | ---: | ---: | ---: |
| JOB | 113 | 103 / 103 | 2,348 / 1,599 | 31.89% |
| JOB-complex | 30 | 28 / 29 | 1,002 / 583 | 41.84% |

![图8 JOB与JOB-complex调优总时长对比](patent-timeout-feedback-figures/png/fig8-tuning-duration.png)

*图8展示两组的调优总时长。表中秒数取整，降幅根据未舍入的数据计算。*

调优结束后，在相同设置下，分别对原始SQL和可用的优化SQL进行三次独立完整执行。下表中每条SQL均采用预先指定的第3次执行时间。如果调优工具未正式交付可用的优化SQL，则采用原始SQL的执行时间。扩展评估另外使用了JOB `16b`、`18c`的恢复候选：根据最后累计的Rows Hint生成候选，核对后独立执行并计时。这两条恢复候选在原调优过程中均未正式交付。[E2][E3][E4]

| 数据集 | 原始SQL总时间（秒） | 无单轮超时组总时间（秒） | 10秒部分反馈组总时间（秒，扩展评估） | 降幅 |
| --- | ---: | ---: | ---: | ---: |
| JOB | 456 | 190 | 168 | 11.25% |
| JOB-complex | 332 | 48 | 47 | 2.26% |

![图9 JOB与JOB-complex优化后总执行时间对比](patent-timeout-feedback-figures/png/fig9-execution-effect.png)

*图9将各条SQL第3次独立完整执行的时间相加。JOB的扩展评估结果中，包含两条未正式交付的恢复候选。*

如果仅使用正式交付的计划，并对未交付查询采用原始SQL的执行时间，则JOB 10秒部分反馈组的执行时间总和约为205秒，无单轮超时组约为190秒。JOB-complex的上述评估没有使用恢复候选。调优耗时与最终计划的执行耗时分别评价，恢复候选的独立测量与工具正式交付也分别记录。

JOB `16b`在无单轮超时组中运行4轮后触发全局预算停止条件，所交付计划的完整执行时间约为36.38秒。10秒组进行了11轮部分反馈迭代；根据最后累计的Rows Hint生成恢复候选后，三次独立完整执行的测量值分别为10.89、10.92、10.99秒。该恢复候选当时未正式交付。[E2][E3][E4]

## 参考资料与实现依据

### 现有技术与论文

- [R1] Stillger等，*LEO - DB2's LEarning Optimizer*，VLDB 2001，[本地论文PDF](../sources/papers/P019.pdf)。
- [R2] Wu等，*Sampling-Based Query Re-Optimization*，SIGMOD 2016，[本地论文PDF](../sources/papers/1601.05748v1.pdf)。
- [R3] Chakkappen等，*Adaptive Statistics in Oracle 12c*，PVLDB 10(12), 1813-1824, 2017，[论文](https://www.vldb.org/pvldb/vol10/p1813-zait.pdf)，DOI [10.14778/3137765.3137785](https://doi.org/10.14778/3137765.3137785)。参见PDF第8-9页，第3.2.2、3.3.1-3.3.2节。
- [R4] Oracle Database 19c，[Monitoring Database Operations](https://docs.oracle.com/en/database/oracle/oracle-database/19/tgsql/monitoring-database-operations.html#GUID-11748F62-BE24-4511-B77B-F28D5E443A0A)。
- [R5] Microsoft Learn，[Live Query Statistics](https://learn.microsoft.com/en-us/sql/relational-databases/performance/live-query-statistics?view=sql-server-ver17)。
- [R6] Microsoft Learn，[sys.dm_exec_query_profiles](https://learn.microsoft.com/en-us/sql/relational-databases/system-dynamic-management-objects/sys-dm-exec-query-profiles-transact-sql?view=sql-server-ver17)。
- [R7] Microsoft Learn，[Query Profiling Infrastructure](https://learn.microsoft.com/en-us/sql/relational-databases/performance/query-profiling-infrastructure?view=sql-server-ver17)。
- [R8] Oracle Database 12c，[How Adaptive Query Plans Work](https://docs.oracle.com/database/121/TGSQL/tgsql_optcncpt.htm#GUID-A42B8874-95AF-4D5B-B5D9-F0376F91D15E)。
- [R9] Oracle Database 19c，[`V$SQL_PLAN_MONITOR`](https://docs.oracle.com/en/database/oracle/oracle-database/19/refrn/V-SQL_PLAN_MONITOR.html)。
- [R15] MariaDB，[`SHOW ANALYZE FOR`](https://mariadb.com/docs/server/reference/sql-statements/administrative-sql-statements/show/show-analyze)。
- [R16] Amazon Redshift，[`SYS_QUERY_DETAIL`](https://docs.aws.amazon.com/redshift/latest/dg/SYS_QUERY_DETAIL.html)与 [`SYS_QUERY_HISTORY`](https://docs.aws.amazon.com/redshift/latest/dg/SYS_QUERY_HISTORY.html)。
- [R17] OceanBase Cloud，[Execution Profile](https://en.oceanbase.com/docs/common-oceanbase-cloud-10000000001725119)。
- [R18] Databricks，[Adaptive Query Execution](https://docs.databricks.com/aws/en/optimizations/aqe)。
- [R24] Markl等，*Robust Query Processing through Progressive Optimization*，SIGMOD 2004，[IBM Research](https://research.ibm.com/publications/robust-query-processing-through-progressive-optimization)，[论文](https://dsf.berkeley.edu/cs286/papers/robust-sigmod2004.pdf)。参见PDF第3页第2节及第5-6页第2.3节。
- [R25] Raman等，*Progressive Optimization in Action*，VLDB 2004，[论文](https://www.vldb.org/conf/2004/DEMP22.PDF)。参见PDF第2页图1及流程说明。
- [R26] Chaudhuri、Narasayya、Ramamurthy，*Estimating Progress of Execution for SQL Queries*，SIGMOD 2004，[Microsoft Research](https://www.microsoft.com/en-us/research/publication/estimating-progress-of-execution-for-sql-queries/)，[论文](https://www.microsoft.com/en-us/research/wp-content/uploads/2016/02/progress-1.pdf)。参见PDF第5-6页第4.1-4.2节及第12页Appendix B。
- [R27] Lee等，*Operator and Query Progress Estimation in Microsoft SQL Server Live Query Statistics*，SIGMOD 2016，[Microsoft Research](https://www.microsoft.com/en-us/research/publication/operator-and-query-progress-estimation-in-microsoft-sql-server-live-query-statistics/)，[论文](https://www.microsoft.com/en-us/research/wp-content/uploads/2016/06/qprog.pdf)。参见PDF第6页第4.2节及第11页参考文献 [7]。
- [R28] Microsoft Learn，[Logical and Physical Showplan Operator Reference](https://learn.microsoft.com/en-us/sql/relational-databases/showplan-logical-and-physical-operators-reference?view=sql-server-ver17)。参见Physical operators、Rebinds, rewinds, and end-of-scans、How to read an execution plan。
- [R29] IBM Db2 12.1.x，[section_actuals - Section actuals configuration parameter](https://www.ibm.com/docs/en/db2/12.1.x?topic=parameters-section-actuals-section-actuals)。
- [R30] IBM Db2 11.5.x，[EXPLAIN_FROM_ACTIVITY procedure](https://www.ibm.com/docs/en/db2/11.5.x?topic=er-explain-from-activity-procedure-explain-statement-using-activity-event-monitor-information)。
- [R31] Oracle Database 12.1，[Reoptimization: Statistics Feedback](https://docs.oracle.com/database/121/TGSQL/tgsql_optcncpt.htm#GUID-739F1F0B-ABE2-44C9-9783-8FA54F4ADB12)。
- [R32] Oracle Database 12.1，[Optimizer Statistics Concepts: SQL Plan Directives](https://docs.oracle.com/database/121/TGSQL/tgsql_statscon.htm)。

### 相关专利

- [R19] Oracle，*Leveraging query-specific statistics in non-equivalent queries*，`US11321317B2`，[授权文本](https://patents.google.com/patent/US11321317B2/en)，[本地原件](patent-originals/US11321317B2.pdf)。参见授权PDF第14-20页的自适应与重新优化说明，以及第31-32页的权利要求；权利要求1涉及非等价后续查询的统计复用。
- [R20] IBM，*System, method, and computer program product for progressive query processing*，`US7716215B2`，[授权文本](https://patents.google.com/patent/US7716215B2/en)，[本地原件](patent-originals/US7716215B2.pdf)。参见授权PDF第21-26页、PDF第9页图7，以及第27-28页权利要求1。
- [R21] Microsoft，*Query progress estimation*，`US7493337B2`，[授权文本](https://patents.google.com/patent/US7493337B2/en)，[本地原件](patent-originals/US7493337B2.pdf)。参见授权PDF第20-21页、PDF第13页图12，以及第24-25页权利要求22-33、42。
- [R23] Sybase，*System and methodology for automatic tuning of database query optimizer*，`US9213740B2`，[授权文本](https://patents.google.com/patent/US9213740B2/en)，[本地原件](patent-originals/US9213740B2.pdf)。参见中断候选的部分耗时记录说明及权利要求1。

上述论文页码从论文PDF第一页起算，授权专利页码从封面第一页起算。

### 当前实现依据

- [C2] [`capture_executor_run`](../contrib/extopt-gaussdb/extopt_partial_capture.cpp)、[`partial_gaussdb_execution_capture_evidence`](../contrib/extopt-gaussdb/extopt_partial_gaussdb.cpp)：取消发生时的统计采集及字段来源。
- [C3] [`partial_observation_evidence`](../contrib/hint_adviser/hint_adviser_partial_proof.c)、Rows目标收集和计划身份核对：节点行数分类，以及统计与反馈目标的对应关系。
- [C4] [`rows_hint_merge`](../contrib/hint_adviser/hint_adviser_rows_state.c)、[`feedback_loop_advance`](../contrib/hint_adviser/hint_adviser_feedback_policy.c)：跨轮合并、部分反馈轮次和最佳候选的选择规则。

### 实验记录

- [E1] [实验配置、代码版本及校验信息](../data/patience5-ab-20260928/run-manifest.txt)。
- [E2] [逐查询工作簿](../outputs/job_job_complex_tuning_query_summary_patience5_20260928.xlsx)及对应的[原始输入记录](../data/patience5-ab-20260928/results/workbook-input.tsv)。
- [E3] [独立执行原始记录](../data/patience5-ab-20260928/results/measurements.tsv)、调优记录和事件记录。
- [E4] [恢复候选来源明细](../data/patience5-ab-20260928/results/recovered-current-partial.tsv)及恢复规则。
- [E5] [调优时长图表的可编辑PPT](patent-timeout-feedback-figures/pptx/fig8-tuning-duration.pptx)和[执行效果图表的可编辑PPT](patent-timeout-feedback-figures/pptx/fig9-execution-effect.pptx)。

## English Summary

**Title: An Iterative Query Execution Plan Optimization Method Based on Partial Feedback from Time-Limited Execution**

IBM's POP papers describe checkpoint-triggered reoptimization of the current query, reuse of actual cardinalities, and cost-based selection of materialized intermediate results. Oracle's related work covers reoptimization during an individual query and saving full or partial execution information for later query optimization. Microsoft's progress-estimation research uses operator counts, cardinality bounds, and termination facts; the product documentation also describes scan-end counters, repeated execution, and operator closure. The present method applies qualified counts captured on candidate cancellation to the next independent tuning round through explicit qualification and merge rules.

Each candidate for the same SQL runs with an execution deadline. On timeout cancellation, the original plan and operator estimates, returned rows, invocation state, and local EOF information are captured before executor cleanup. Partial feedback refers to usable operator statistics from a candidate whose execution has not completed; it may include exact counts for qualified operators as well as lower bounds.

Feedback qualification identifies the scan or join represented by each observation and requires compatible query semantics and counting ranges. An exact count requires all relevant output to have been returned and no further output to be possible if the candidate continued. An unfinished, monotone count yields a usable lower bound only when it exceeds a comparable estimate. Other observations produce no feedback. Counts are merged across rounds only while the target and its statistics remain applicable. Contradictory exact counts and bounds are excluded from merging until their target and counting ranges are reconciled. Qualified exact counts replace previous estimates and clear bounds. Without an exact count, lower bounds only raise inputs and merge by maximum; an existing valid exact count takes precedence over applicable bounds. Accepted feedback informs the next independently planned candidate. Normally completed candidates also supply qualified row feedback, while complete runtimes update the best measured candidate and adaptive deadline.

The initial cap T₀ is configurable and was ten seconds in the experiment. Once the feedback iterations retain a normally completed candidate with best complete runtime t_best, later caps are min(T₀, 1.25 × t_best), further limited by the available global tuning budget with time reserved for cancellation and processing.

The business benefits are shorter slow-candidate trials with usable statistics retained, and more opportunity for useful subsequent iterations. In the historical comparison of two versions under the recorded workload configuration, aggregate tuning time was 31.89% lower on JOB and 41.84% lower on JOB-complex. For JOB 16b, four rounds without a deadline for each candidate produced a delivered plan with a complete runtime of about 36 seconds; eleven partial-feedback rounds produced a recovered candidate independently measured at about 11 seconds. The recovered candidate was not formally delivered. Formal delivery results and extended evaluations that include recovered candidates are reported separately.
