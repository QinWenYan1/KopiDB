"""读取计划清单，输出阶段总览或包含 PNG 的聊天消息。"""

import argparse
import base64
from dataclasses import dataclass
import json
from pathlib import Path
import re
import subprocess


SKILL = Path(__file__).resolve().parents[1]
DEFAULT_PLAN = SKILL.parents[1] / "PLAN.md"
OPEN = re.compile(r"^<!-- plan-progress: (.+?) -->$", re.MULTILINE)
CLOSE = re.compile(r"^<!-- /plan-progress -->$", re.MULTILINE)
TASK = re.compile(r"^\s*(?:[-*+]|\d+[.)])\s+\[([ xX])\]\s+(\S.*)")
HEADING = re.compile(r"^(?:\*\*(.+?)\*\*|#{1,6}\s+(.+?)\s*#*$)")


@dataclass
class Stage:
    name: str
    status: str = ""
    description: str = ""
    done: int = 0
    total: int = 0


def stage_key(name):
    # 表里可以写“① 建立每个 DB 的读写锁”，清单里可以简写为“① 库级锁”。
    return name[0] if name and "①" <= name[0] <= "⑳" else name


def stage_overview(lines):
    stages = {}
    columns = None
    for line in lines:
        if not line.strip().startswith("|"):
            columns = None
            continue
        cells = [c.strip() for c in re.split(r"(?<!\\)\|", line.strip().strip("|"))]
        if "阶段" in cells and "状态" in cells:
            stages = {}
            columns = cells
        elif columns:
            row = dict(zip(columns, cells))
            name = row.get("阶段", "")
            if name and not re.fullmatch(r":?-+:?", name):
                description = row.get("任务与改动") or row.get("主要代码", "")
                stages[stage_key(name)] = Stage(name, row.get("状态", ""), description)
    return stages


def checklist(plan):
    lines = []
    fence = ""
    for line in plan.read_text(encoding="utf-8").splitlines():
        stripped = line.strip()
        if fence:
            if stripped.startswith(fence) and not stripped.strip(fence[0]):
                fence = ""
            continue
        if opening := re.match(r"(`{3,}|~{3,})", stripped):
            fence = opening.group(1)
            continue
        lines.append(line)

    text = "\n".join(lines)
    overview = text
    title = "计划进度"
    starts, ends = list(OPEN.finditer(text)), list(CLOSE.finditer(text))
    if starts or ends:
        if len(starts) != 1 or len(ends) != 1 or starts[0].end() > ends[0].start():
            raise ValueError("请检查 PLAN 中的进度清单起止标记")
        title = starts[0].group(1)
        overview = text[:ends[0].start()]
        text = text[starts[0].end():ends[0].start()]
    stages = stage_overview(overview.splitlines())
    name = next(iter(stages.values())).name if len(stages) == 1 else title
    for line in text.splitlines():
        if heading := HEADING.match(line.strip()):
            label = heading.group(1) or heading.group(2)
            if not re.match(r"^步骤\s*\d", label):
                name = label
        elif task := TASK.match(line):
            key = stage_key(name)
            if key not in stages:
                stages[key] = Stage(name)
            stage = stages[key]
            stage.name = name
            if not stage.description:
                stage.description = task.group(2)
            stage.total += 1
            stage.done += task.group(1).lower() == "x"
    if not any(s.total for s in stages.values()):
        raise ValueError("PLAN 中没有可计数的验收项")
    return title, stages.values()


def percentage(done, total):
    if not total:
        return "—"
    percent = f"{100 * done / total:.1f}".removesuffix(".0") + "%"
    if 0 < done < total and percent in ("0%", "100%"):
        return "<0.1%" if percent == "0%" else ">99.9%"
    return percent


def progress_image(done, total):
    width = 100 * done / total
    svg = f"""<svg xmlns="http://www.w3.org/2000/svg" width="680" height="12">
  <rect width="100%" height="12" rx="6" fill="#c6c6c6"/>
  <rect width="{width:.8f}%" height="12" rx="6" fill="#454545"/>
</svg>"""
    png = subprocess.run(
        ["rsvg-convert"], input=svg.encode("ascii"), stdout=subprocess.PIPE, check=True
    ).stdout
    return "data:image/png;base64," + base64.b64encode(png).decode("ascii")


def render(plan, chat=False):
    title, stages = checklist(plan)
    done = sum(s.done for s in stages)
    total = sum(s.total for s in stages)
    summary = f"**{title}　{done}/{total}（{percentage(done, total)}）**"
    rows = [
        "| 阶段 | 任务与改动 | 已完成 | 完成度 | 状态 |",
        "| --- | --- | ---: | ---: | --- |",
    ]
    for stage in stages:
        status = stage.status
        if not status:
            status = "待开始" if not stage.done else "进行中"
            if stage.total and stage.done == stage.total:
                status = "已完成"
        name = re.sub(r"(?<!\\)\|", r"\\|", stage.name)
        description = re.sub(r"(?<!\\)\|", r"\\|", stage.description or "—")
        rows.append(
            f"| {name} | {description} | {stage.done}/{stage.total} | "
            f"{percentage(stage.done, stage.total)} | {status} |"
        )

    table = "\n".join(rows)
    if chat:
        message = {
            "summary": summary,
            "image_url": progress_image(done, total),
            "table": table,
        }
        print(json.dumps(message, ensure_ascii=False))
    else:
        print(f"{summary}\n\n{table}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--plan", type=Path, default=DEFAULT_PLAN)
    parser.add_argument("--chat", action="store_true", help="通过标准输出返回含进度图的 JSON")
    args = parser.parse_args()
    try:
        render(args.plan.resolve(), chat=args.chat)
    except (ValueError, OSError, subprocess.CalledProcessError) as error:
        parser.exit(1, f"{error}\n")
