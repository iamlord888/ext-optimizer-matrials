# 五件数据库专利与超时节点反馈方案的重合分析

核对日期：2026-09-30。本文比较 IBM `US7716215B2`、Oracle `US11321317B2`、Microsoft `US11334538B2` 与 `US7493337B2`、Sybase `US9213740B2` 的英文授权文本，与当前 `hint_adviser` 原型及[交底书草稿](patent-timeout-feedback-disclosure-draft.md)拟描述的技术方案。英文引文来自各件[原始 PDF](patent-originals/MANIFEST.md)对应的说明书或权利要求；段号沿公开英文全文，权利要求号沿授权文本。这里的“未见”仅指所核五件文本没有明确写出该步骤，不等于该技术从未公开，也不是可授权性或侵权结论。

## 一、比较对象与总体判断

本案面对的是多轮、独立候选计划的试运行。每轮以真实 SQL 候选进行 `EXPLAIN ANALYZE`，设定执行期限；期限到达而候选尚未结束时，取消该次执行。在执行器对象失效前取得**已生成的原物理计划**及其部分节点运行观测，然后决定哪些行数能修正下一轮候选的基数输入。这里“部分”修饰的是运行观测，并非计划树只生成了一部分。

拟讨论的严格方法有四道相继的门槛：①取消时取得节点身份、原估算、累计输出和执行状态；②将观测绑定到同一查询块、谓词、连接与调用范围的基数目标；③证明目标已完整，或者证明同口径已输出量只是可用于纠正低估的下界；④仅将合格行数送入下一轮独立候选，不把取消期限当作本轮的完整运行时间。所谓“独立候选”，可以是**同一原始 SQL 加不同 Rows Hint/Outline 后的下一次试运行**，不能简单理解为 Oracle 权利要求 1 所称“非等价第二查询”。

**总体判断：这些构件大多已有明确先例。** IBM 已公开执行中检查点、时间与误差双阈值、EOF/前缀越界判断及全计划节点统计；Oracle 已公开部分执行的操作统计用于以后查询，亦公开运行统计触发提前终止；Microsoft 两件分别公开逐节点实际/估算基数到未来优化的反馈链，以及算子行数下界与运行中估计修正；Sybase 已公开被中断候选的部分耗时可作为计划选择证据。因此，宽泛主张“超时后取得部分计划”“把 A-rows 反馈给优化器”“未完成行数是下界”或“中断候选仍有价值”，均有很高的现有技术风险。五件单篇文本中尚未核到上述四道门槛按**同一取消现场、同一目标口径、下一轮独立候选**组成的完整处理链；但将这些文献组合，是必须正面应对的创造性风险。

| 专利 | 与本案重合最强的环节 | 所核文本的主要用途或触发条件 | 不能单独作为本案区别点 |
| --- | --- | --- | --- |
| IBM `US7716215B2` | 运行中基数比对、EOF/前缀阈值、时间条件、全树节点统计 | 计划内 `CHECK` 触发当前查询暂停、重优化、重启 | `A` 超阈可证低估；EOF 才能证实低输出；执行时间阈值 |
| Oracle `US11321317B2` | 部分执行、表/连接等操作统计、随后查询使用、提前终止 | 自动重新优化相同/等价查询；授权权利要求 1 另涉及非等价第二查询 | “部分执行统计供下一次规划” |
| Microsoft `US11334538B2` | 每节点估算/实际基数、执行中事件、后续 CE feedback、hint | 基数估计反馈服务以后相同或相似查询 | “微软只有监控”“Rows Hint 形式的反馈首次出现” |
| Microsoft `US7493337B2` | 算子已输出量作下界、多节点上下界、运行中修正估计、中止入口 | 当前查询的进度估计与展示 | “前缀输出是下界”“按边界修正节点估计” |
| Sybase `US9213740B2` | 候选被中断后保存部分耗时并学习坏计划 | 计划级性能比较与未来计划选用 | “取消轮次完全没有可用证据” |

## 二、逐件原文对照

### 2.1 IBM `US7716215B2`：检查点、时间阈值和部分计数

[英文原件](patent-originals/US7716215B2.pdf)；[公开全文](https://patents.google.com/patent/US7716215B2/en)。其说明书 `p-0040` 直接描述执行中计数和重优化：

> “checkpoint operators, which compare the optimizer's cardinality estimates with the actual number of rows processed thus far”

权利要求 1 同时要求执行时间超过剩余时间阈值和估算误差超过阈值后，暂停计划、重新优化并重启；其中分别出现 “above a remaining execution time threshold” 和 “restarting query execution with the re-optimized query plan”。因此不能说 IBM 没有时间条件，也不能只用“本案设有超时”与其区分。不过 IBM 的时间条件与误差条件在计划内检查点同时判定，服务于**当前查询**的重规划与重启；本案候选达到外部单轮期限后按取消结束，下一轮重新产生独立候选。

说明书 `p-0065` 对部分计数的逻辑已经相当接近本案：

> “when its child in the QEP returns EOF and the buffer contains less than b rows at this time”

> “when the bth row is inserted into the buffer”

第一句是在子流到 `EOF` 时确认输出低于阈值；第二句表示第 `b` 行一出现即可确认越过阈值。故“尚未完成的前缀足以证明原估算偏低，而输出偏少须等待完成”不是新颖点。`p-0088` 还写道：

> “for all nodes of a QEP, not only for the materialization point”

该句的上下文是取得各节点基数及更详细统计，反馈给后续重优化阶段。不能将 IBM 说成“只采物化点”。**剩余可讨论的区别**是：无需事先在候选计划内安放 `CHECK`，外部期限导致的取消发生后，在对象清理前读取原计划节点；对没有在检查点形成完整中间结果的节点，还须核对目标及调用范围，判断本地 EOF 后有无未来重入，再供下一轮独立候选使用。IBM 的独立权利要求还包含物化、结果复用及输出补偿等具体限制；说明书公开的技术事实仍应作为现有技术对待。

### 2.2 Oracle `US11321317B2`：终止长计划、运行中换计划与跨次反馈

[英文原件](patent-originals/US11321317B2.pdf)；[公开全文](https://patents.google.com/patent/US11321317B2/en)。说明书 `[0009]` 已经讨论运行过久的 *catastrophic plan*：用户或 DBA 可以终止该计划，之后需要采取措施避免其再次被选中。该段属于问题背景，没有单独规定终止现场的逐节点快照。

说明书 `[0083]` 又明确区分当前执行的 adaptive plan 与以后执行的 automatic reoptimization，并写道：

> “The first execution of the query may have been fully executed or partially executed.”

`[0084]` 列出的可收集事实包括：

> “cardinality of a table, cardinality of a join, cardinality of a group by, cardinality of an index”

`[0087]` 又指出收集器可设置在 “such as after each operator”，并将至少部分信息 “fed back into the query optimizer the next time a similar query is received”。这三处合起来已经公开了**部分执行 + 操作级基数 + 以后相似查询的优化器消费**，不能在交底书中笼统表述成 Oracle 必须等待第一条 SQL 完全执行。`[0037]` 甚至提醒收集行数可能只根据已读取表的严格子集形成：

> “The number of rows may only be based on a strict subset of the table that has been read.”

Oracle 还公开另一条与本案高度相关的提前终止路径。`[0105]` 说：

> “execution of an execution plan for a query is terminated before the execution plan is fully executed based on statistics that are collected during runtime.”

`[0106]`–`[0107]` 进一步说明，是否终止还可以结合剩余操作数量、剩余成本，以及利用运行统计重新计算当前计划和替代计划的总成本；当替代计划明显更优时，终止当前计划并启动另一计划。因此，本案也不能仅以“终止未完成计划再优化”区分 Oracle。Oracle 的 adaptive plan 和 in-flight reoptimization 都是在**当前查询执行过程中**从当前计划切换到其它计划；automatic reoptimization 则把本次执行统计用于**后续查询编译**。本案的边界是外部调优器针对同一 SQL 的迭代循环：第 `i` 轮候选到达期限后被取消，清理前捕获可用节点信息，再用于第 `i+1` 轮候选，而不是在当前候选内部完成动态换计划。还需区分**外部期限触发**与**误估/成本收益触发**，以及取消现场对多个既有节点的可信性筛选。

授权权利要求 1 的重点另有边界：它要求为第一计划中的**一个操作**存储统计，并在收到 “a second query that is not equivalent to the first query” 后，按数据库对象和操作识别相关统计，确定第二计划。`[0083]` 的相同/等价查询实施例与权利要求 1 的非等价第二查询不能混写为一个条件。对本案最不利的是说明书的宽泛部分反馈公开；单看该授权权利要求 1 的文字，又没有直接列出“预设期限取消后的逐节点 `EXACT/LOWER_BOUND/NONE` 证明”。

### 2.3 Microsoft `US11334538B2`：节点级基数反馈并非只有监控

[英文原件](patent-originals/US11334538B2.pdf)；[公开全文](https://patents.google.com/patent/US11334538B2/en)。说明书 `[0024]` 称查询监测器产生事件的时间为：

> “during and at the completion of execution of the queries”

`[0026]` 对 profile data 的描述更直接：

> “estimated and actual cardinality values for each node in a query plan”

同段允许修正采取 “additional query hints” 等形式。权利要求 1 的反馈管理器依据 “a difference between the estimated cardinality and the actual cardinality” 形成变更建议；从属权利要求 5 进一步称建议可 “alter a subsequent execution of the query and one or more similar queries”。由此，**逐节点估算/实际基数、执行中事件、以后查询的 CE 修正、hint 形式的输出**，均不能写成本案首创。微软既有实时监控产品，也有这件直接涉及反馈优化的专利，两类公开应分别讨论。

另一方面，`[0028]` 的常规数据流写明 “At the end of query execution, the query profile data and/or standard profiling data is submitted to an optimization service for analysis”。`[0024]` 已允许执行中事件，所以不能据 `[0028]` 推出该专利**一律**等查询完成才有信号。所核文本没有明确给出：预设单轮期限导致取消后，在执行器清理前保留原计划节点事实；对可能继续输出的节点先证明目标及调用口径，再分为完整值、可用下界或不反馈。这个限定的**缺席于所核文本**是比较点，不是“SQL Server 产品没有该能力”的断言。

### 2.4 Microsoft `US7493337B2`：下界及估计修正已经公开

[英文原件](patent-originals/US7493337B2.pdf)；[公开全文](https://patents.google.com/patent/US7493337B2/en)。独立权利要求 22 的主题是 “A method of estimating query progress”。权利要求 23 从属于 22，以算子返回项数建立工作量模型；权利要求 27 再从属于 23，明确写道：

> “setting the lower bound to a number of items returned by the query operator at a given point during query execution.”

权利要求 33 同样从属于 23，写有 “upper and lower bounds are maintained for a plurality of query operators in a query execution plan”，并将边界变化传播至其它算子。因此，前缀输出作为下界、多算子边界维护与传播，不能作为本案单项发明点。

这件专利也不只是静态显示。说明书 `p-0089` 写道：

> “the initial estimates from the optimizer are refined using feedback obtained during query execution.”

`p-0091` 进一步称：

> “If the current optimizer estimate for the node is outside the bounds, the optimizer is updated”

上下文是**本次查询进度估计模型**在运行期间更新节点估计和总工作量，而未明确写出取消后生成下一轮独立候选物理计划。这里应如实承认它已将算子界限用于修正“optimizer estimate”，再把用途和生命周期区别开。独立权利要求 42 包含允许用户 “abort execution of a query” 的输入设备，但其主题仍是进度显示；权利要求 42 与 27 不是同一权利要求链，不能机械拼成“中止后把算子下界送到下一轮优化器”的单一权利要求。不过，二者都属于可供后续组合论证的公开技术启示。

### 2.5 Sybase `US9213740B2`：中断候选并不等于白费

[英文原件](patent-originals/US9213740B2.pdf)；[公开全文](https://patents.google.com/patent/US9213740B2/en)。权利要求 1 要求对新计划记录执行指标，以优于或劣于已知最佳计划来学习未来应采用或避开的计划。说明书 `[0130]` 说中止后的执行引擎仍须：

> “record the partial “plancost” information even when the execution is aborted.”

并明确限定其所需事实：

> “only partial elapsed time is needed.”

`[0194]` 举出管理员因计划运行过久而中止、尝试其它计划的情形，并说：

> “execution is “incomplete” but the system nevertheless tracks the query plan's metrics.”

若中止前已耗时间超过既有最佳计划，它已足以证明该计划较慢，因而可记作坏计划；尚未超过时，则将中断视为无效训练执行。故“取消候选没有任何有益信息”不成立，且 **Sybase 也有证据可用/不可用的筛选**。它筛选的是计划级已耗时能否证实劣势，所核文本没有将被取消计划的逐节点行数映射到目标基数，也没有目标完整性、重入和累计调用口径的证明。本案可以强调部分时长不能伪装为**完整运行时间**，但不能说部分时长对候选比较完全无用；当前原型也仅在超时已足以证实未达到改进门槛时把它计入停止策略，不将其更新为最好候选的完整耗时。[实现依据](../contrib/hint_adviser/hint_adviser_feedback_policy.c#L8)

## 三、组合风险与可以主张的边界

单篇未完整写出本案链条，并不自动产生创造性。审查时最可能出现的组合如下：

| 拟写得过宽的表述 | 最直接的公开组合 | 对交底书的影响 |
| --- | --- | --- |
| “超时/中断后仍利用部分执行信息” | Oracle `[0083]` 的部分执行统计 + Sybase `[0130]`、`[0194]` 的中断学习 | 必须说清所得信息是**逐节点基数**，并解释取消现场如何判定其适用性。 |
| “执行中已输出行数超过估算就修正” | IBM `p-0065` 的越阈判断 + Microsoft `US7493337B2` 权利要求 27、`p-0089` 的下界修正 | `A>E` 和下界更新本身保护空间很小；应限定同一目标及调用口径、取消后的下一轮用途。 |
| “逐节点基数反馈下一次优化” | Oracle `[0084]`、`[0087]` + Microsoft `US11334538B2` `[0026]`、权利要求 1/5 | 必须具体说明未完成节点、局部 EOF、未来重入和无反馈的判定。 |
| “从计划全部节点取得统计” | IBM `p-0088` + Microsoft `US11334538B2` `[0026]` | 不能以全树采集作为核心；目标级可用性证明比采集范围重要。 |
| “取消轮次的耗时不参与最快计划选择” | Sybase `[0194]` 已识别部分耗时只在越过最佳时才有判别力 | 这是评价口径约束，可作为配套限定，难以单独支撑发明点。 |

**较稳妥的技术边界**可写成连续依赖关系：为真实候选设置单轮期限；期限触发取消时，在运行对象清理前取得同一原计划的节点身份、累计行数和状态；对每个待修正基数目标，先核对查询语义、数据可见性、调用次数与计数/估算范围；仅在目标全部相关调用已经完成、且父链不会再使其输出时给 `EXACT(A)`，在同口径输出只能累加且 `A>E` 时给仅抬高估算的 `LOWER_BOUND(A)`，否则给 `NONE`；按证据强度合并后交给下一轮**独立候选**规划。取消时长保留为已耗时事实，但不作完整运行时间。这是需要进一步审查的**组合候选**，不是对其中单个步骤的首创声明。

这里的调用口径并非文字修饰。例如某 Nested Loop 内侧节点对第一条外侧行已到 `EOF`，但第二条外侧行稍后仍会重新调用它，则该节点的本地完成不等于目标在整轮执行中完整。又如计划估算 `E=100` 表示**每次调用**的行数，节点被调用两次、每次实际输出 80 行，累计 `A=160`；直接据 `A>E` 认定单次基数低估就会得出错误结论。因此，应把“父链未来重入”和“估算/观测同调用范围”写成可执行的证明或保守拒绝规则，而非只列执行器字段名。

申请前还宜核查上述五件的更早公开文本与同族：IBM 母案 `US20050097078A1`（2005-05-05）及本继续申请 `US20080177722A1`，Oracle 同族 `US20140095475A1`（2014-04-03）及本件申请 `US20170357692A1`，Microsoft 两件的 `US20050222965A1` 与 `US20200379963A1`，Sybase `US20090100004A1`。授权日不是相关内容的最早公开日；[扩展检索笔记](patent-timeout-feedback-patent-landscape.md)还列有其它专利、论文与产品。后续应以完整权利要求及同族、审查档案核验组合风险。

## 四、当前实现对上述边界的支撑程度

报告不能把拟议的充分条件写成目前所有节点已由代码证明。当前工作树的关键事实如下：

| 环节 | 已有实现依据 | 对交底书的约束 |
| --- | --- | --- |
| 取消清理前取证 | [`capture_executor_run`](../contrib/extopt-gaussdb/extopt_partial_capture.cpp#L616) 在查询取消异常中输出快照，再重新抛出原异常。 | 可写“取消时保留原计划的部分运行观测”；不可写“超时 `EXPLAIN ANALYZE` 正常返回完整结果”。 |
| 执行器字段 | [`partial_gaussdb_execution_capture_evidence`](../contrib/extopt-gaussdb/extopt_partial_gaussdb.cpp#L579) 从 `Instrumentation` 取得 `status`、活动线索、累计 `ntuples`、`nloops`；无计数节点跳过。 | `status` 为局部结束线索；`nloops` 是已关闭计数时期，不能直接证明未来不会重入。 |
| 证据分类 | [`partial_observation_evidence`](../contrib/hint_adviser/hint_adviser_partial_proof.c#L14) 以 `TERMINAL && rescan_count == 0` 给 `EXACT`，否则在 `A>E` 时给 `LOWER_BOUND`。 | 这还未沿父链排除未来**首次**重入，也未对多次调用累计 `A` 与单次 `E` 全面归一；不能宣称当前规则对所有拓扑都严格证明精确值或下界。 |
| 目标绑定与跨轮合并 | [Rows key](../contrib/hint_adviser/hint_adviser_rows_private.h#L26) 主要由查询块及关系目标组成；[合并逻辑](../contrib/hint_adviser/hint_adviser_rows_state.c#L55) 保留精确证据、合并下界，但新一轮 `EXACT` 可覆盖旧值。 | key 尚不能单独证明谓词、连接类型、外侧参数、数据可见性一致；严格语义核对与冲突失效属于拟议方法或后续实现限定。 |
| 完整时间与部分时长 | [反馈循环](../contrib/hint_adviser/hint_adviser_feedback_policy.c#L37) 只让正常完成的候选更新最好运行时间；部分轮次的期限在足以证实未达改进门槛时影响耐心计数。 | 应写“不以期限冒充完整运行时间”，不应写“取消轮次的耗时完全不参与任何判断”。 |

据此，交底书最需要把**已运行且本地 EOF**与**目标的全部相关输出已完成**分开，把**累计节点输出**与**单次目标基数**分开。若选择主张严格的目标级证明，应同时说明不满足证明的节点返回 `NONE` 或取得分次计数后再处理；在相关代码和验证补齐前，不能以当前原型的 `TERMINAL && rescan_count == 0` 作为普遍充分条件。
