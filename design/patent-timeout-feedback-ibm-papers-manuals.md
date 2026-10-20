# IBM US7716215B2：相关论文与官方产品手册

核查日期：2026-10-20。范围为 IBM `US7716215B2` 的渐进式查询处理、相关 POP 论文及 Db2 官方产品文档。以下区分同一技术工作的论文、相关研究和产品已公开的功能；论文中的原型不直接等同于正式产品功能。

## 1 核查结论

已找到两篇与该专利直接相关的 POP 论文，以及公开算子运行统计采集和计划分析能力的 Db2 官方手册。SIGMOD 2004 论文和 VLDB 2004 演示论文描述 CHECK、基数有效区间、执行中停止与重优化，以及已有中间结果和统计的利用，技术内容与专利说明书直接对应。VLDB 论文明确称实现为 Db2 研究原型；本次未找到 Db2 产品手册明确确认整套 POP CHECK 机制作为正式功能发布。[IBM 论文机构页](https://research.ibm.com/publications/robust-query-processing-through-progressive-optimization)、[VLDB 演示论文](https://www.vldb.org/conf/2004/DEMP22.PDF)、[专利原文](https://patents.google.com/patent/US7716215B2/en)。

## 2 直接相关论文

### 2.1 Robust Query Processing through Progressive Optimization

- **作者：** Volker Markl、Vijayshankar Raman、David Simmen、Guy Lohman、Hamid Pirahesh、Miso Cilimdzic。
- **发表：** SIGMOD 2004。
- **DOI：** [10.1145/1007568.1007642](https://doi.org/10.1145/1007568.1007642)。IBM Research 的 Publication 链接指向该 DOI；本次 DOI 站点正文抓取返回 403，题名、作者与会议由 IBM 机构页核实。
- **入口：** [IBM Research 作者机构页](https://research.ibm.com/publications/robust-query-processing-through-progressive-optimization)、[论文全文 PDF](https://dsf.berkeley.edu/cs286/papers/robust-sigmod2004.pdf)。

论文在查询计划中放置 CHECK，对照实际行数与计划有效的基数区间；违反条件时停止当前执行阶段并重新优化。下一阶段可使用部分执行已取得的实际基数，并由优化器按成本决定是否复用中间结果。主要阅读 PDF 第 3-8 页：第 2 节整体流程，第 2.2 节有效区间计算，第 2.3 节中间结果利用，以及第 3 节不同 CHECK 方式。其作者与专利发明人高度重合，CHECK、区间计算、统计利用和结果复用等机制亦直接对应，属于本次最应优先阅读的相关论文。[论文全文](https://dsf.berkeley.edu/cs286/papers/robust-sigmod2004.pdf)、[专利发明人与说明书](https://patents.google.com/patent/US7716215B2/en)。

### 2.2 Progressive Optimization in Action

- **作者：** Vijayshankar Raman、Volker Markl、David Simmen、Guy Lohman、Hamid Pirahesh。
- **发表：** VLDB 2004，演示论文，印刷页 1337-1340，PDF 共 4 页。
- **入口：** [VLDB 官方论文 PDF](https://www.vldb.org/conf/2004/DEMP22.PDF)、[VLDB 2004 官方摘要目录](https://www.vldb.org/archives/website/2004/abstracts.html)。

该论文演示在 Db2 研究原型中展示基数有效区间与 CHECK 位置、执行时触发重优化、生成新计划并利用已计算的中间结果。PDF 第 2 页 Figure 1 展示完整反馈流程，第 3-4 页展示检查点及计划变化。它可以补充理解前述长篇论文，并证明研究原型及演示的存在；论文没有据此证明当前或当时某个正式 Db2 版本已交付全部机制。[VLDB 官方 PDF](https://www.vldb.org/conf/2004/DEMP22.PDF)。

本次检索到的正确题名为 **Progressive Optimization in Action**，不采用未核实的“Progressive Optimization in a DBMS”作为这篇论文的名称。

## 3 其他相关研究

| 论文 | 关联与边界 | 一手入口 |
| --- | --- | --- |
| *LEO - DB2's LEarning Optimizer*，Stillger、Lohman、Markl、Kandil，VLDB 2001 | 从以往执行的节点实际基数与估算偏差中学习，改进后续查询；专利将其作为相关背景，并区分其与当前查询的渐进式重优化。它是相关技术基础，不是 POP 2004 论文的替代品。 | [IBM Research](https://research.ibm.com/publications/leo-db2s-learning-optimizer)、[专利中 LEO 的讨论](https://patents.google.com/patent/US7716215B2/en) |
| *POP/FED: Progressive Query Optimization for Federated Queries in DB2*，Kache、Han、Markl、Raman、Ewen，VLDB 2006 | 将 POP 扩展到联邦查询，演示多轮重优化及统计获取；IBM 页面明确为 WebSphere Information Integrator for DB2 原型。 | [IBM Research](https://research.ibm.com/publications/popfed-progressive-query-optimization-for-federated-queries-in-db2) |

## 4 相关产品手册页面

### 4.1 section_actuals：采集算子的实际基数

**页面：** [Db2 12.1.x — section_actuals - Section actuals configuration parameter](https://www.ibm.com/docs/en/db2/12.1.x?topic=parameters-section-actuals-section-actuals)。

官方说明 `BASE` 可启用基本算子基数计数及所引用对象的运行统计；通过 activity event monitor 采集，再用 `EXPLAIN_FROM_ACTIVITY` 查看。该页直接支持“Db2 有算子运行统计采集能力”，但没有描述 CHECK 越界后自动暂停、重新优化当前查询的整个 POP 流程。

### 4.2 EXPLAIN_FROM_ACTIVITY：查看原执行计划与运行统计

**页面：** [Db2 11.5.x — EXPLAIN_FROM_ACTIVITY procedure](https://www.ibm.com/docs/en/db2/11.5.x?topic=er-explain-from-activity-procedure-explain-statement-using-activity-event-monitor-information)。

该过程从 activity event monitor 获取某次执行的 section，生成计划信息；存在 section actuals 时，输出同时包括计划和算子运行统计。适合作为“把本次运行计划与实际统计关联起来”的官方产品依据，不能单凭该接口推导自动执行反馈重优化已经实现。

该页的 Usage notes 还明确：通过 `WLM_CAPTURE_ACTIVITY_IN_PROGRESS` 获取的运行中活动记录不含 section actuals。因而“能够捕获运行中活动”与“该次捕获带有算子实际行数”需要分别核对，不能直接等同。[Db2 11.5.x 官方过程说明](https://www.ibm.com/docs/en/db2/11.5.x?topic=er-explain-from-activity-procedure-explain-statement-using-activity-event-monitor-information)。

### 4.3 比较估算与实际统计的诊断步骤

**页面：** [Db2 11.5.x — Obtaining a section explain with actuals to investigate poor query performance](https://www.ibm.com/docs/en/db2/11.5.x?topic=casa-obtaining-section-explain-actuals-investigate-poor-query-performance)。

该官方操作指南说明取得 section actuals，并与优化器产生的估算值比较，以排查查询性能下降。它是正式手册中的使用场景，能够支持实际行数与估算的诊断比较；没有规定按照每轮外部执行期限取消后，将精确行数或下界自动合并为下一轮基数输入。

## 5 应单独区分的 REOPT

Db2 手册也使用运行时“重新优化”的名称，但 `REOPT ONCE/ALWAYS` 及 `CURRENT EXPLAIN MODE = REOPT` 主要针对实际绑定值可用时的编译与访问路径选择，例如 host variables、parameter markers、special registers 和 global variables。该功能不能仅因名字相似，就被当作本文 POP 的 CHECK 越界后中途重优化。[Db2 官方说明](https://www.ibm.com/docs/en/db2/11.1?topic=facility-guidelines-capturing-explain-information)、[SET CURRENT EXPLAIN MODE](https://www.ibm.com/docs/en/db2-as-a-service?topic=statements-set-current-explain-mode)。

## 6 用于专利材料的引用方式

介绍 IBM 渐进式查询处理的核心机制时，可优先引用 **US7716215B2 + SIGMOD 2004 POP 论文**；需要说明 Db2 研究原型和演示时，补引 **Progressive Optimization in Action**；需要说明正式产品中的算子统计采集与分析能力时，另引 **section_actuals、EXPLAIN_FROM_ACTIVITY 和诊断指南**。应分别写明这些资料能够证明的内容，保持原型与正式产品手册之间的证据边界。

本次核验 IBM Research、VLDB 官方原文及 IBM Docs 官方索引正文。部分 IBM Docs 页面直接打开时抓取服务报错，相关页面内容仍可从搜索服务返回的官方索引正文核对；这不影响其链接作为官方产品手册入口，但未据此扩展任何未公开实现。产品手册版本和本次检索日期不代表该功能最早公开的时间。

## English summary

The closest papers are *Robust Query Processing through Progressive Optimization* (SIGMOD 2004) and *Progressive Optimization in Action* (VLDB 2004). Their authors and CHECK-based mechanisms closely match US7716215B2. The demonstration explicitly identifies a Db2 research prototype. IBM's product documentation separately confirms operator cardinality collection through section actuals and access-plan analysis through EXPLAIN_FROM_ACTIVITY. This search did not find a product manual establishing that the complete POP checkpoint-driven mid-execution reoptimization workflow shipped as a standard Db2 feature. REOPT based on actual bind values is a separate documented mechanism.
