# 五件近似专利原文引证核对笔记

核对日期：2026-09-30。来源为已下载的五件英文授权 PDF（见 [`design/patent-originals/MANIFEST.md`](../../design/patent-originals/MANIFEST.md)）及相应 Google Patents 英文全文。以下引文仅截取与本案直接相关的短句；“未见”限定于本轮核对的公开文本，不能推断其它公开文献或同族申请不存在相应内容。说明书编号沿 Google Patents 文本显示的 `num`；IBM、Microsoft 2009 件显示为 `p-` 编号。

本案比较对象：预设单轮期限取消真实候选后，在执行器清理前读取原计划逐节点状态与累计输出；核对节点观测与待修正基数目标的语义/调用口径；据是否全局完成将其作为精确值、可用下界或不反馈；把合格证据送到下一轮独立候选的规划输入。

## 1. IBM US7716215B2：执行中检查点、EOF 和所有节点基数

原件：[英文 PDF](../../design/patent-originals/US7716215B2.pdf)；[Google Patents](https://patents.google.com/patent/US7716215B2/en)。

| 原文短引及定位 | 能支持的结论 | 不支持的扩大解释 |
| --- | --- | --- |
| “checkpoint operators, which compare the optimizer's cardinality estimates with the actual number of rows processed thus far” （说明书 `p-0040`） | 计划中的 CHECK 以当前已处理行数比对估算；实时基数误差监测已公开。 | 未说明外部调优器设定期限，在任意取消现场读取整棵原计划。 |
| “when its child in the QEP returns EOF and the buffer contains less than b rows at this time” （说明书 `p-0065`） | 对“少于阈值”的判断需要相应子流 EOF；前缀少不能直接证明最终少。 | 单个 CHECK 子流的 EOF 不等于任意执行计划节点对于任意调用口径都已全局完成。 |
| “when the bth row is inserted into the buffer” （说明书 `p-0065`） | 达到阈值时，尚未 EOF 即可确定越过阈值。 | “前缀超过估算可证低估”并非本案独创。 |
| “for all nodes of a QEP, not only for the materialization point” （说明书 `p-0088`） | 说明书明确设想获取整棵 QEP 各节点的基数统计，再用于后续重优化阶段。 | 不能声称 IBM 仅在物化点采集行数。此句未给出超时取消时逐节点证据分级。 |
| “restarting query execution with the re-optimized query plan” （权利要求 1） | 其主张包含对**正在运行的同一查询**暂停、重优化及重启。权利要求 1 还要求执行时间阈值与估算误差阈值均满足，并有中间结果/输出补偿安排。 | 其权利要求中的时间阈值不是本案单轮期限取消后的独立候选迭代；但不能以“IBM 没有时间条件”区分。 |

**边界判断：** EOF 辅助证明、前缀越阈、所有节点统计、执行时间阈值均与本案近似。比较点须落在期限取消的取证时点、没有预置 CHECK 的原计划节点、跨调用完整性/口径判定及下一轮独立候选的使用上。

## 2. Oracle US11321317B2：部分执行、逐算子统计与后续查询

原件：[英文 PDF](../../design/patent-originals/US11321317B2.pdf)；[Google Patents](https://patents.google.com/patent/US11321317B2/en)。

| 原文短引及定位 | 能支持的结论 | 不支持的扩大解释 |
| --- | --- | --- |
| “The first execution of the query may have been fully executed or partially executed.” （说明书 `[0083]`） | 原文明确允许首次查询只执行一部分，随后用所收集信息改善相同/等价查询的执行。 | 不能写成 Oracle 必须等 SQL 全部结束才取得反馈；也不能仅据这句认定它实现了期限取消时对所有节点的完成性证明。 |
| “cardinality of a table, cardinality of a join, cardinality of a group by” （说明书 `[0084]`） | 表、连接、分组等不同操作的运行期基数都列为可收集信息。 | 不能把“算子级 A-rows”单独作为区别点。 |
| “The number of rows may only be based on a strict subset of the table that has been read.” （说明书 `[0037]`） | 统计收集器可能只看已读取表的一部分；该数值并不自动构成完整基数。 | 该例没有说明对一般计划节点输出的精确/下界/无反馈分类规则。 |
| “such as after each operator” 及 “fed back into the query optimizer the next time a similar query is received” （说明书 `[0087]`，两个分开短引） | 说明书设想在多个操作后置统计收集器，并于相似查询再次到来时反馈优化器。 | 不能声称 Oracle 的部分统计只供当前计划使用。 |
| “a second query that is not equivalent to the first query” （权利要求 1） | 授权独立权利要求 1 的重点是按对象和操作关联保存**某一个操作**的统计，并用于后来的非等价查询；与说明书 `[0083]` 的相同/等价查询实施例应分开描述。 | 不能把“非等价查询”说成所有实施例的唯一适用范围，也不能把权利要求 1 直接等同于取消后全树分级。 |
| “execution of an execution plan for a query is terminated before the execution plan is fully executed” （说明书 `[0105]`） | Oracle 还公开：运行期统计与估算显著不同时，可提前终止当前计划并产生新计划，称 in-flight reoptimization。 | 这一路径由统计偏差触发、服务当前查询；所核段落未见外部固定期限取消、清理前快照和下一轮独立候选的三级证据判定。 |

**边界判断：** “部分执行 + 算子统计 + 后续查询优化”已被说明书正面公开；“运行中提前终止并换计划”也有。最应避免将本案概括为首次解决这两件事。说明书 `[0083]` 与授权权利要求 1 的第二查询关系不同，报告需分别定位。

## 3. Microsoft US11334538B2：节点级 CE feedback 链

原件：[英文 PDF](../../design/patent-originals/US11334538B2.pdf)；[Google Patents](https://patents.google.com/patent/US11334538B2/en)。

| 原文短引及定位 | 能支持的结论 | 不支持的扩大解释 |
| --- | --- | --- |
| “during and at the completion of execution of the queries” （说明书 `[0024]`） | 查询监测器可在执行期间和完成时发出包含实际基数等信息的事件。 | 不能把微软反馈机制限定为仅在 SQL 完成后生效。 |
| “estimated and actual cardinality values for each node in a query plan” （说明书 `[0026]`） | 所述 profile data 可含每个计划节点的估算基数和实际基数。 | 不能声称微软只有语句级状态/行数；此处未给出取消节点是否完整的具体规则。 |
| “based at least on a difference between the estimated cardinality and the actual cardinality” （权利要求 1） | 独立权利要求 1 明确把估算与实测基数差异送入反馈管理器形成变更建议。 | 不能说微软只做进度显示或排障。 |
| “alter a subsequent execution of the query and one or more similar queries” （权利要求 5） | 后续相同/相似查询可消费该建议；权利要求 5 从属权利要求 1。 | 其所核权利要求未指定单轮期限取消后的原计划节点取证与证据分级。 |
| “This may take the form of additional query hints” （说明书 `[0026]`） | CE 修正可通过 query hints 等形式注入，Rows Hint 作为反馈接口并非宽泛区别。 | 不能把该句具体解释为本案的同语义 Rows 目标、同调用口径及仅低估下界更新规则。 |

**边界判断：** 这是“执行期节点 profile + 估算/实际差异 + 后续查询优化”的直接近似文献。核心待区分应是期限取消后对尚未完成的各节点如何取得、证明及筛选证据。

## 4. Microsoft US7493337B2：算子下界、估计修正和中止入口

原件：[英文 PDF](../../design/patent-originals/US7493337B2.pdf)；[Google Patents](https://patents.google.com/patent/US7493337B2/en)。

| 原文短引及定位 | 能支持的结论 | 不支持的扩大解释 |
| --- | --- | --- |
| “setting the lower bound to a number of items returned by the query operator at a given point during query execution” （权利要求 27，从属 23、22） | 当前已输出行数作为算子最终输出下界是明确权利要求内容。 | `A` 是下界这一数学事实不是本案独创；该权利要求用途为查询进度估计。 |
| “upper and lower bounds are maintained for a plurality of query operators in a query execution plan” （权利要求 33，从属 23、22） | 其权利要求覆盖计划中多个算子的上下界维护，并有边界向其它算子传播。 | 不能说微软只监控一个根节点或只提供语句级状态。 |
| “initial estimates from the optimizer are refined using feedback obtained during query execution” （说明书 `p-0089`） | 说明书明确称运行中反馈可修正从优化器取得的各节点估计；并非完全“不做估计修正”。 | 此处的修正是**本次查询进度估计模型**中的数值调整，不等于取消候选后写入下一轮独立优化。 |
| “If the current optimizer estimate for the node is outside the bounds, the optimizer is updated” （说明书 `p-0091`） | 文本甚至用 “optimizer is updated” 描述违反边界后的估计更新，报告应如实承认。 | 不宜仅凭该措辞推断数据库物理计划被重新生成；上下文是进度估计器的边界传播。 |
| “abort execution of a query” （权利要求 42） | 用户输入设备允许中止查询。 | 权利要求 42 属另一项独立系统权利要求；它本身不把权利要求 27 的下界反馈给后续规划。不可机械拼接两项权利要求为同一实施例。 |

**边界判断：** 用户中止入口、算子下界、多节点状态及“优化器估计”修正分别公开；但所核文本的权利要求目标是查询进度显示，没有明示取消现场逐节点基数证据进入下一轮独立候选。

## 5. Sybase US9213740B2：中断候选的部分性能事实

原件：[英文 PDF](../../design/patent-originals/US9213740B2.pdf)；[Google Patents](https://patents.google.com/patent/US9213740B2/en)。

| 原文短引及定位 | 能支持的结论 | 不支持的扩大解释 |
| --- | --- | --- |
| “learning performance for each new query plan generated by recording corresponding query execution metrics” （权利要求 1） | 多候选计划执行指标可用于学习，进而决定未来使用/避开的计划。 | 权利要求 1 没有明说节点基数反馈。 |
| “record the partial “plancost” information even when the execution is aborted” （说明书 `[0130]`） | 被中止候选依然可留下部分性能指标。 | 不能声称取消轮次没有任何可用性能事实。 |
| “only partial elapsed time is needed” （说明书 `[0130]`） | 该实施例用已耗时间与最佳计划比较，就能排除已较慢者。 | 不能将部分耗时当作中断候选的完整运行时间。 |
| “the database administrator may abort the current plan (e.g., because it is running too long)” （说明书 `[0194]`） | 长时间运行后人为中止、试下一个计划的场景已有明确公开。 | 这不是本案预设单轮期限到时自动取消，也没有逐节点行数可信性判定。 |
| “execution is “incomplete” but the system nevertheless tracks the query plan's metrics” （说明书 `[0194]`） | 专利认识到“不完整”执行仍可用于计划级学习。 | 未见把不完整节点行数作为下一轮 Rows 基数修正。 |
| “the interruption is treated as an invalid execution” （说明书 `[0194]`） | 当已耗时尚不能证明比最佳计划慢时，实施例不更新训练对象；它区分可用/无效中断证据。 | 不能宽泛声称 Sybase 无证据筛选；只能说筛选维度是计划级耗时，不是本案节点基数。 |

**边界判断：** “取消/中断候选并从中学习”不能单独主张。相异的证据对象是计划级部分耗时与逐节点基数，且本案仍须把部分耗时视为非完整测量。

## 对现有交底书/报告最重要的纠错

1. **Oracle US11321317B2 不仅公开部分执行统计用于后续查询**（`[0083]`、`[0087]`），还公开运行中统计触发提前终止、换计划（`[0105]`）；不能写成“必须完全执行”或“只能把部分统计用于本轮”。同时要区分 `[0083]` 的相同/等价查询实施例与授权权利要求 1 的非等价第二查询。
2. **Microsoft US7493337B2 的说明书已称运行中反馈修正 optimizer estimate**（`p-0089`、`p-0091`）。准确区别是修正服务当前查询的进度估计，并未见取消后输入下一轮候选；不能写成“微软完全不修正节点估计”。
3. **Microsoft US11334538B2 明确有逐节点估算/实际 profile、执行中事件和后续查询 CE feedback**（`[0024]`、`[0026]`、权利要求 1/5）。不能写成“微软仅监控展示”或“只有语句级状态”。
4. **IBM US7716215B2 已有时间阈值与误差阈值的合取条件**（权利要求 1），并在 `p-0088` 明示所有 QEP 节点统计可用于后续重优化。区别不能简化为“IBM 无时间约束/只看检查点行数”。
5. **Sybase US9213740B2 已让中断轮次成为学习事件**（`[0130]`、`[0194]`）。本案可说“部分耗时不可伪作完整运行时间”，但不能说“中断候选的耗时毫无价值”。

这些短引文支持“拟主张的组合链条在五件单篇公开文本中尚未按本案同一数据依赖链描述”的检索观察；它们不构成新颖性/创造性的最终结论。正式撰写权利要求之前，还应沿各同族和引用链逐项核对。
