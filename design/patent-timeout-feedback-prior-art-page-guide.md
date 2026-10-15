# 限时执行部分反馈专利：三件现有专利的原文页码导读

核验日期：2026-10-15。对比对象为“一种基于限时执行部分反馈的查询执行计划迭代优化方法”。

本导读以本目录保存的英文授权 PDF 为依据，逐页抽取正文并按左右栏核对。表中的 PDF 页码从封面第 1 页起算；正文顶部的数字是印刷栏号，每张正文页通常有两栏。附图的 `Sheet x of y` 是附图序号。三者不能互换，Google Patents 网页的元素编号也不能当作授权 PDF 页码或段落号。

## 1. 可直接放入现有技术材料的页码范围

| 专利 | 与本方案最相关的连续正文范围 | 重点附图 | 授权权利要求 |
| --- | --- | --- | --- |
| IBM US7716215B2，渐进式查询处理 | **PDF Pages 21-25**，印刷栏 5-14；重点是栏 6-13 | PDF Page 5：Fig. 3；PDF Page 9：Fig. 7 | **PDF Pages 27-28**，栏 18-20，权利要求 1 |
| Oracle US11321317B2，在非等价查询中利用查询专属统计 | **PDF Pages 18-20**，栏 11-16；如同时介绍 Adaptive Plan，则扩大为 **Pages 14-21**，栏 3-18 | PDF Pages 3-4：Figs. 1-2；PDF Page 6：Fig. 4 | **PDF Pages 31-32**，栏 37-40，重点为权利要求 1、7、10-12、16 |
| Microsoft US7493337B2，查询进度估计 | **PDF Pages 20-21**，栏 11-14；如同时介绍 GetNext 和进度模型，则扩大为 **Pages 16-21**，栏 3-14 | PDF Page 11：Fig. 10；PDF Page 13：Fig. 12 | **PDF Pages 24-25**，栏 19-22；重点为权利要求 22-33，栏 20-21 |

各正文范围包含首尾页。IBM 的 PDF Pages 5-17 主要是附图，不能用来替代其检查点、EOF 条件和统计反馈机制的正文出处。

## 2. IBM：US7716215B2

英文名称：*System, Method, and Computer Program Product for Progressive Query Processing*。

原文：[本地授权 PDF](patent-originals/US7716215B2.pdf)，共 28 页；[Google Patents](https://patents.google.com/patent/US7716215B2/en)。

| 阅读位置 | 原文标题或短英文锚点 | 与本方案的关联 |
| --- | --- | --- |
| PDF Page 20，栏 3；Page 21，栏 5-6 | `SUMMARY OF THE INVENTION`；`Progressive Query Processing` | 执行过程中比较估算统计与实际统计，判断是否停止当前计划、重新优化并重新执行。Page 21 栏 6 明确检查成功时继续执行，否则终止当前执行并利用部分执行期间得到的统计重新优化。 |
| **PDF Page 21，栏 6** | `a range of cardinalities`；`current execution of the query will be terminated` | CHECK 使用基数区间 `[l, u]`。实际基数处于区间时检查通过；违反检查条件时触发终止和重优化。这里的上下界是检查点允许的基数范围，应与“最终节点行数的上界、下界”区分。 |
| **PDF Page 22，栏 8，续 Page 23，栏 9；PDF Page 9，Fig. 7** | `returns EOF`；`if count > high`；`if count < low and r = EOF` | 上方阈值可在输出过程中被超过；判断输出量不足时，需要读到 EOF。正文给出缓冲检查的 EOF 条件，Fig. 7 给出 CHECK/BUFCHECK 的 open/next/close 伪代码。这是核对“已输出行数为何能证明过低估算，而不能在未结束时证明最终行数不足”的直接原文。 |
| **PDF Page 24，栏 11** | `remainder of the plan ... must depend on the cardinality`；`exist alternative query plans` | 说明检查点的选择条件：上方计划依赖该处基数，且存在替代计划等；不是对每个节点无条件插入检查点。 |
| **PDF Page 24，栏 11-12** | `Exploiting Intermediate Results`；`temporary materialized view`；`exact cardinalities` | 把已形成的中间结果提升为临时物化视图，提供其精确基数；重优化同时比较复用和不复用中间结果的候选。 |
| **PDF Page 24，栏 12 末，续 Page 25，栏 13 开头** | `for all nodes of a QEP, not only for the materialization point`；`fed back into subsequent re-optimization phases` | 明确还可取得执行计划所有节点的基数或更详细统计，并反馈到后续重优化阶段。对比时应同时承认这部分统计反馈描述，不能把 IBM 完全概括为“只复用中间结果”。 |
| PDF Page 26，栏 16 | `Check Condition` | 讨论检查区间的选取，以及连接算法成本交叉点等启发式依据。 |
| PDF Page 27，栏 17 | `Ensuring Termination` | 讨论重复重优化的终止，例如限制尝试次数或要求复用中间结果。这是其迭代控制的补充内容。 |
| **PDF Page 27，栏 18 末，续 Page 28，栏 19-20** | `We claim`；`remaining execution time threshold`；`error threshold` | 授权权利要求 1 包括检查点、估算误差、剩余执行时间阈值、暂停与重优化、中间结果复用以及缓冲/补偿等要素。其 `remaining execution time threshold` 不应直接解释为我方对候选设置的固定单轮已用时间上限。 |

与我方比较时，重点应放在触发方式和反馈处理的组合：IBM 在执行期间由选定检查点的估算偏差及相关条件触发重优化；我方对外部调优过程的每轮候选设置执行期限，在期限结束取消时读取已有节点统计，再按统计适用条件分类、合并为下一轮的基数输入。IBM 已描述部分执行统计以及全节点基数向后续重优化反馈，不能仅凭“使用部分执行统计”或“下一轮使用节点行数”来表述差异。

## 3. Oracle：US11321317B2

英文名称：*Leveraging Query-Specific Statistics in Non-Equivalent Queries*。

原文：[本地授权 PDF](patent-originals/US11321317B2.pdf)，共 32 页；[Google Patents](https://patents.google.com/patent/US11321317B2/en)。

| 阅读位置 | 原文标题或短英文锚点 | 与本方案的关联 |
| --- | --- | --- |
| PDF Pages 14-15，栏 3-5；PDF Pages 3-4，Figs. 1-2 | `Adaptive Plan Execution Overview`；`Statistics Collector` | 预先准备含多个子计划的自适应计划；统计收集器根据已读取的行数等信息选择后续子计划。Page 14 栏 4 明确统计可能只来自已读取的部分表数据。 |
| **PDF Page 18，栏 11-12，续 Page 19，栏 13；PDF Page 6，Fig. 4** | `Automatic Reoptimization`；`fully executed or partially executed` | 明确执行期间收集的信息可改善同一或等价查询的后续执行，而且第一次执行可以完整结束，也可以仅部分执行。统计包括表、连接、Group By 等基数。应在对比中保留这项直接相关内容。 |
| **PDF Page 18，栏 11-12** | `after each operator`；`subsequent query that is the same as or equivalent` | 统计收集器可置于多个算子后；实际成本与估算成本差异达到条件时，将游标标记为后续重优化，后续计划成本可利用先前运行统计。 |
| **PDF Page 19，栏 14 后半，续 Page 20，栏 15 顶部** | `In-Flight Reoptimization`；`10,000 rows when only 100 rows were estimated` | 在整个计划执行完之前，因实际统计和编译期估算显著不同而停止，重新优化；还可结合剩余操作及其成本判断是否切换到其他计划。此节标题和主要论述在 Page 19，不能仅标 Page 20。 |
| **PDF Page 20，栏 15-16；相关状态续 Page 21，栏 17-18** | `Statistics`；`Plan Directives` | 把部分具有价值的运行统计持久化，按数据库对象和操作标识保存；后续非等价查询也可检索并使用相关统计。计划指令及其状态是统计保存和后续利用的机制。 |
| **PDF Page 31，栏 37-38，权利要求 1；Page 32，栏 39-40，权利要求 12** | `second query that is not equivalent to the first query` | 授权独立权利要求针对选择保存某个操作的统计，并据数据库对象与操作识别其是否适用于后续非等价查询，再据此生成第二执行计划。说明书还包括自适应及运行中重优化等内容，应与该件授权权利要求分别表述。 |
| PDF Page 31，栏 38，权利要求 7；Page 32，栏 39，权利要求 10-11 | `particular join or a particular predicate`；`first number ... read`；`second number ... resulted` | 涉及统计匹配，以及操作已读取数据项数和操作产生数据项数；第一计划不能为第二查询产生有效结果的情形也在权利要求中明确记载。 |
| PDF Page 32，栏 40，权利要求 16 | `amount of time to devote to processing dynamic statistics` | 使用执行时间数据决定动态统计处理的时间投入；这里描述的是动态统计处理时间，并非直接给出我方 `min(T0, 1.25 × Tbest)` 的候选执行期限规则。 |

与我方比较时，不能只说 Oracle 面向“一次查询内切换”或“不同查询复用”。其 Automatic Reoptimization 已明确涉及同一或等价查询，以及首次仅部分执行后的统计反馈。我方应具体说明单轮执行期限、取消前取得的节点状态、可反馈精确值/下界的条件，以及多轮基数合并规则如何共同参与外部候选调优。

## 4. Microsoft：US7493337B2

英文名称：*Query Progress Estimation*。

原文：[本地授权 PDF](patent-originals/US7493337B2.pdf)，共 25 页；[Google Patents](https://patents.google.com/patent/US7493337B2/en)。

| 阅读位置 | 原文标题或短英文锚点 | 与本方案的关联 |
| --- | --- | --- |
| PDF Page 16，栏 4；PDF Page 5，Fig. 3 | `Model of Work`；`GetNext() calls` | 以迭代器模型及 GetNext 调用/算子返回的数据项作为查询工作量模型。与我方算子统计的数据来源相关，但用途为估计当前查询进度。 |
| **PDF Page 20，栏 11-12；PDF Page 11，Fig. 10** | `Initializing and Refining Driver Node Cardinalities` | 先用优化器估算初始化，再利用运行期间反馈修正基数。明确只在能够确认估算会更准确时修正，以避免引入新的误差。 |
| **PDF Page 20，栏 12；PDF Page 13，Fig. 12** | `Refining Upper and Lower Bounds`；`current estimate ... lies outside the bounds` | 维护已输出行数，以及节点最终输出基数的上下界；估算越界时修正到范围内，范围内则保持；界限变化可沿执行计划向上传播。 |
| **PDF Page 21，栏 13，Table 1** | `Ki is the actual number of rows output ... thus far` | 列出 Filter、Group By、Sort、Nested Loop、Hash Join 等算子的上下界规则。Filter 下界为已输出行数，上界为前序节点剩余可能输入量加当前已输出量；Hash Join 等规则使用算子特定信息。 |
| **PDF Page 21，栏 13 底部** | `Whenever an operator terminates, the upper and lower bounds ... are known exactly` | 明确算子终止时其上下界成为已知精确值。这与我方区分完整行数和未完成时的行数下界直接相关。该语句未定义我方实现中的 `active`/`terminal` 字段含义。 |
| PDF Pages 21-22，栏 14-15 | `Monotonicity of Progress Bar` | 解释怎样保持进度条不倒退，以及采用上界和估计精度之间的关系，体现这些基数估计所服务的最终用途。 |
| **PDF Page 24，栏 20，续 Page 25，栏 21，权利要求 22-33** | `maintaining an upper bound and a lower bound`；`setting the lower bound to a number of items returned` | 权利要求 22 和 23 起于 Page 24，23 续至 Page 25；相关权利要求涵盖利用上下界修正总工作量或算子输出量估算、算子特定规则、以已返回数据项为下界，以及上下界向其他算子传播。权利要求 27 对下界的描述尤为直接。 |

与我方比较时，Microsoft 的直接用途是估计并显示当前查询进度；我方把符合条件的节点行数用于后续候选的基数输入与计划生成。不过，已输出行数可构成下界、完成后取得精确值，以及只在估算越界时修正的思想，在其正文中已有直接描述。

## 5. 引用与阅读建议

若 PPT 只允许给每件专利写一组页码，建议使用：

- IBM：**Pages 21-25**。若包含 PPT 中“启发式确定检查区间”的内容，则标为 **Pages 21-26**；在讲 EOF 时另加 **Page 9, Fig. 7**。
- Oracle：**Pages 18-20**。在讲授权主题“非等价查询复用统计”时另加 **Pages 31-32, Claims 1 and 12**；在讲 Adaptive Plan 时补 **Pages 14-15**。
- Microsoft：**Pages 20-21**。在讲上下界更新流程时另加 **Page 13, Fig. 12**；在核对权利要求时加 **Pages 24-25, Claims 22-33**。

在交底书现有技术部分，宜同时写出相关机制所在的说明书位置和对应权利要求位置。前者用于准确描述公开的技术内容，后者用于核对该件授权文本实际记载的权利要求。

本次页码来自授权 PDF 原文；未将既有检索笔记的网页元素编号（例如 `p-0084`、`p-0106`）转换或标注为 PDF 段落号。

## English quick reference

Page numbers refer to the granted patent PDFs, counting the cover as page 1. The printed numbers above the two text columns are column numbers, not PDF page numbers. The most relevant description ranges are IBM US7716215B2, pages 21-25; Oracle US11321317B2, pages 18-20; and Microsoft US7493337B2, pages 20-21. Relevant claims are on pages 27-28, 31-32, and 24-25, respectively. IBM Figure 7 is on PDF page 9; Oracle Figure 4 is on page 6; Microsoft Figure 12 is on page 13. Oracle's adaptive-plan discussion additionally appears on pages 14-15.
