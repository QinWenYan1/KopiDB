---
name: plan-progress
description: KopiDB 有当前计划时，在聊天中展示当轮总进度图和全部阶段的任务、完成数、百分比及状态。进度图仅在内存中生成。
---

# 计划进度条

有当前计划时，每轮修改、检查或汇报都调用本技能。只维护协作工具时同样展示业务计划的实际进度。以 PLAN 中的 `[x]` 和 `[ ]` 计算验收数。[逐函数计数约定](../goal-to-plan/SKILL.md#4-给阶段配验收标准)

1. 按本轮实际结果更新复选框和阶段状态。草案、排版预览或技能维护沿用业务计划的实际完成数。
2. 从仓库根目录用 `--chat` 运行渲染脚本。脚本读取 PLAN，在内存中绘制灰色进度条，经标准输出返回总计、PNG 数据和阶段表。仓库保存清单与渲染代码，进度条仅在聊天里显示。
3. 在同一个工具调用中按下面的示例直接显示图片。最终答复使用返回的总计和完整阶段表。表头沿用“阶段 / 任务与改动 / 已完成 / 完成度 / 状态”。每个阶段都占一行，包括尚未开始的阶段。
4. 业务实施时，在表下按[逐函数的代码排版](../lab-workflow/SKILL.md#4-一次讲清一个函数)展开本轮步骤。维护或收尾时只说明实际改动与当前准备状态。详细验收记录由助手维护，遇到需要用户处理的问题就指出具体位置。

```js
const result = await tools.exec_command({
  cmd: "python3 agent/skills/plan-progress/scripts/render.py --chat",
  max_output_tokens: 4000,
});
if (result.exit_code !== 0) throw new Error(result.output);
const report = JSON.parse(result.output);
text(report.summary);
image(report.image_url);
text(report.table);
```

脚本默认读取 `agent/PLAN.md`，可用 `--plan 文件路径` 指定另一份计划。清单前后的 `<!-- plan-progress: 简短计划名 -->` 和 `<!-- /plan-progress -->` 可限定计数范围。按粗体标题或 Markdown 小标题分组，阶段状态取自 PLAN 的“阶段 / 状态”表，两处使用相同名称或相同圈号。单阶段清单也可直接使用。代码块中的示例会被跳过。

阶段百分比按本阶段的验收项计算，总百分比按所有验收项计算。零项通过时显示空轨道。在终端直接运行脚本可查看纯文本总计与 Markdown 阶段表。
