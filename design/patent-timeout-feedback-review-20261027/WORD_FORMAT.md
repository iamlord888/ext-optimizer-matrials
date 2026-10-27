# 专利交底书Word同步约定

当前交付正文为[revised-manuscript.md](revised-manuscript.md)，对应Word为[revised-manuscript.docx](revised-manuscript.docx)。后续修改正文时，应在同一次交付中重新生成Word，避免两个格式的内容不一致。

正文使用宋体、小四12磅、1.5倍行距，首行缩进两个字符。Word中同时设置字符单位缩进和与12磅正文对应的24磅缩进。标题同样使用宋体，以字号和加粗区分层级，不作正文首行缩进，不添加标题横线。中文与英文之间不主动添加空格，正文保持自然段。

使用[export_word.py](export_word.py)从配对Markdown重新生成Word。脚本保留文稿文字、层级、引用和可编辑的期限公式，并生成本机同步校验信息。需要Python和python-docx；在Codex中应使用已加载的工作区Python运行。

```bash
python3 export_word.py
```

生成后，应核对正文内容与Markdown一致，并将Word渲染为页面图像逐页检查字体、公式、分页和引用。字体显示或标题样式需要修正时，应重新生成并重新检查最终版本。渲染图像仅作为本机检查资料，不作为Word正文的一部分。

本工具当前对应无表格、无插图的用户章节结构正文。若正文新增表格或插图，需在导出器中明确处理这些内容；工具会提示缺少的处理，不会静默省略。
