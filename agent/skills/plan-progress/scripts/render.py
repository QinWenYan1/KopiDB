"""从验收清单生成阶段总览，复用或更新聊天进度图。"""

import argparse
from dataclasses import dataclass
from html import escape
from pathlib import Path
import re
import subprocess
from string import Template
from tempfile import TemporaryDirectory


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


def progress_image(output, title, done, total):
    template = Template((SKILL / "assets/bar.svg").read_text(encoding="utf-8"))
    image = template.substitute(title=escape(title), done=done, total=total,
                                width=f"{100 * done / total:.8f}".rstrip("0").rstrip("."))
    svg = output / "plan-progress.svg"
    png = output / "plan-progress.png"
    # SVG 记录本次绘图的输入。报告中的阶段状态每次重算，图相同时直接复用。
    if svg.is_file() and png.is_file():
        png_info = png.stat()
        if (png_info.st_size and png_info.st_mtime_ns <= svg.stat().st_mtime_ns
                and svg.read_text(encoding="utf-8") == image):
            return png
    output.mkdir(exist_ok=True)
    with TemporaryDirectory(dir=output) as temp:
        new_png = Path(temp) / png.name
        new_svg = Path(temp) / svg.name
        subprocess.run(["rsvg-convert", "-o", str(new_png)], input=image.encode("utf-8"), check=True)
        new_svg.write_text(image, encoding="utf-8")
        new_png.replace(png)
        new_svg.replace(svg)
    return png


def render(plan):
    title, stages = checklist(plan)
    done = sum(s.done for s in stages)
    total = sum(s.total for s in stages)
    png = progress_image(plan.parent / "visuals", title, done, total)
    print(f"**{title}　{done}/{total}（{percentage(done, total)}）**")
    print(f"\n![{title}，已完成 {done}/{total}](<{png}>)\n")
    print("| 阶段 | 任务与改动 | 已完成 | 完成度 | 状态 |")
    print("| --- | --- | ---: | ---: | --- |")
    for stage in stages:
        status = stage.status
        if not status:
            status = "待开始" if not stage.done else "进行中"
            if stage.total and stage.done == stage.total:
                status = "已完成"
        name = re.sub(r"(?<!\\)\|", r"\\|", stage.name)
        description = re.sub(r"(?<!\\)\|", r"\\|", stage.description or "—")
        print(f"| {name} | {description} | {stage.done}/{stage.total} | {percentage(stage.done, stage.total)} | {status} |")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--plan", type=Path, default=DEFAULT_PLAN)
    try:
        render(parser.parse_args().plan.resolve())
    except (ValueError, OSError, subprocess.CalledProcessError) as error:
        parser.exit(1, f"{error}\n")
