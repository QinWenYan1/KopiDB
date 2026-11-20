"""校验发布的 Python wheel 本身，包括两个 Mach-O 文件的系统与加载要求。"""
import re
import subprocess
import sys
import sysconfig
import tempfile
from pathlib import Path
from zipfile import ZipFile


def require(ok, message):
    if not ok:
        raise SystemExit(message)


def output(*args):
    return subprocess.check_output(args, text=True)


def main(wheel, minimum):
    # macOS 11 起，一个主版本只使用 major_0 形式的平台标签。
    major = int(minimum.split(".")[0])
    tag = f"cp313-cp313-macosx_{major}_0_arm64"
    require(wheel.name.endswith(f"-{tag}.whl"), f"wheel 文件名与预期 {tag} 不符")
    lib = Path("kopidb/core/lib")
    engine = lib / "libkopidb.so"
    ext = lib / ("kopidb_pybind" + sysconfig.get_config_var("EXT_SUFFIX"))
    with ZipFile(wheel) as archive, tempfile.TemporaryDirectory() as tmp:
        names = set(archive.namelist())
        info, = [n.rsplit("/", 1)[0] for n in names if n.endswith(".dist-info/WHEEL")]
        metadata = archive.read(info + "/WHEEL").decode().splitlines()
        require("Tag: " + tag in metadata and "Root-Is-Purelib: false" in metadata,
                "wheel 内部标签与原生库不符")
        required = {"kopidb/__init__.py", "kopidb/api.py", "kopidb/py.typed",
                    "kopidb/core/__init__.py", "kopidb/core/__init__.pyi",
                    str(ext), str(engine), info + "/licenses/LICENSE"}
        for notice in ("toml11-LICENSE", "spdlog-LICENSE", "pybind11-LICENSE"):
            required.add(info + "/licenses/licenses/" + notice)
        require(required <= names, f"wheel 缺少文件: {required - names}")
        for member in (engine, ext):
            binary = Path(archive.extract(str(member), tmp))
            require(output("lipo", "-archs", binary).strip() == "arm64", f"{member} 架构不符")
            commands = output("otool", "-l", binary)
            target = re.search(r"^\s*minos (\S+)", commands, re.M)
            require(target and target.group(1) == minimum, f"{member} 最低 macOS 版本不符")
            deps = [line.strip().split(" (compatibility")[0]
                    for line in output("otool", "-L", binary).splitlines()[1:]]
            for dep in deps:
                require(dep == "@rpath/libkopidb.so" or dep.startswith(("/usr/lib/", "/System/Library/")),
                        f"{member} 依赖未打包的库: {dep}")
            if member == ext:
                require("@rpath/libkopidb.so" in deps and "path @loader_path (offset" in commands,
                        "扩展无法按同目录规则定位引擎库")


if __name__ == "__main__":
    main(Path(sys.argv[1]), sys.argv[2])
