#!/bin/bash
set -euo pipefail

# 从脚本自身定位仓库，允许在其他目录执行；输出目录也可单独指定。
repo_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
if [[ "${1:-}" == "--help" || "${1:-}" == "-h" ]]; then
  printf '用法: %s [输出目录]\n默认输出到仓库的 dist/，当前支持 macOS arm64。\n' "$0"
  exit 0
fi
if (( $# > 1 )); then
  printf '错误: 只接受一个可选的输出目录。\n' >&2
  exit 1
fi

fail() {
  printf '打包失败: %s\n' "$*" >&2
  exit 1
}

[[ "$(uname -s)" == "Darwin" && "$(uname -m)" == "arm64" ]] ||
  fail "当前仅支持在 macOS arm64 上生成 SDK。"
for tool in xmake rsync tar lipo otool xcrun; do
  command -v "$tool" >/dev/null 2>&1 || fail "缺少工具: $tool"
done
xmake_bin="$(command -v xmake)"
output_dir="${1:-$repo_dir/dist}"
mkdir -p "$output_dir"
output_dir="$(cd "$output_dir" && pwd -P)"

# 只删除本次 mktemp 创建的目录/文件。失败或被中断时，也不会删除旧正式包。
work_dir=""
pending_archive=""
cleanup() {
  local status=$?
  if [[ -n "$pending_archive" ]]; then
    rm -f "$pending_archive" || true
  fi
  if [[ -n "$work_dir" ]]; then
    rm -rf "$work_dir" || true
  fi
  if (( status != 0 )); then
    printf '打包未完成，原有正式压缩包保持不变。\n' >&2
  fi
  exit "$status"
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

work_dir="$(mktemp -d "${TMPDIR:-/tmp}/kopidb-package.XXXXXX")"
source_dir="$work_dir/source"
metadata_dir="$work_dir/metadata"
mkdir -p "$source_dir/sdk" "$metadata_dir"

printf '[1/4] 复制当前源码到临时目录\n'
# 复制工作树内容，包含尚未提交的源码；不使用 git archive，也不借用仓库的
# build/.xmake。SDK 的旧 Python 二进制不属于本次 C++ 构建输入。
rsync -a --exclude='.DS_Store' --exclude='__pycache__/' \
  --exclude='build/' --exclude='dist/' \
  "$repo_dir/src" "$repo_dir/include" "$repo_dir/test" \
  "$repo_dir/example" "$repo_dir/packaging" "$source_dir/"
cp "$repo_dir/xmake.lua" "$repo_dir/LICENSE" \
  "$repo_dir/README.md" "$repo_dir/config.toml" "$source_dir/"
cp "$repo_dir/sdk/kopidb_pybind.cpp" "$source_dir/sdk/"

# 通过 Xmake 读取实际项目/依赖信息，避免在 shell 中猜测 Lua 配置内容。
# 此临时脚本只处理当前共享库使用的依赖；新增库依赖时应补齐对应许可收集。
cat > "$work_dir/metadata.lua" <<'LUA'
import("core.project.config")
import("core.project.project")

function main(output_dir)
    config.load()
    local target = assert(project.target("kopidb_shared"))
    local version = assert(project.version(), "项目未设置版本号")
    io.writefile(path.join(output_dir, "version"), version .. "\n")
    os.mkdir(path.join(output_dir, "licenses"))
    local compiler = assert(target:tool("cxx"))
    local info = {
        "KopiDB version: " .. version,
        "Platform: macos-arm64",
        "Build mode: release",
        "C++ standard: C++20",
        "Compiler: " .. compiler,
        os.iorunv(compiler, {"--version"}):trim(),
        "",
        "Dependencies:"
    }

    -- orderpkgs 只读取 kopidb_shared 的实际依赖，不将测试用 gtest 或
    -- Python 绑定用 pybind11 的构建依赖作为本包的分发内容。
    for _, pkg in ipairs(target:orderpkgs()) do
        local manifest = assert(io.load(path.join(pkg:installdir(), "manifest.txt")))
        assert(pkg:license() == "MIT", "依赖许可发生变化，请更新许可收集: " .. pkg:name())
        assert(not manifest.librarydeps or #manifest.librarydeps == 0,
               "新增了传递库依赖，请补齐许可收集: " .. pkg:name())
        table.insert(info, string.format("%s %s (%s)", pkg:name(), pkg:get("version"), pkg:license()))

        if pkg:name() == "toml11" then
            -- toml11 在安装后的主头文件中保留了完整 MIT 许可，直接提取
            -- 本次构建所用版本的文本，不依赖可能已被清理的下载缓存。
            local lines = {}
            local collecting = false
            for line in io.lines(path.join(pkg:installdir(), "include", "toml.hpp")) do
                if line:startswith("// The MIT License") then
                    collecting = true
                end
                if collecting then
                    if not line:startswith("//") then
                        break
                    end
                    table.insert(lines, (line:gsub("^// ?", "")))
                end
            end
            local license = table.concat(lines, "\n")
            assert(license:find("THE SOFTWARE%.$"), "未找到完整 toml11 许可")
            io.writefile(path.join(output_dir, "licenses", "toml11-LICENSE"), license .. "\n")
        elseif pkg:name() == "spdlog" then
            -- 安装树没有保留 spdlog 自身的 LICENSE；仓库保存了从 v1.17.0
            -- 核对的上游原文：https://github.com/gabime/spdlog/blob/v1.17.0/LICENSE
            os.cp("packaging/cpp/licenses/spdlog-LICENSE", path.join(output_dir, "licenses"))
            local opts = manifest.configs
            if not opts.std_format and not opts.fmt_external and not opts.fmt_external_ho then
                local bundled = path.join(pkg:installdir(), "include", "spdlog", "fmt", "bundled")
                os.cp(path.join(bundled, "fmt.license.rst"), path.join(output_dir, "licenses"))
                local header = path.join(bundled, "base.h")
                if not os.isfile(header) then
                    header = path.join(bundled, "core.h")
                end
                local fmt_version = assert(tonumber(io.readfile(header):match("#define FMT_VERSION (%d+)")))
                table.insert(info, string.format("fmt %d.%d.%d (bundled with spdlog)",
                    math.floor(fmt_version / 10000), math.floor(fmt_version / 100) % 100, fmt_version % 100))
            end
        else
            error("请补齐新增依赖的许可收集: " .. pkg:name())
        end
    end
    io.writefile(path.join(output_dir, "BUILD-INFO.txt"), table.concat(info, "\n") .. "\n")
end
LUA

printf '[2/4] 配置并构建 Release 共享库\n'
(
  cd "$source_dir"
  "$xmake_bin" f -y -p macosx -a arm64 -m release
  "$xmake_bin" build -y kopidb_shared
  "$xmake_bin" lua "$work_dir/metadata.lua" "$metadata_dir"
)
version="$(cat "$metadata_dir/version")"
# 版本号用于文件名，只接受普通版本字符，不能让它变成路径。
[[ "$version" =~ ^[0-9][a-zA-Z0-9._+-]*$ ]] || fail "无效版本号: $version"
package_name="kopidb-$version-macos-arm64"
package_dir="$work_dir/stage/$package_name"
mkdir -p "$package_dir"

printf '[3/4] 安装并整理 SDK 文件\n'
(
  cd "$source_dir"
  "$xmake_bin" install -o "$package_dir" kopidb_shared
)
cp "$source_dir/LICENSE" "$package_dir/"
cp -R "$metadata_dir/licenses" "$package_dir/"
# 第六步提供专用包内说明后自动采用；此前保留现有项目说明，不捏造下载链接。
if [[ -f "$source_dir/packaging/cpp/README.md" ]]; then
  cp "$source_dir/packaging/cpp/README.md" "$package_dir/README.md"
else
  cp "$source_dir/README.md" "$package_dir/README.md"
fi

for required in include/kopidb/lsm/engine.h lib/libkopidb.so \
  xmake.lua KopiDBConfig.cmake LICENSE README.md licenses/spdlog-LICENSE licenses/toml11-LICENSE; do
  [[ -s "$package_dir/$required" ]] || fail "安装目录缺少文件: $required"
done
[[ "$(lipo -archs "$package_dir/lib/libkopidb.so")" == "arm64" ]] ||
  fail "共享库架构与包名不一致。"

# 当前 SDK 只分发 KopiDB 库。若引入其他动态库，就不能依赖开发机上的路径
# 默默发布；先报错，待补齐那份库及其运行路径后再打包。
otool -L "$package_dir/lib/libkopidb.so" > "$work_dir/dependencies.txt"
while IFS= read -r dependency; do
  case "$dependency" in
    @rpath/libkopidb.so|/usr/lib/*|/System/Library/*) ;;
    *) fail "尚未打包的动态库依赖: $dependency" ;;
  esac
done < <(awk 'NR > 1 {print $1}' "$work_dir/dependencies.txt")

otool -l "$package_dir/lib/libkopidb.so" > "$work_dir/load-commands.txt"
minimum_macos="$(awk '
  /cmd LC_BUILD_VERSION/ {build_version=1}
  build_version && /minos / {print $2; exit}
  /cmd LC_VERSION_MIN_MACOSX/ {legacy_version=1}
  legacy_version && /version / {print $2; exit}
' "$work_dir/load-commands.txt")"
[[ -n "$minimum_macos" ]] || fail "无法读取共享库的最低 macOS 版本。"
{
  cat "$metadata_dir/BUILD-INFO.txt"
  printf '\nBuild host macOS: %s\n' "$(sw_vers -productVersion)"
  printf 'Apple SDK: %s\n' "$(xcrun --show-sdk-version)"
  printf 'Library minimum macOS: %s\n' "$minimum_macos"
  printf '\nDynamic dependencies:\n'
  sed '1d' "$work_dir/dependencies.txt"
} > "$package_dir/BUILD-INFO.txt"

printf '[4/4] 生成压缩包\n'
# 先写同一输出目录下的临时归档。tar 完成且目录清单可读后才替换正式文件，
# 失败时 EXIT trap 只清理临时文件，保留上一次成功生成的包。
archive_path="$output_dir/$package_name.tar.gz"
[[ ! -d "$archive_path" ]] || fail "归档输出路径已是目录: $archive_path"
pending_archive="$(mktemp "$output_dir/.$package_name.XXXXXX")"
COPYFILE_DISABLE=1 tar -czf "$pending_archive" -C "$work_dir/stage" "$package_name"
tar -tzf "$pending_archive" >/dev/null
chmod 644 "$pending_archive"
mv -f "$pending_archive" "$archive_path"
pending_archive=""
printf '打包完成: %s\n' "$archive_path"
