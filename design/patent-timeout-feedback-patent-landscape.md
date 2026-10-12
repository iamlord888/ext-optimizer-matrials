# 超时执行反馈方案的公开专利检索笔记

检索日期：2026-09-29。本文是交底书的技术检索底稿，不是新颖性、创造性或自由实施的法律意见。比较对象是：**对真实候选 SQL 设置单轮期限；超时取消时保留执行计划节点的累计输出与执行状态；判定同一基数目标的完整值、可用下界或无反馈；把合格行数送入下一轮独立候选计划的基数修正**。下文的“未见”只表示所列公开文本中未核到该步骤，不能解释为行业内不存在。

## 检索与筛选

主要在 Google Patents 检索页查询公开文献，以其链接的美国授权/公开 PDF 原文核对权利要求、说明书和附图；对中文公开文本查看原始公开号及 Google Patents 的中文/英文页面。美国 PDF 是公开文本的影印件，页码、栏号和权利要求号可用于复核。Google Patents 的法律状态与同族列表只是检索线索，未向 USPTO、CNIPA、EPO 和 WIPO 的法律状态登记簿逐件复核。

检索式按问题递进，而非只检索方案名称：

| 检索方向 | 实际使用的代表性检索式 | 筛选结果 |
| --- | --- | --- |
| 限时/中断后的执行反馈 | `"query execution" "timeout" "optimizer"`、`"incomplete execution" "query plan"`、`"timed out query" optimizer cardinality`、`"aborted query" "cardinality" "optimizer"` | 找到 Sybase 对人工中断候选的性能学习和 Oracle 的失控查询终止；精确短语零结果不证明不存在。 |
| 部分执行与重优化 | `"partial query execution" optimizer`、`"query optimization" "partial results" "cardinality"`、`"partial execution" "row count" "query"` | 找到 IBM 渐进式查询处理及联邦查询重优化、IBM pilot-run、Oracle 限时近似结果。 |
| 算子完成、下界及反馈 | `"lower bound" cardinality reoptimization`、`"operator progress" "query plan" "feedback"`、`"end of scan" "query" "optimizer"`、`"query" "operator" "lower bound" "re-optimization"` | 找到微软算子输出下界/进度专利；未检到“取消时对每个节点作全局完成性证明”的明示权利要求。 |
| 中文与同族 | `"超时" "执行计划" "基数"`、`"超时" "基数反馈"`、`"超时返回" "执行计划"`、`"EXPLAIN ANALYZE" "timeout"`；按公开号追查中、英页面 | 重点核查九有数据库的自动优化、华为执行计划选择，以及腾讯数据库性能分析；后两项的检索词命中并不等于机制相关。 |
| 申请人和引用链 | IBM、Oracle、Microsoft、Sybase、Teradata、Amazon、Snowflake、Alibaba、Huawei；从 IBM US7716215B2、Oracle US11321317B2、Microsoft US11334538B2 的同族及前后引证继续筛选 | 发现微软 2004 年表达式基数反馈、微软 2020 年 CE feedback，以及 Oracle 部分执行供后续查询的公开说明。 |

筛选规则：先看摘要与独立权利要求；凡可能覆盖任一核心环节，再查说明书对应位置。排除把数据库**计算/存储节点**误当执行计划**算子节点**、把“优化器搜索超时”误当“候选 SQL 执行超时”、把对用户返回部分**查询结果**误当部分**执行计划反馈**。下表的 A-D 分别是：A 超时或取消后保留反馈；B 算子/计划节点级实际行数；C 对完整、未完整/下界、未启动或调用口径进行区分；D 用于后续独立候选计划的基数修正。`△` 表示只覆盖近似环节或说明书实施例，不表示满足本方案全链条。

## 重点文献对照

| 公开文本及风险 | A | B | C | D | 关键差别与核查位置 |
| --- | :---: | :---: | :---: | :---: | --- |
| IBM [US7716215B2](https://patents.google.com/patent/US7716215B2/en)，**最高** | △ | 是 | △ | △ | 权利要求1：执行中 CHECK 比较估算/实际基数，在时间及误差阈值下 suspend、re-optimize、restart。图7明确 `count > high` 可在 EOF 前触发，`count < low` 需 EOF。对象是**同一查询中途重优化**，不是按外部期限取消后启动下一轮独立候选。 |
| Oracle [US11321317B2](https://patents.google.com/patent/US11321317B2/en)，**最高** | △ | 是 | 未见 | 是 | 说明书第12-15栏称首次查询可以完全或部分执行，并举表/连接等基数供随后查询优化；权利要求1按单个操作保存统计，匹配第二条非等价查询后生成第二计划。没有核到外部单轮超时、取消现场的逐节点完成/下界分类。 |
| Microsoft [US11334538B2](https://patents.google.com/patent/US11334538B2/en)，**高** | 未见 | 是 | 未见 | 是 | 权利要求1：运行时 event signal 提取 actual cardinality，按与估算差异发出反馈建议；说明书称监测信号在执行中及结束时产生，供以后相同/相似查询。全文未核到取消后的节点证据分级。 |
| Microsoft [US7493337B2](https://patents.google.com/patent/US7493337B2/en)，**高** | △ | 是 | △ | 否 | 权利要求22、23、27、33：算子输出维护上下界，当前已返回条数作为最终输出下界，多算子可传播；权利要求42含用户中止查询。用途是**进度估计与展示**，未核到中止后把下界写入下一轮优化器。 |
| Sybase [US9213740B2](https://patents.google.com/patent/US9213740B2/en)，**高** | 是 | 未见 | △ | △ | 说明书图5及第25-26栏：DBA 因运行过久中断新计划，若中断时耗时已超过已知最佳计划，就保存该计划的性能指标并判为坏计划；权利要求1、21据计划级指标决定以后用哪个计划。未核到取消节点行数或 Rows 基数修正。 |
| IBM [US7877381B2](https://patents.google.com/patent/US7877381B2/en)，**中高** | △ | 是 | △ | △ | 权利要求1：联邦查询部分执行、检查点实际基数越界、重编译并反复重执行直至出结果。它处理的是本次联邦查询的连续重优化及远端统计完整性，不是期限到达后的独立候选反馈。 |
| IBM [US9836506B2](https://patents.google.com/patent/US9836506B2/en)，**中** | △ | △ | 未见 | △ | 权利要求1：对选定 UDF 作至少部分 pilot-run，收集统计并在本次查询执行中改进谓词顺序、基数与代价估算。其观测是选定子表达式的试运行，而非被取消的完整候选物理计划。 |
| Oracle [US11556539B2](https://patents.google.com/patent/US11556539B2/en)，**中** | 是 | 未见 | 未见 | 未见 | 权利要求1：按计划组件预测资源消耗，运行中监测性能并在阈值不满足时 halt runaway query；未核到取消后的实际行数反馈。 |
| CN [CN117271481B](https://patents.google.com/patent/CN117271481B/zh)，**中，译文待复核** | △ | 未核实 | 未见 | △ | 权利要求1涉及从执行数据优化计划，以及当前计划性能低于基线时满足执行终止条件并选历史计划；说明书另有**优化器优化时间**参数。不能据此认定公开了 SQL 执行期限或节点完成判定。中文授权 PDF 文字层不能可靠抽取，本项目前按 Google Patents 译文初筛，正式引用须核对中文原件。 |

上表所列重点件的时间信息如下。“较早公开”是已核同族的公开文本，不等于已经查尽该族所有国家阶段：

| 授权/当前公开文本 | 申请人或公开文本所列权利人 | 已核最早优先权/申请基础 | 已核较早公开及日期 |
| --- | --- | --- | --- |
| US7716215B2 | IBM | 母案 2003-10-31 | [US20050097078A1](https://patents.google.com/patent/US20050097078A1/en)，2005-05-05；本继续申请 [US20080177722A1](https://patents.google.com/patent/US20080177722A1/en)，2008-07-24 |
| US11321317B2 | Oracle International | 同族主张 2012-09-28 临时申请利益；本继续申请 2017-08-01 | [US20140095475A1](https://patents.google.com/patent/US20140095475A1/en)，2014-04-03；本件 [US20170357692A1](https://patents.google.com/patent/US20170357692A1/en)，2017-12-14 |
| US11334538B2 | Microsoft Technology Licensing | 2019-05-31 | [US20200379963A1](https://patents.google.com/patent/US20200379963A1/en)，2020-12-03 |
| US7493337B2 | Microsoft | 2004-03-31 | [US20050222965A1](https://patents.google.com/patent/US20050222965A1/en)，2005-10-06 |
| US9213740B2 | Sybase | 2007-10-11 | [US20090100004A1](https://patents.google.com/patent/US20090100004A1/en)，2009-04-16 |
| US7877381B2 | IBM | 2006-03-24 | [US20070226186A1](https://patents.google.com/patent/US20070226186A1/en)，2007-09-27 |
| US9836506B2 | IBM | 2014-06-11 | [US20150363466A1](https://patents.google.com/patent/US20150363466A1/en)，2015-12-17 |
| US11556539B2 | Oracle International | 2019-09-16 | 本件 2023-01-17 授权公开；更早申请公开未在本轮核定 |
| CN117271481B | 深圳九有数据库有限公司 | 2023-11-22 | [CN117271481A](https://patents.google.com/patent/CN117271481A/zh)，2023-12-22 |

## 直接影响保护边界的原文细节

### 1. IBM 的前缀阈值与 EOF 判定已经公开

[US7716215B2 PDF](https://patentimages.storage.googleapis.com/b1/07/18/edc8bbe72b0545/US7716215.pdf) 的摘要及第5-8栏将 CHECK 算子插入物理计划，用运行中输出与估算比较，发现明显误估后结束当前计划并重优化**当前查询**。图7给出的 `CHECKNEXT` 分支，在 `count > high` 时立即调用重优化；在 `count < low` 且子流返回 EOF 时才调用重优化。这已经体现“未完成前缀可以证实低估，低输出必须等完成才能证实高估”的判断，故**不能把 `A>E` 可纠正低估、`A<E` 需完成证明单独写成新颖点**。独立权利要求1（PDF 第19-20页）还写有实际基数、执行时间阈值、suspend/re-optimize/restart、可选重用中间结果。它不要求用户指定的单轮 deadline，不要求在执行器异常清理前将原计划全部节点快照导出，不要求对下一轮**独立候选**逐节点形成 `EXACT/LOWER_BOUND/NONE`。该件是 [US7383246B2](https://patents.google.com/patent/US7383246B2/en) 的继续申请；母案 2003-10-31 申请、[US20050097078A1](https://patents.google.com/patent/US20050097078A1/en) 于 2005-05-05 公开。母案独立权利要求1也写有运行中判断、suspend、重优化与 restart，但结合管线输出 side table/anti-join 补偿；两件应作为一族一起检索，不能把 2010 授权日当作最早公开日。

### 2. Oracle 已说明“部分执行后用于未来查询”

[US11321317B2 PDF](https://patentimages.storage.googleapis.com/cb/c7/fc/7708e63e84221e/US11321317.pdf) 第12栏直接写首次查询 *may have been fully executed or partially executed*，第12-13栏列出表、连接、group-by、索引基数等运行统计，并说明随后相同/等价查询可据此重优化；第14-15栏另讨论运行中实测与估算差异导致终止原计划、生成新计划的 *in-flight reoptimization*。第39-40页权利要求1进一步针对**单个操作**保存统计，按数据库对象和操作（包括谓词/连接匹配）提供给第二条**非等价**查询的计划。该件的当前公开申请为 [US20170357692A1](https://patents.google.com/patent/US20170357692A1/en)，公开于 2017-12-14；同族母案 [US20140095475A1](https://patents.google.com/patent/US20140095475A1/en) 于 2014-04-03 已公开含“首次部分执行”及 *in-flight* 的说明书，母案主张 2012-09-28 临时申请利益。**部分执行 + 算子统计 + 后续优化**因此不宜再作为本案宽泛区别。当前所核说明书没有给出“外部时限取消时，对全树节点依据 EOF、未来重入和计数口径作三级证明”的规则；这只是文本核查结论，须由代理人进一步查家族全案。

### 3. Microsoft 已分别公开“算子下界”和“基数反馈”

[US7493337B2](https://patents.google.com/patent/US7493337B2/en)（[2005 年公开申请 PDF](https://patentimages.storage.googleapis.com/4e/ca/94/ff92902d607fdb/US20050222965A1.pdf)）的授权权利要求22、23、27、33，将算子在某时已输出行数作为其最终输出行数的下界，并可对多个算子维护上下界和传播；权利要求42的界面允许用户 abort query。其用途是估计、显示执行进度，不要求 abort 之后把节点证据转成下一轮 Rows 修正。2004-03-31 优先权、[US20050222965A1](https://patents.google.com/patent/US20050222965A1/en) 于 2005-10-06 公开；下界思想早于 SQL Server Live Query Statistics 文档。

[US11334538B2](https://patents.google.com/patent/US11334538B2/en) 的独立权利要求1，从 query host 的运行事件提取实际基数及关联估算的参数，由 feedback manager 输出更改建议；说明书描述事件既在执行中也在完成时产生，建议用于以后相同或相似查询，并含振荡时暂停反馈的状态。[US20200379963A1](https://patents.google.com/patent/US20200379963A1/en) 于 2020-12-03 公开，Google Patents 列出 WO/EP/CN 同族。所核全文未出现超时、取消、abort 或 incomplete 的明确路径。因此“微软只公开监控、不公开反馈修正”并不准确：**微软至少在这一专利中公开了真实执行基数到后续优化器的反馈链**；待区分的应是强制取消后的节点证据判定。

更早的 Microsoft [US20040236722A1](https://patents.google.com/patent/US20040236722A1/en)（公开 2004-11-25，后授权 [US7146363B2](https://patents.google.com/patent/US7146363B2/en)）摘要公开执行中观测表达式基数、保存并估算未来计划；公开申请权利要求1/5覆盖表达式匹配、执行观测与保存。IBM [US20020198867A1](https://patents.google.com/patent/US20020198867A1/en)（公开 2002-12-26，后授权 [US6763359B2](https://patents.google.com/patent/US6763359B2/en)）权利要求1/3/5/7覆盖执行经验验证优化模型、执行中监测与未来计划修正。Microsoft [US20090094191A1](https://patents.google.com/patent/US20090094191A1/en)（公开 2009-04-09；Google Patents 标示 abandoned）权利要求1/3/4也明确按表达式基数执行反馈自动选择新访问方法。上述公开文本排除“基于真实执行反馈修正未来计划”这一宽泛新颖性叙述。

### 4. 中断候选的**耗时**反馈早有专利

[US9213740B2 PDF](https://patentimages.storage.googleapis.com/4c/85/07/5adb62796e158c/US9213740.pdf) 第25-26栏图5实施例明确：DBA 因候选运行过久而 abort，新计划虽 *incomplete*，若已运行时间超过当前最佳计划，则该事实仍可作为学习事件，把该计划记录为不佳并避免未来采用。其独立权利要求1和21属于计划级好坏比较及后续计划选用；未要求从中断时 `PlanState/Instrumentation` 取得逐节点 rows，也没有基数目标的精确/下界分类。优先权 2007-10-11；[US20090100004A1](https://patents.google.com/patent/US20090100004A1/en) 于 2009-04-16 公开。交底书可以说“取消候选不能拿截止时长冒充**完整运行时间**”，但不宜说“取消轮次的耗时完全不能提供有效比较信息”；只有当已耗时越过某阈值时，耗时本身确实能排除优胜可能。

### 5. 其它相近路径及排除理由

- IBM [US7877381B2](https://patents.google.com/patent/US7877381B2/en)（[US20070226186A1](https://patents.google.com/patent/US20070226186A1/en)，2007-09-27）：独立权利要求1把远端数据源的部分执行统计、检查点实际基数、重复重优化和重执行串在一起，最终仍服务于当前联邦查询的结果；其“统计完整性”说的是远端统计可用性，不是取消节点对 Rows 目标的完成性证明。
- IBM [US9836506B2](https://patents.google.com/patent/US9836506B2/en)（[US20150363466A1](https://patents.google.com/patent/US20150363466A1/en)，2015-12-17）：独立权利要求1的 UDF pilot-run 属子表达式试运行，统计用于本次查询谓词顺序和基数/成本；不能概括成必须等待主 SQL 全部完成才能取得反馈。
- Oracle [US7953728B2](https://patents.google.com/patent/US7953728B2/en)（[US20080288446A1](https://patents.google.com/patent/US20080288446A1/en)，2008-11-20）：权利要求1以软时间限制预测耗时，预先把 SQL 改写成限制行数或采样比例，从而返回**部分查询结果**；不是超时后的计划节点行数证据。
- Oracle [US11556539B2](https://patents.google.com/patent/US11556539B2/en)（2023-01-17 授权公开）：独立权利要求1按计划组件预测资源和性能阈值，运行时超阈值就停止失控查询；没有核到中止后逐节点基数回灌。
- Teradata [US8818988B1](https://patents.google.com/patent/US8818988B1/en)（2014-08-26）：独立权利要求1在工作负载调度器监控执行时将 request/query-plan 的运行统计反馈给优化器，主要为资源估计和优先级；未核到超时后的节点级 rows。
- Oracle [US9720941B2](https://patents.google.com/patent/US9720941B2/en)（2017-08-01）：独立权利要求1在一个或多个调优时段内生成建议、执行候选并比较计划；说明书提到采样与部分执行技术，但未核到被取消节点的完整/下界规则。
- CN [CN114610588A](https://patents.google.com/patent/CN114610588A/zh)（2022-06-10）：权利要求1采集数据库**处理节点**的资源描述并形成日志用于性能分析/预测；说明书虽列 `EXPLAIN ANALYZE` 例子，处理节点指分布式计算/存储节点，并非计划树算子。因此此前以 `EXPLAIN ANALYZE timeout` 检出的此件不是最接近方案。
- CN [CN118363985A](https://patents.google.com/patent/CN118363985A/en) / [WO2024152634A1](https://patents.google.com/patent/WO2024152634A1/en)（华为，2024）：权利要求1按训练所得成本模型选择计划；虽命中 “actual rows + timeout” 检索式，未核到取消时节点反馈。

## 对交底书的修改约束

1. **不主张单个构件首创。** 超时/中断计划的有用信息（Sybase）、部分执行统计的后续重优化（Oracle）、执行中算子基数反馈（IBM、Microsoft）、算子已输出量作为下界及 EOF 后判断（Microsoft、IBM）均有公开。尤其不能把 Oracle 只写成“完整执行后才有统计反馈”，也不能把 Microsoft 只写成“实时展示”。
2. **候选区别应写成具体的依赖链。** 在预设单轮期限到达后，仍在执行器对象有效期内采集**原候选计划树**的节点状态与累计输出；对每个可映射的基数目标先核对谓词/连接/调用口径，再结合 EOF 与未来重入证明是否已全局完成；未完成但累计量同口径且超估算时仅传下界；其余不传行数。合格证据用于**下一轮独立候选**的基数输入，取消轮次不以时限冒充完整实测耗时。上述各环节的**组合及其必要次序**才是值得进一步审查的候选保护范围。
3. **创造性风险仍高。** IBM 已有前缀阈值和 EOF 分别证明低估/高估，Microsoft 已有算子下界，Oracle 已有部分执行供随后查询；代理人应逐篇审查这些文献是否可组合、最早申请日、同族权利要求及审查档案，并检索 CNIPA/EPO/WIPO 的非 Google 收录文本。本文只能支持“在已核文本中尚未看到完整相同链条”，不能支持“没有先例”的断言。
4. **实施依据必须对齐真实代码。** 若权利要求写“节点已完成”，必须说明仅本地 EOF 不足以排除 Nested Loop 内侧等未来重入；若写“下界”，必须说明跨多次调用的累计 `ntuples` 不能与单次 `plan_rows` 直接比较。对不支持的节点及并行/重扫口径宁可形成 `NONE`，不能用宽泛表述掩盖尚未证明的行为。

## 供代理人审查的候选保护组合

以下是技术要素顺序，不是已经通过新颖性审查的权利要求文本；任何单项都不宜独立宣称首创。

1. 为**真实候选 SQL 的独立试运行**设置单轮执行期限；期限触发取消时，在执行器状态失效之前取得该候选的原物理计划树、各可观测节点的累计输出和运行状态。
2. 对待反馈的扫描或连接目标，先建立节点与目标的语义对应，并证明实际计数与原估算覆盖相同的查询、谓词及调用范围。若只有跨多次调用的累计计数而原估算表示单次输出，应取得同口径的目标或拒绝该反馈。
3. 在上述映射成立后，结合本地 EOF 与父链未来可能的重入判断目标是否完整：能证明不再输出时给精确值；不能证明完整但输出只会累加、且当前输出已超过同口径估算时给**只提高基数的下界**；其它情况不发布行数证据。已执行并以零行完成与尚未启动须区分。
4. 按证据强度合并目标基数，使其成为**下一轮独立候选**的优化器输入；取消轮次的已耗时不得冒充完整执行时间参与已测最快候选的比较。

较窄的从属层级可进一步限定：父子调用边的保守分类、重复调用的计数归一化或拒绝、不同证据强度的合并顺序、数据/谓词语义改变时的失效，以及对未知或并行节点的保守 `NONE`。`NOTICE` 分帧、连接恢复、相同计划的通信校验等属于当前部署实施细节，不应替代上述技术问题成为主张中心。

**实施支撑限制：**目前原型具有取证、分级和下一轮合并，但[实现审计](patent-timeout-feedback-manuscript-audit.md)指出，父链未来首次重入、跨多次调用的统一计数口径，以及跨轮谓词/连接语义一致性仍未对所有节点得到证明。上述组合中的第 2、3 项是待落实的严格方法条件，不能描述为当前代码已普遍完成。即便补足实现，IBM、Oracle、Microsoft 与 Sybase 文献的组合仍带来明显创造性挑战。

## 尚未完成的检索边界

本轮已按英文、中文关键词和三条高风险文献的引用链筛选，并核对上述独立权利要求及关键说明书位置；**没有完成**各国专利数据库的分类号遍历、全部同族对照、所有引证的逐件全文审查、审查意见与法律状态核验，也没有检索未公开申请。中文 [CN117271481B](https://patents.google.com/patent/CN117271481B/zh) 的技术判断仍依赖 Google Patents 英译；正式交底书引用其权利要求前，应以 CNIPA 原文重核“执行终止条件”和“执行数据”的具体语义。专利申请前应由专利代理人开展 FTO/新颖性专项检索；本笔记提供优先核查清单，不给出可授权结论。
