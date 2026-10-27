# 限时执行部分反馈专利材料 / Time-Limited Execution Feedback Patent Materials

**专利名称：一种基于限时执行部分反馈的查询执行计划迭代优化方法。**

**Patent title: An Iterative Query Execution Plan Optimization Method Based on Partial Feedback from Time-Limited Execution.**

慢候选可能占用大部分调优预算，超时取消会中断 `EXPLAIN ANALYZE` 的正常结果路径。本材料整理了在取消清理前保留行数和执行状态、判断可用反馈并用于同一 SQL 下一轮候选的方案。

Slow candidates can consume much of the tuning budget, while timeout cancellation interrupts the normal `EXPLAIN ANALYZE` result path. These materials describe capturing row counts and execution state before cleanup and using qualified feedback for the next candidate of the same SQL.

业务目标是控制慢候选的单轮试运行时间，保留并利用已产生的有效节点统计，以降低整体调优耗时，为全局调优预算内的后续有效迭代争取时间，增加发现更优执行计划的机会。实测支持固定工作负载的调优总时长下降，候选轮次和最终交付效果仍需分别评价。

The business objective is to bound each slow candidate's trial execution and retain its usable operator statistics to reduce overall tuning time, leave time for useful iterations within the global tuning budget, and increase the chance of finding a better execution plan. Workload tests support shorter aggregate tuning duration; iteration counts and final delivery quality require separate evaluation.

## 文稿 / Documents

- [专利交底书底稿（2026-10-27修订）/ Disclosure draft, revised 2026-10-27](design/patent-timeout-feedback-disclosure-draft.md)
- [完整修订稿Word版 / Complete revised manuscript in Word](design/patent-timeout-feedback-review-20261027/revised-manuscript.docx)
- [配对Markdown正文 / Paired manuscript source](design/patent-timeout-feedback-review-20261027/revised-manuscript.md)
- [Word排版与同步约定 / Word formatting and synchronization](design/patent-timeout-feedback-review-20261027/WORD_FORMAT.md)
- [七页中文汇报文稿 / Chinese slide text](design/patent-timeout-feedback-idea-slides.md)
- [英文汇报文稿 / English slide text](design/patent-timeout-feedback-idea-slides-en.md)
- [PPT 内容与拟保护点修订建议 / Slide and protection recommendations](design/patent-timeout-feedback-protection-review.md)
- [修订建议英文对照 / Recommendations in English](design/patent-timeout-feedback-protection-review-en.md)
- [本轮专利原文复核 / Primary patent review](design/patent-timeout-feedback-primary-review-20261012.md)
- [文稿一致性与事实核查 / Manuscript consistency and fact audit](design/patent-timeout-feedback-manuscript-audit.md)
- [现有技术初步核查 / Prior-art review](design/patent-timeout-feedback-prior-art.md)
- [专利检索与技术边界 / Patent landscape](design/patent-timeout-feedback-patent-landscape.md)
- [论文和数据库产品核查 / Papers and database products](design/patent-timeout-feedback-products-papers.md)
- [三件专利的论文与产品手册导读 / Related papers and product manuals](design/patent-timeout-feedback-related-papers-manuals.md)
- [与已有方案的重叠分析 / Overlap analysis](design/patent-timeout-feedback-overlap-analysis.md)
- [五件专利的方法流程分析 / Five patent method analyses](design/patent-five-method-flow-analysis.md)
- [三件相关专利的原文页码导读 / PDF page guide to three related patents](design/patent-timeout-feedback-prior-art-page-guide.md)

## 配图 / Figures

每张图只保留当前版本，同一图在不同格式中使用相同文件名。PNG 和 SVG 为约一页 16:9 PPT 高度的三分之一，PPTX 中的文字、形状和图表可以编辑。

Each figure has one current version and the same filename across formats. PNG and SVG figures occupy about one third of a 16:9 slide's height. Text, shapes and charts in the PPTX files remain editable.

- [PNG](design/patent-timeout-feedback-figures/png/)：9 张图片 / 9 images
- [SVG](design/patent-timeout-feedback-figures/svg/)：7 张技术图 / 7 technical diagrams
- [PPTX](design/patent-timeout-feedback-figures/pptx/)：9 份可编辑文件 / 9 editable presentations
- [HTML 总览 / HTML gallery](design/patent-timeout-feedback-figures/html/index.html)

| 图 / Figure | 内容 / Subject | PNG | 可编辑 PPT / Editable PPT |
| --- | --- | --- | --- |
| 1 | 候选 SQL 的限时执行 / Candidate execution deadline | [PNG](design/patent-timeout-feedback-figures/png/fig1-conventional.png) | [PPTX](design/patent-timeout-feedback-figures/pptx/fig1-conventional.pptx) |
| 2 | 单轮候选与行数反馈 / Candidate rounds and row-count feedback | [PNG](design/patent-timeout-feedback-figures/png/fig2-system.png) | [PPTX](design/patent-timeout-feedback-figures/pptx/fig2-system.pptx) |
| 3 | 算子统计、get_next 与本地 EOF / Operator statistics, get_next and local EOF | [PNG](design/patent-timeout-feedback-figures/png/fig3-executor-observation-flow.png) | [PPTX](design/patent-timeout-feedback-figures/pptx/fig3-executor-observation-flow.pptx) |
| 4 | 执行器释放前读取统计 / Capture before executor cleanup | [PNG](design/patent-timeout-feedback-figures/png/fig4-capture-window.png) | [PPTX](design/patent-timeout-feedback-figures/pptx/fig4-capture-window.pptx) |
| 5 | 行数分类和 Rows Hint 更新 / Row classification and Rows Hint updates | [PNG](design/patent-timeout-feedback-figures/png/fig5-evidence.png) | [PPTX](design/patent-timeout-feedback-figures/pptx/fig5-evidence.pptx) |
| 6 | 超时执行中的三种算子状态 / Three operator states at timeout | [PNG](design/patent-timeout-feedback-figures/png/fig6-mixed-states.png) | [PPTX](design/patent-timeout-feedback-figures/pptx/fig6-mixed-states.pptx) |
| 7 | 再次扫描与累计行数 / Repeated scans and cumulative rows | [PNG](design/patent-timeout-feedback-figures/png/fig7-reentry-scope.png) | [PPTX](design/patent-timeout-feedback-figures/pptx/fig7-reentry-scope.pptx) |
| 8 | 调优总时长 / Total tuning duration | [PNG](design/patent-timeout-feedback-figures/png/fig8-tuning-duration.png) | [PPTX](design/patent-timeout-feedback-figures/pptx/fig8-tuning-duration.pptx) |
| 9 | 完整执行时间总和 / Total complete execution time | [PNG](design/patent-timeout-feedback-figures/png/fig9-execution-effect.png) | [PPTX](design/patent-timeout-feedback-figures/pptx/fig9-execution-effect.pptx) |

第 2 张图明确标出第 i 轮候选。正常完成和超时取消都进入算子行数反馈，用于生成第 i+1 轮候选；正常完成还提供运行时间，用于比较最佳已完成候选。第 3 页文稿已简化为遍历算子、读取行数、判定本地 EOF、返回统计四步。

Figure 2 shows both normal-completion and timeout statistics entering row-count feedback for candidate i+1. Normal completion also supplies runtime for selecting the best completed candidate. Slide 3 uses four steps: traverse operators, read row counts, identify local EOF and return statistics.

本地查看 HTML：在仓库根目录运行 `python3 -m http.server 4311`，然后打开 `http://127.0.0.1:4311/design/patent-timeout-feedback-figures/html/index.html`。

For local HTML preview, run the command above from the repository root and open that URL.

## 原始资料 / Sources

- [五件专利英文 PDF 与校验清单 / Five original patent PDFs and checksums](design/patent-originals/MANIFEST.md)
- [核心权利要求与关键段落校译 / Reviewed Chinese translations](design/patent-translations/README.md)
- [结构化英文专利正文与引文核对 / Structured patent text and quote audit](sources/patents/)
- [论文 PDF / Paper PDFs](sources/papers/)
- [原需求串讲 / Original requirements walkthrough](sources/requirements/)

## 实现与实验依据 / Implementation and Experimental Evidence

- [当前部分反馈需求 / Current partial-feedback requirements](design/超时EXPLAIN_ANALYZE部分反馈需求串讲.md)
- [反馈协议 / Feedback protocol](design/timeout-feedback-protocol.md)
- [实施与验证记录 / Implementation and validation record](design/timeout-feedback-implementation.md)
- [JOB 对照测试方案 / JOB comparison plan](design/JOB超时反馈对照测试方案.md)
- [GaussDB 部署记录 / GaussDB deployment record](design/JOB独立GaussDB部署记录.md)
- [JOB 执行总时间差额分析 / JOB runtime contribution analysis](design/patent-timeout-feedback-job-execution-attribution.md)
- [逐查询数据工作簿 / Per-query workbook](outputs/job_job_complex_tuning_query_summary_patience5_20260928.xlsx)
- [测量、调优与事件记录 / Measurement, tuning and event records](data/patience5-ab-20260928/)
- [文稿引用的实现快照 / Referenced implementation snapshots](contrib/)

实现快照用于核对文稿中的结构体、字段和调用关系。实验材料保留正式交付候选与恢复候选的区别，具体采用方式见文稿及数据记录。

The implementation snapshots support inspection of the structures, fields and call relationships cited by the documents. Experimental records retain the distinction between formally delivered and recovered candidates, as explained in the documents and data.

[材料来源及 SHA-256 清单 / Provenance and SHA-256 manifest](MATERIALS-MANIFEST.json)
