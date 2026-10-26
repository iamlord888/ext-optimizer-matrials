# IBM、Oracle、Microsoft 三件专利的相关论文与产品手册

初次核验日期：2026-10-20；算子结束机制及交底书对应关系更新：2026-10-22。范围为 IBM US7716215B2、Oracle US11321317B2、Microsoft US7493337B2。使用论文原文、作者机构页面、会议出版页面和厂商官方手册核对，不以第三方博客建立产品实现结论。

本报告区分三种关联：作者与机制直接对应的研究论文；明确说明产品实现的后续论文；公开相近功能及接口的产品手册。作者重合及机制对应可以建立研究关联，但不足以逐项认定某产品实施全部授权权利要求。产品手册版本及本次检索日期也不代表功能的最早公开日。

## 1 快速对应表

| 专利 | 优先阅读的论文 | 官方产品资料 | 关联程度 |
| --- | --- | --- | --- |
| IBM US7716215B2 | *Robust Query Processing through Progressive Optimization*，SIGMOD 2004；*Progressive Optimization in Action*，VLDB 2004 | Db2 section_actuals、EXPLAIN_FROM_ACTIVITY、section actuals 性能诊断指南 | 论文与专利的作者和 CHECK 机制直接对应；论文明确为 Db2 研究原型，所查手册只确认算子统计采集和诊断 |
| Oracle US11321317B2 | *Adaptive Statistics in Oracle 12c*，PVLDB 2017 | Adaptive Query Plans、Statistics Feedback、SQL Plan Directives | 两名作者与专利发明人重合；论文和手册覆盖统计反馈与跨查询复用，但各路径的采集时点和保存内容不同 |
| Microsoft US7493337B2 | *Estimating Progress of Execution for SQL Queries*，SIGMOD 2004；*Operator and Query Progress Estimation in Microsoft SQL Server Live Query Statistics*，SIGMOD 2016 | SQL Server Live Query Statistics、sys.dm_exec_query_profiles、Query Profiling Infrastructure | 2004 论文作者与发明人对应；2016 产品论文明确引用并采用早期边界技术，能建立直接研究与产品联系 |

## 2 IBM US7716215B2：渐进式查询处理

### 2.1 最直接相关的论文

**Robust Query Processing through Progressive Optimization**，Volker Markl 等，SIGMOD 2004，DOI [10.1145/1007568.1007642](https://doi.org/10.1145/1007568.1007642)。[IBM Research 论文页面](https://research.ibm.com/publications/robust-query-processing-through-progressive-optimization)；[论文 PDF](https://dsf.berkeley.edu/cs286/papers/robust-sigmod2004.pdf)。

论文在计划中设置 CHECK，使用计划仍有效的基数区间检查实际返回行数；违反条件时停止当前执行阶段并重优化，后续阶段利用已取得的统计，并按成本决定是否复用中间结果。作者与[专利发明人](https://patents.google.com/patent/US7716215B2/en)高度重合，机制也直接对应。重点阅读 PDF **Pages 3-8，第 2-3 节**。

**Progressive Optimization in Action**，Vijayshankar Raman、Volker Markl、David Simmen、Guy Lohman、Hamid Pirahesh，VLDB 2004 演示论文，印刷页 1337-1340。[VLDB 官方 PDF](https://www.vldb.org/conf/2004/DEMP22.PDF)。

该文以 Db2 研究原型演示检查点、基数有效区间、重新优化及计划变化，适合快速理解完整流程。论文提供原型证据，本次未找到正式产品手册明确确认全部 POP CHECK 中途重优化机制已作为标准 Db2 功能交付。

### 2.2 相关产品手册

| 页面 | 可核对的产品能力 |
| --- | --- |
| [Db2 12.1.x：section_actuals 配置](https://www.ibm.com/docs/en/db2/12.1.x?topic=parameters-section-actuals-section-actuals) | BASE 启用基本算子基数计数，供后续 section explain 查看 |
| [Db2 11.5.x：EXPLAIN_FROM_ACTIVITY](https://www.ibm.com/docs/en/db2/11.5.x?topic=er-explain-from-activity-procedure-explain-statement-using-activity-event-monitor-information) | 从活动事件监控器读取某次执行的计划；存在 section actuals 时一并显示算子运行统计。Usage notes 明确通过 WLM_CAPTURE_ACTIVITY_IN_PROGRESS 捕获的记录不含 section actuals |
| [Db2：使用 section actuals 排查查询性能](https://www.ibm.com/docs/en/db2/11.5?topic=casa-obtaining-section-explain-actuals-investigate-poor-query-performance) | 给出采集和比较实际统计与优化器估算的操作步骤 |

这些手册适合证明产品的统计采集与诊断接口。`REOPT` 根据实际绑定值重新选择访问路径，需与 POP 的 CHECK 驱动重优化分别表述。更完整的作者、背景工作和产品边界见 [IBM 专项核查](patent-timeout-feedback-ibm-papers-manuals.md)。

## 3 Oracle US11321317B2：跨查询统计复用

### 3.1 最相关论文及关联依据

**Adaptive Statistics in Oracle 12c**，Sunil Chakkappen、Suratna Budalakoti、Ramarajan Krishnamachari、Satyanarayana R. Valluri、Alan Wood、Mohamed Zait，PVLDB 10(12)，1813-1824，2017。[VLDB 官方 PDF](https://www.vldb.org/pvldb/vol10/p1813-zait.pdf)；[DOI：10.14778/3137765.3137785](https://doi.org/10.14778/3137765.3137785)。DOI、作者和出版页已通过 [Crossref 注册元数据](https://api.crossref.org/works/10.14778%2F3137765.3137785)核对。

该[专利](https://patents.google.com/patent/US11321317B2/en)发明人为 Hong Su、Sunil P. Chakkappen、Mohamed Zait，其中后两人与论文作者对应。论文描述 Oracle 12c 的自适应统计、统计查询及 SQL Plan Directives，与专利的运行统计保存和后续查询利用具有直接技术关联；不将整篇论文与该件授权权利要求等同。

重点阅读原 PDF **Page 8，第 3.2.2 节及第 3.3.1 节**，以及 **Page 9，第 3.3.2 节**：前者说明统计查询受时间预算限制并可使用已产生的部分结果，以及根据运行时基数偏差形成指令；后者说明统计结果的持久化缓存。这里的限时对象是优化阶段的统计查询，不能直接解释为取消用户主 SQL 后读取其节点快照。[论文原文](https://www.vldb.org/pvldb/vol10/p1813-zait.pdf)。

### 3.2 官方产品手册页面

| 手册及位置 | 产品公开内容 | 与专利的联系 |
| --- | --- | --- |
| [Oracle Database 12.1 SQL Tuning Guide：Query Optimizer Concepts](https://docs.oracle.com/database/121/TGSQL/tgsql_optcncpt.htm)，定位 How Adaptive Query Plans Work | 计划包含多个预定子计划和 statistics collector，运行期间根据统计选择子计划 | 对应专利说明书中的 Adaptive Plan 分支 |
| [同章：Reoptimization: Statistics Feedback](https://docs.oracle.com/database/121/TGSQL/tgsql_optcncpt.htm#GUID-739F1F0B-ABE2-44C9-9783-8FA54F4ADB12) | 执行结束时比较各操作估算与实际行数，后续执行使用修正估算 | 对应统计反馈到后续规划的产品路径 |
| [Oracle Database 12.1：Optimizer Statistics Concepts](https://docs.oracle.com/database/121/TGSQL/tgsql_statscon.htm)，定位 SQL Plan Directives | 指令按查询表达式定义，不绑定单一 SQL ID；可持久化并供其他语句使用 | 对应跨查询复用相关统计信息的机制，但指令可记录误估事实并指导统计收集，不应全部描述为保存原始节点 A-rows |

12.1 手册给出了修改选择列表和谓词值后使用已有 directive 的示例；Statistics Feedback 的手册路径明确在执行结束时处理。专利关于部分执行和 In-Flight Reoptimization 的描述仍应引用专利原文，不能用上述手册替代其全部实施方式。较新的 [Oracle 19c 同名章节](https://docs.oracle.com/en/database/oracle/oracle-database/19/tgsql/query-optimizer-concepts.html)也保留 Adaptive Query Plans 与 Statistics Feedback 的说明；不同版本的默认设置需分别核对。

## 4 Microsoft US7493337B2：查询进度估计

### 4.1 直接相关研究论文

**Estimating Progress of Execution for SQL Queries**，Surajit Chaudhuri、Vivek Narasayya、Ravishankar Ramamurthy，SIGMOD 2004，803-814。[Microsoft Research 页面](https://www.microsoft.com/en-us/research/publication/estimating-progress-of-execution-for-sql-queries/)；[全文 PDF](https://www.microsoft.com/en-us/research/wp-content/uploads/2016/02/progress-1.pdf)；[DOI：10.1145/1007568.1007659](https://doi.org/10.1145/1007568.1007659)。

三名作者与[专利](https://patents.google.com/patent/US7493337B2/en)发明人对应。论文利用算子返回量或 GetNext 调用建立工作量模型，维护节点基数上下界，修正用于进度估计的行数。重点阅读 PDF **Pages 5-6，第 4.1-4.2 节**，以及 **Page 12，Appendix B**。

### 4.2 能建立研究与产品联系的后续论文

**Operator and Query Progress Estimation in Microsoft SQL Server Live Query Statistics**，Kukjin Lee 等，SIGMOD 2016。[Microsoft Research 页面](https://www.microsoft.com/en-us/research/publication/operator-and-query-progress-estimation-in-microsoft-sql-server-live-query-statistics/)；[全文 PDF](https://www.microsoft.com/en-us/research/wp-content/uploads/2016/06/qprog.pdf)；[DOI：10.1145/2882903.2903728](https://doi.org/10.1145/2882903.2903728)。

论文明确介绍 SQL Server 2016 LQS 的设计与实现。PDF **Page 6，第 4.2 节 Cardinality Bounding** 说明采用参考文献 [7] 的算子基数上下界技术，估算越界时修正到最近边界；**Page 11，参考文献 [7]** 即上述 2004 年论文。这是早期研究被后续产品功能采用的直接文献证据；最终用途仍为当前查询进度估计。[2016 年原文](https://www.microsoft.com/en-us/research/wp-content/uploads/2016/06/qprog.pdf)。

### 4.3 官方产品手册

| 页面 | 可核对的产品能力 |
| --- | --- |
| [Live Query Statistics](https://learn.microsoft.com/en-us/sql/relational-databases/performance/live-query-statistics?view=sql-server-ver17) | SSMS 中显示运行查询的实时计划、查询及算子进度、返回行数和已耗时间 |
| [sys.dm_exec_query_profiles](https://learn.microsoft.com/en-us/sql/relational-databases/system-dynamic-management-objects/sys-dm-exec-query-profiles-transact-sql?view=sql-server-ver17) | 提供算子及线程级 node_id、row_count、estimate_row_count、end_of_scan_count、rewind_count、rebind_count、close_time 等统计 |
| [Query Profiling Infrastructure](https://learn.microsoft.com/en-us/sql/relational-databases/performance/query-profiling-infrastructure?view=sql-server-ver17) | 说明统计采集基础设施、标准与轻量 profiling，以及版本相关的启用方式 |
| [Logical and Physical Showplan Operator Reference](https://learn.microsoft.com/en-us/sql/relational-databases/showplan-logical-and-physical-operators-reference?view=sql-server-ver17) | 说明物理算子生命周期、扫描结束和重复执行；相关语义已集中写入交底书第 1.4 节：现有相关技术三 |

算法联系由产品论文支持，用户可见功能由手册支持；本报告不据此逐项认定某版本实现了专利全部权利要求。更多作者、页码和版本条件见 [Microsoft 专项核查](patent-timeout-feedback-microsoft-papers-manuals.md)。

## 5 建议阅读顺序及引用方式

1. IBM：先读 4 页的 *Progressive Optimization in Action*，再读 SIGMOD 2004 完整论文；产品手册单独作为统计采集和诊断依据。
2. Oracle：先读 Statistics Feedback 与 SQL Plan Directives 手册示例，再读 2017 论文第 8-9 页。分别说明用户查询反馈和优化阶段统计查询的作用。
3. Microsoft：先读 2004 论文理解上下界，再读 2016 产品论文第 4.2 节，最后查看 LQS 和 DMV 手册。

若补充专利 PPT 的现有技术页，每家公司可使用“专利 + 关键论文 + 官方手册”的三类依据，分别说明公开机制、论文关联和产品功能。授权专利本身的精确页码见 [三件专利页码导读](patent-timeout-feedback-prior-art-page-guide.md)。

## English summary

IBM's POP papers closely match US7716215B2 and explicitly describe a Db2 research prototype. The product manuals separately document section actuals and execution-plan diagnostics; this search did not establish shipment of the complete POP workflow. Oracle's 2017 adaptive-statistics paper shares two authors with the inventors of US11321317B2, and official manuals explain adaptive plans, statistics feedback, and SQL Plan Directives. Its time-limited statistics queries are distinct from cancellation of the user's main query. Microsoft's 2016 SQL Server Live Query Statistics paper explicitly reuses the cardinality-bounding techniques of the 2004 progress-estimation paper associated with US7493337B2. The corresponding manuals document the available progress display and operator counters. Research linkage, product documentation, and implementation of individual patent claims are treated separately.
