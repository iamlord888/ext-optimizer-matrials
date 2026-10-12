# 五件数据库专利中文译稿

本目录存放中文工作译稿；[英文原始影印 PDF](../patent-originals/) 单独保存。译文按 Google Patents 英文公开页的摘要、说明书和权利要求书提取，保留说明书 `p-####` 段落号及权利要求编号。附图请与原始 PDF 对照。译稿用于技术研究，不是官方中文译本，权利要求的法律含义以英文原件为准。

目前可阅读[核心权利要求与关键段落中文校译](核心权利要求与关键段落校译.md)。五件全文的结构化英文正文已核对齐全；完整中文译稿仍在制作中，未经校订的低质量机译没有放入本目录。

| 公开号 | 原文页数 | 英文公开页 |
| --- | ---: | --- |
| `US7716215B2` | 28 | [Progressive query processing](https://patents.google.com/patent/US7716215B2/en) |
| `US11321317B2` | 32 | [Query-specific statistics](https://patents.google.com/patent/US11321317B2/en) |
| `US11334538B2` | 22 | [Cardinality estimation feedback loops](https://patents.google.com/patent/US11334538B2/en) |
| `US7493337B2` | 25 | [Query progress estimation](https://patents.google.com/patent/US7493337B2/en) |
| `US9213740B2` | 20 | [Query plan performance learning](https://patents.google.com/patent/US9213740B2/en) |

## 术语对照

| 英文 | 本稿统一译法 |
| --- | --- |
| cardinality / actual cardinality / estimated cardinality | 基数 / 实际基数 / 估算基数 |
| query execution plan (QEP) | 查询执行计划 |
| operator / query plan operator | 算子 / 执行计划算子 |
| checkpoint / CHECK operator | 检查点 / `CHECK` 算子 |
| lower bound / upper bound | 下界 / 上界 |
| partial execution / partial result | 部分执行 / 部分结果 |
| materialized intermediate result | 物化中间结果 |
| re-optimization / feedback | 重新优化 / 反馈 |
| abort / cancel / terminate | 中止 / 取消 / 终止，依原文语境区分 |
| end-of-file (EOF) | 数据流结束信号（`EOF`） |
| claim | 权利要求 |

检索结论与本案边界见[专利检索笔记](../patent-timeout-feedback-patent-landscape.md)。
