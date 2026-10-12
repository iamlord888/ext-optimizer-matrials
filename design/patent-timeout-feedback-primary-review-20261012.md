# 超时行数反馈：专利原文复核与拟保护点调整

复核日期：2026-10-12。本文针对目前 PPT 中的 IBM、Oracle、Microsoft 三项现有技术，以及更直接涉及基数反馈的 Microsoft `US11334538B2`，核对公开原文中的相关权利要求和说明书。沿用仓库既有检索线索，不重新开展泛化景观检索。以下区分原文事实、技术比较和候选保护建议；“所核文本未见”不等于已经证明新颖性或创造性。

## 一、最需要修改的比较结论

**同一 SQL、部分执行统计、实际行数反馈、行数下界、外部反馈服务和 hint，分别已有公开。** 因此，不宜再把“同一 SQL 的下一轮迭代”单独作为区别，也不能把 Oracle 概括为仅在一次执行内部换计划，或只把统计交给其他 SQL。三种反馈标签能帮助解释方案，但标签本身不是技术区别。

目前更值得组织成主保护组合的是：**取消时保存原执行计划和节点统计；取消结束后只重新规划相同候选 SQL；以原计划为基准核对整个重新生成的计划结构；全部匹配后才把统计写入对应节点；随后分类、合并并用于下一轮候选。** 这条恢复与校验路径要与“为何不能直接使用取消计数”一起说明，而不是仅罗列采集、超时和反馈三个功能。

## 二、逐项原文核对

### 1. IBM：US7716215B2

标题核对：*System, method, and computer program product for progressive query processing*。原申请人为 IBM。授权文本只有一项权利要求；权利要求 1 还包括物化位置、剩余执行时间及误差阈值、暂停与重优化、可选择复用中间结果、管线结果补偿等限制，不能将说明书某个 CHECK 分支当成该项权利要求的完整范围。[授权全文与权利要求 1](https://patents.google.com/patent/US7716215B2/en#C1)

**原文事实：** 说明书“Progressive Query Processing”明确使用部分执行取得的统计来重优化当前查询，短引文为“partial query execution”。“Eager Check with Buffering”、图 7 的 `CHECKNEXT` 明确：`count > high` 可以在 EOF 前触发；`count < low` 需结合 EOF。图 7 位于原 PDF 第 9 页。中间结果复用由优化器权衡，不应概括为每次都必须复用临时视图。[说明书及图 7](https://patents.google.com/patent/US7716215B2/en#description)；[原件 PDF](https://patentimages.storage.googleapis.com/b1/07/18/edc8bbe72b0545/US7716215.pdf)

**PPT 调整：** 承认“输出前缀可以证明低估，低输出需等结束才能证明高估”已经公开。区别改为本案的执行期限取消、取消后原计划恢复校验，以及对下一轮独立试运行的反馈；不能只以 `A > E` 或 EOF 为区别。

### 2. Oracle：US11321317B2

标题核对：*Leveraging query-specific statistics in non-equivalent queries*。权利要求 1 针对单个操作统计的保存，以数据库对象及操作判定统计对第二条非等价查询是否相关，再据此生成执行计划；权利要求 2、3 分别涉及过滤和连接。[权利要求 1–3](https://patents.google.com/patent/US11321317B2/en#C1)

**原文事实：** “Automatic Reoptimization”首段明确首次查询可“fully executed or partially executed”，统计用于后续“same or equivalent”查询。随后列出表、连接、分组等基数，并说明第二次规划可用前次操作统计。该段 HTML 定位为 `p-0084`。“In-Flight Reoptimization”另说明运行统计偏离估算时可以停止未完成计划、产生新的执行计划；HTML 定位 `p-0106`。[Automatic Reoptimization 与 In-Flight Reoptimization](https://patents.google.com/patent/US11321317B2/en#description)

**PPT 调整：** 必须同时写明执行中换计划、部分或完整执行后的同一查询重优化、非等价查询的统计复用。差异只能进一步具体到取消现场统计的安全解释、原计划与重规划结构的一致性校验，以及跨轮证据合并。

### 3. Microsoft：US7493337B2

标题核对：*Query progress estimation*。权利要求 22、23 用上下界修正算子预计返回量；从属权利要求 27 明确将当前返回量作为下界，短引文为“setting the lower bound to a number of items returned”。权利要求 33 将多个算子的边界变化传播至其他算子；权利要求 34、42 涉及允许用户中止查询的界面。[权利要求 22–33、34、42](https://patents.google.com/patent/US7493337B2/en#claims)

**原文事实：** “Refining Cardinality Estimates”、图 12 明确在估算落在界外时修正；表 1 后写有“Whenever an operator terminates”，该算子的上下界此时精确已知。所以，它不只是显示一个静态进度值。[说明书图 12 与表 1](https://patents.google.com/patent/US7493337B2/en#description)

**PPT 调整：** 承认下界和估算修正机制；主要用途区别是当前查询的进度模型，而本案使用取消后经过校验的行数生成下一轮候选基数输入。不同权利要求链中分别出现下界和用户中止，不能直接说一项权利要求已经规定中止后的下一轮反馈；但这些公开仍构成组合分析的重要材料。

### 4. 必须补入更接近的 Microsoft：US11334538B2

标题核对：*System and method for cardinality estimation feedback loops in query processing*。权利要求 1 已有运行统计、实际与估算基数比较、反馈建议，以及基数振荡时临时停用反馈；权利要求 5 明确建议改变同一查询的后续执行及相似查询。[权利要求 1、5](https://patents.google.com/patent/US11334538B2/en#C1)

**原文事实：** 说明书 `[0024]` 明确事件既在运行中也在结束时产生，短引文“during and at the completion”；反馈优化器可以单独部署。`[0026]` 明确计划每个节点的估算和实际基数，反馈形式可以是“additional query hints”。`[0028]` 的结束后提交是其中一种数据流，不能据此概括全文只能正常结束后收集。HTML 对应 ID 分别为 `p-0025`、`p-0027`、`p-0029`；不要将 HTML ID 误当正式段落号。[说明书 `[0024]`–`[0028]`](https://patents.google.com/patent/US11334538B2/en#description)

**PPT 调整：** 它比纯进度估计更直接涉及反馈优化，建议作为现有技术主对照。独立外部服务、同一 SQL、节点实际基数和 hint 输出均不能单独区分它；应突出本案面对取消异常时的恢复、校验、分类与合并流程。

### 5. 与候选耗时比较有关的 Sybase 补充

`US9213740B2` 的“Aborted Query Execution and Auto Query Tuning Mode”、图 5 已说明中止后保留部分计划耗时：如果中止前已耗时超过既有最佳计划，仍可判定为较差计划；未超过时不更新训练对象。本文不把它扩展解释为节点行数反馈。[原文](https://patents.google.com/patent/US9213740B2/en#description)

另一个“Search Engine Timeout”例子按既有执行时间设置的是**优化器搜索时限**，不是候选查询的执行期限。不可据此直接认定公开了本案的 `min(T₀, 1.25 × T_best)`。但“利用既有最佳运行时间约束调优资源”已存在相关启示，固定数值 1.25 不宜承担主保护点。

## 三、当前最有价值的候选保护组合

以下是技术要素组织建议，不是已完成审查的法律权利要求。

1. **取消现场保存执行事实。** 对候选计划限时执行；取消异常发生、执行状态对象仍有效时，取得原执行计划、节点身份、已返回行数及执行状态。保留取消结果的错误语义，避免把这次试运行表述为成功的完整执行。
2. **不再次执行查询地恢复计划结构。** 取消结束后，对相同候选 SQL 及其规划输入运行不带 `ANALYZE` 的 `EXPLAIN`，取得后续处理需要的计划结构。这里的 SQL 是被取消的本轮候选，不是已经加入新反馈的下一轮 SQL。
3. **先核对整轮原计划，再绑定统计。** 以取消现场的原执行计划为依据，检查重规划所得计划的节点 ID、算子类型、查询块、按文本显示精度核对的估算行数及父子层次。全部一致才将取消统计绑定至对应节点；结构冲突时拒绝本轮统计，不把碰巧相同的节点 ID 视为足够证明。
4. **分类与合并形成下轮基数。** 完整且适用的实际行数可以替换旧估算；适用的部分输出只形成下界；其他观测不更新。下界证据要单独保留，不能与当前用于规划的值混成一个变量；精确证据与较弱下界的合并优先级应明确。
5. **分别维护行数输入和完整耗时。** 正常完成和超时取消都可能提供合格行数；只有完整执行时间更新已测最佳候选及自适应期限。已耗时超过阈值仍可用于判断候选没有达到改善目标，不能拿它当最终完整耗时。

这条链条与已核原文的区别，主要是**从取消前原计划到取消后恢复的计划结构之间，经过整轮校验才允许将统计发布为可消费反馈**。IBM 的 CHECK 在已知计划内触发重编译；Oracle 按对象与操作复用统计；Microsoft CE 使用带计划节点 profile 的反馈服务。所核文本尚未明确披露上述取消后恢复及整轮校验组合。另一方面，结构匹配与取消错误处理可能被视为常规实现手段，必须通过真实失败场景及处理效果说明其技术必要性。

## 四、不能把拟议加强写成已实现

当前工作树的 [`partial_observation_evidence`](../contrib/hint_adviser/hint_adviser_partial_proof.c#L14) 已确认分类条件是 `TERMINAL && rescan_count == 0 → EXACT`，其他情况下 `A > E → LOWER_BOUND`。这只能直接证明当前代码如何分类，**不能自动证明一个节点的全部相关输出都已结束**：Nested Loop 内侧可能在首次 EOF 后再次执行；多次执行累计返回量也可能与单次估算覆盖不同计数范围。

例如单次估算 `E = 100`，两次调用各返回 80 行，累计 `A = 160`，直接比较累计 `A > E` 会误判低估。若要把“所有节点均可严格判定精确值或下界”纳入保护和效果，应先补上限定、分次计数或拒绝反馈规则，并完成代码与实测支撑。当前主保护组合可以围绕已实现的取消采集、计划恢复、全结构校验和证据状态合并，不必强行依赖尚未实现的未来调用证明。

建议支撑材料包含三个对照：取消时本地 EOF 后仍可能再次调用的节点；同一 SQL 重规划后结构变化导致整轮反馈被拒绝；旧采样值大于本轮下界时仅保存下界、不降低规划输入。它们能分别说明分类边界、原计划校验和双状态合并的作用。

## 五、保护层级建议

| 层级 | 推荐内容 | 不宜承担主要区别的内容 |
| --- | --- | --- |
| 主组合 | 取消前原计划及节点事实保存 → 取消后只规划恢复 → 整轮结构校验后绑定 → 分类合并 → 下一轮基数修正 | 同一 SQL、实际行数反馈、简单 `A > E`、三个标签 |
| 重要从属 | 精确证据优先；下界单独累计；无合格证据保持旧输入；整轮匹配失败拒绝统计；部分耗时与完整耗时分开 | 仅改变量名、状态枚举名或图中文字 |
| 配套从属 | 单轮上限与最佳完整耗时按倍率缩短；完整消息及执行标识；保留取消错误 | 固定 10 秒、固定 1.25、固定消息长度 |
| 实施例 | `QueryDesc` / `PlanState` / `Instrumentation`、具体状态字段、PRETTY / JSON、NOTICE 传输、Rows Hint | 将主保护范围限于某个产品、指针或消息格式 |

技术方案标题可围绕“限时试运行中的节点行数恢复与反馈”，拟保护点可围绕“取消后的原计划校验与行数反馈更新方法”。数值倍率用一般参数描述、以 1.25 举例，比把 1.25 写成发明本质更合适。

## English abstract

The reviewed primary patents already disclose partial-execution statistics, later executions of the same query, operator-output lower bounds, external feedback services, and hint-based cardinality corrections. These individual elements cannot establish a distinct technical contribution. A more concrete candidate combination is cancellation-time capture of the original plan and operator observations, non-executing reconstruction of the same candidate's plan, whole-plan consistency validation before attaching observations, and evidence-aware merging into the next candidate's cardinality inputs. The current implementation must not be described as proving that local EOF rules out future calls or that cumulative counts always match per-invocation estimates. Novelty and inventive step of the proposed combination remain to be examined against the full patent families and additional prior art.
