# 超时行数反馈：专利原文复核与拟保护点调整

复核日期：2026-10-12。本文针对目前 PPT 中的 IBM、Oracle、Microsoft 三项现有技术，以及更直接涉及基数反馈的 Microsoft `US11334538B2`，核对公开原文中的相关权利要求和说明书。沿用仓库既有检索线索，不重新开展泛化景观检索。以下区分原文事实、技术比较和候选保护建议；“所核文本未见”不等于已经证明新颖性或创造性。

## 一、最需要修改的比较结论

**同一 SQL、部分执行统计、实际行数反馈、行数下界、外部反馈服务和 hint，分别已有公开。** 因此，不宜再把“同一 SQL 的下一轮迭代”单独作为区别，也不能把 Oracle 概括为仅在一次执行内部换计划，或只把统计交给其他 SQL。三种反馈标签能帮助解释方案，但标签本身不是技术区别。

拟保护组合集中于：**候选期限触发取消；清理前保存节点估算、已返回行数和执行状态；按行数适用条件分类；按反馈类型跨轮合并；修正下一轮独立候选的基数输入。** 原计划重建与匹配用于当前实现防止统计错配，放入实施说明，不列入拟保护点。

## 二、逐项原文核对

### 1. IBM：US7716215B2

标题核对：*System, method, and computer program product for progressive query processing*。原申请人为 IBM。授权文本只有一项权利要求；权利要求 1 还包括物化位置、剩余执行时间及误差阈值、暂停与重优化、可选择复用中间结果、管线结果补偿等限制，不能将说明书某个 CHECK 分支当成该项权利要求的完整范围。[授权全文与权利要求 1](https://patents.google.com/patent/US7716215B2/en#C1)

**原文事实：** 说明书“Progressive Query Processing”明确使用部分执行取得的统计来重优化当前查询，短引文为“partial query execution”。“Eager Check with Buffering”、图 7 的 `CHECKNEXT` 明确：`count > high` 可以在 EOF 前触发；`count < low` 需结合 EOF。图 7 位于原 PDF 第 9 页。中间结果复用由优化器权衡，不应概括为每次都必须复用临时视图。[说明书及图 7](https://patents.google.com/patent/US7716215B2/en#description)；[原件 PDF](https://patentimages.storage.googleapis.com/b1/07/18/edc8bbe72b0545/US7716215.pdf)

**PPT 调整：** 承认“输出前缀可以证明低估，低输出需等结束才能证明高估”已经公开。比较集中于本案的执行期限取消、节点行数适用判定与分级合并，以及对下一轮独立试运行的反馈；不能只以 `A > E` 或 EOF 为区别。

### 2. Oracle：US11321317B2

标题核对：*Leveraging query-specific statistics in non-equivalent queries*。权利要求 1 针对单个操作统计的保存，以数据库对象及操作判定统计对第二条非等价查询是否相关，再据此生成执行计划；权利要求 2、3 分别涉及过滤和连接。[权利要求 1–3](https://patents.google.com/patent/US11321317B2/en#C1)

**原文事实：** “Automatic Reoptimization”首段明确首次查询可“fully executed or partially executed”，统计用于后续“same or equivalent”查询。随后列出表、连接、分组等基数，并说明第二次规划可用前次操作统计。该段 HTML 定位为 `p-0084`。“In-Flight Reoptimization”另说明运行统计偏离估算时可以停止未完成计划、产生新的执行计划；HTML 定位 `p-0106`。[Automatic Reoptimization 与 In-Flight Reoptimization](https://patents.google.com/patent/US11321317B2/en#description)

**PPT 调整：** 必须同时写明执行中换计划、部分或完整执行后的同一查询重优化、非等价查询的统计复用。比较应进一步具体到执行期限触发的取消、取消现场行数的适用判定，以及跨轮证据合并。

### 3. Microsoft：US7493337B2

标题核对：*Query progress estimation*。权利要求 22、23 用上下界修正算子预计返回量；从属权利要求 27 明确将当前返回量作为下界，短引文为“setting the lower bound to a number of items returned”。权利要求 33 将多个算子的边界变化传播至其他算子；权利要求 34、42 涉及允许用户中止查询的界面。[权利要求 22–33、34、42](https://patents.google.com/patent/US7493337B2/en#claims)

**原文事实：** “Refining Cardinality Estimates”、图 12 明确在估算落在界外时修正；表 1 后写有“Whenever an operator terminates”，该算子的上下界此时精确已知。所以，它不只是显示一个静态进度值。[说明书图 12 与表 1](https://patents.google.com/patent/US7493337B2/en#description)

**PPT 调整：** 承认下界和估算修正机制；主要用途区别是当前查询的进度模型，而本案使用取消后经过校验的行数生成下一轮候选基数输入。不同权利要求链中分别出现下界和用户中止，不能直接说一项权利要求已经规定中止后的下一轮反馈；但这些公开仍构成组合分析的重要材料。

### 4. 必须补入更接近的 Microsoft：US11334538B2

标题核对：*System and method for cardinality estimation feedback loops in query processing*。权利要求 1 已有运行统计、实际与估算基数比较、反馈建议，以及基数振荡时临时停用反馈；权利要求 5 明确建议改变同一查询的后续执行及相似查询。[权利要求 1、5](https://patents.google.com/patent/US11334538B2/en#C1)

**原文事实：** 说明书 `[0024]` 明确事件既在运行中也在结束时产生，短引文“during and at the completion”；反馈优化器可以单独部署。`[0026]` 明确计划每个节点的估算和实际基数，反馈形式可以是“additional query hints”。`[0028]` 的结束后提交是其中一种数据流，不能据此概括全文只能正常结束后收集。HTML 对应 ID 分别为 `p-0025`、`p-0027`、`p-0029`；不要将 HTML ID 误当正式段落号。[说明书 `[0024]`–`[0028]`](https://patents.google.com/patent/US11334538B2/en#description)

**PPT 调整：** 它比纯进度估计更直接涉及反馈优化，建议作为现有技术主对照。独立外部服务、同一 SQL、节点实际基数和 hint 输出均不能单独区分它；应比较本案在期限取消后对行数的适用判定、更新方向及跨轮合并流程。

### 5. 与候选耗时比较有关的 Sybase 补充

`US9213740B2` 的“Aborted Query Execution and Auto Query Tuning Mode”、图 5 已说明中止后保留部分计划耗时：如果中止前已耗时超过既有最佳计划，仍可判定为较差计划；未超过时不更新训练对象。本文不把它扩展解释为节点行数反馈。[原文](https://patents.google.com/patent/US9213740B2/en#description)

另一个“Search Engine Timeout”例子按既有执行时间设置的是**优化器搜索时限**，不是候选查询的执行期限。不可据此直接认定公开了本案的 `min(T₀, 1.25 × T_best)`。但“利用既有最佳运行时间约束调优资源”已存在相关启示，固定数值 1.25 不宜承担主保护点。

## 三、候选保护组合

以下是技术要素组织建议，不是已完成审查的法律权利要求。计划重建及匹配是当前实现的防护措施，保留在实施说明中，不列入拟保护点。

1. **限时取消时保存节点事实。** 对候选计划限时执行；期限触发取消、执行状态对象仍有效时，取得节点估算、已返回行数及执行状态。
2. **判定行数的适用范围。** 检查该扫描或连接是否已经返回全部相关结果，实际与估算是否覆盖相同执行次数；区分精确值、可用下界或无反馈。
3. **按反馈类型合并。** 精确值允许上调或下调；未完成但合格的 A > E 下界只抬高；其他观测保持旧输入。精确证据优先，下界单独保留并累计取最大值。
4. **用于下一轮独立候选。** 合并结果修正同一 SQL 后续候选的基数输入。正常完成与超时取消均可提供行数，只有完整运行时间更新已测最佳候选及自适应期限。

保护组合的重点是期限触发采集后，如何将不同执行状态下的行数限定为不同强度的反馈，并据此约束下一轮更新方向和跨轮合并。当前恢复计划及整轮匹配步骤避免统计错配，但节点反馈也可以使用直接获得的原计划结构，不能把必须重新规划或匹配两份计划作为该一般机制的前提。

IBM、Oracle 和 Microsoft 已分别公开部分执行统计、后续查询反馈及行数下界。去掉具体计划恢复限制后，应进一步用可执行的行数适用条件、精确值与下界的合并规则说明组合的技术作用；不能把同一 SQL、简单 A > E 或标签名称视为已证明的主要区别。

## 四、不能把拟议加强写成已实现

当前工作树的 [`partial_observation_evidence`](../contrib/hint_adviser/hint_adviser_partial_proof.c#L14) 已确认分类条件是 `TERMINAL && rescan_count == 0 → EXACT`，其他情况下 `A > E → LOWER_BOUND`。这只能直接证明当前代码如何分类，**不能自动证明一个节点的全部相关输出都已结束**：Nested Loop 内侧可能在首次 EOF 后再次执行；多次执行累计返回量也可能与单次估算覆盖不同计数范围。

例如单次估算 `E = 100`，两次调用各返回 80 行，累计 `A = 160`，直接比较累计 `A > E` 会误判低估。若要把“所有节点均可严格判定精确值或下界”纳入保护和效果，应先补上限定、分次计数或拒绝反馈规则，并完成代码与实测支撑。当前已有取消采集、状态分类和证据合并，但严格行数判定仍需补充上述条件；计划恢复与匹配不代替这些判定，也不作为拟保护要素。

建议支撑材料包括本地 EOF 后仍需再次调用、累计计数与单次估算不一致，以及旧采样值大于合格下界时不降低规划输入的对照。它们说明行数判定及分级合并的作用。重规划结构变化时拒绝本轮统计，可单独说明当前实现的安全防护。

## 五、保护层级建议

| 层级 | 推荐内容 | 不宜承担主要区别的内容 |
| --- | --- | --- |
| 主组合 | 限时取消 → 清理前取得节点事实 → 行数适用判定 → 分级合并 → 下一轮基数修正 | 同一 SQL、实际行数反馈、简单 `A > E`、三个标签 |
| 重要从属 | 再次调用和计数范围检查；精确证据优先；合格下界单独累计；无合格证据保持旧输入；部分耗时与完整耗时分开 | 仅改变量名、状态枚举名或图中文字 |
| 配套从属 | 单轮上限与最佳完整耗时按倍率缩短，受全局剩余预算约束 | 固定 10 秒、固定 1.25 |
| 实施说明 | `QueryDesc` / `PlanState` / `Instrumentation`、PRETTY / JSON、NOTICE、重新规划与计划匹配、连接恢复、Rows Hint | 将这些实现方式作为一般反馈方法的必要条件 |

拟保护标题可围绕“限时执行中的节点行数判定与跨轮反馈”。数值倍率用一般参数描述、以 1.25 举例。重新规划匹配保留为当前代码的安全防护说明，不列入本次拟保护组合。

## English abstract

The reviewed primary patents already disclose partial-execution statistics, later executions of the same query, operator-output lower bounds, external feedback services, and hint-based cardinality corrections. The revised candidate combination centers on deadline-triggered cancellation, capture before executor cleanup, qualification of counts by execution state and counting range, and evidence-aware merging into the next independent candidate's cardinality inputs. Non-executing replanning and whole-plan matching remain implementation safeguards and are excluded from the proposed protection elements. The current implementation must not be described as proving that local EOF rules out future calls or that cumulative counts always match per-invocation estimates. The concrete qualification and update rules still require assessment against existing feedback techniques and further prior art.
