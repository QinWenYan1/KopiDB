#!/bin/bash
set -euo pipefail

# 从脚本位置定位仓库；编译和测试均在临时副本中完成。
repo_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
report_dir="$repo_dir/coverage-report"
usage() {
  cat <<'HELP'
用法: bash test_coverage.sh [--filter GTest模式] [test_target ...]
      bash test_coverage.sh --all [--filter GTest模式]

不指定目标时，运行全部 test_* 模块的常规用例，排除以下四项：
  LSMTest.BigPersistence
  LSMTest.BigPersistence2
  CompactTest.TombstoneDefaultConfig
  WiscKeyCompactionBenchmarkDeathTest.MeasuresReferenceCompactionCost
--all       运行 Xmake 中所有 test_* 二进制，包括大规模和耗时测试。
test_target 显式指定目标时，不应用上述四项排除；可配合 --filter 使用。
--filter    指定 GTest 过滤器；未指定时使用 GTEST_FILTER，默认 *。
--help, -h  显示帮助。

当前支持 macOS，使用 xcrun 提供的 Apple Clang/LLVM 工具。
报告: 仓库/coverage-report/index.html
范围仅为所选测试已链接的 src/include 插桩代码，不代表全引擎。
HELP
}
fail() {
  printf '覆盖率失败: %s\n' "$*" >&2
  exit 1
}

selection="default"
requested=()
requested_count=0
test_filter="${GTEST_FILTER-*}"
while (( $# > 0 )); do
  case "$1" in
    --help|-h) usage; exit 0 ;;
    --all)
      [[ "$selection" != "explicit" ]] || fail "--all 不能与目标列表同时使用。"
      selection="all"; shift ;;
    --filter)
      (( $# >= 2 )) || fail "--filter 后需要一个 GTest 模式。"
      test_filter="$2"; shift 2 ;;
    --filter=*) test_filter="${1#--filter=}"; shift ;;
    --*) fail "未知选项: $1（使用 --help 查看用法）。" ;;
    *)
      [[ "$selection" != "all" ]] || fail "--all 不能与目标列表同时使用。"
      selection="explicit"
      requested[requested_count]="$1"
      requested_count=$((requested_count + 1))
      shift ;;
  esac
done

# 默认覆盖全部模块，只排除已确认的大规模/基准用例；不按 Large 等
# 模糊名称过滤，避免把小规模边界测试和正常持久化检查一起排除。
# 显式指定目标或 --all 时不应用本表，仍由调用者自己的过滤器决定。
default_exclusions=(
  LSMTest.BigPersistence
  LSMTest.BigPersistence2
  CompactTest.TombstoneDefaultConfig
  WiscKeyCompactionBenchmarkDeathTest.MeasuresReferenceCompactionCost
)
default_exclusion_reasons=(
  '约200万条写入、删除及重开逐项查询'
  '约100万条乱序写入、批量删除及重开查询'
  '约100万条记录的墓碑持久化检查'
  '10000条4KiB值的合并基准，预热1轮并采样5轮'
)
requested_filter="$test_filter"
if [[ "$selection" == "default" && -n "$test_filter" ]]; then
  exclusions_filter="$(IFS=:; printf '%s' "${default_exclusions[*]}")"
  if [[ "$test_filter" == *-* ]]; then
    # GTest 的第一个 - 后是排除列表；追加到已有列表，保留正向匹配。
    test_filter="$test_filter:$exclusions_filter"
  else
    test_filter="$test_filter-$exclusions_filter"
  fi
fi
# 空过滤器继续表示零匹配，不能拼成 -排除列表而意外运行全部用例。

# 先检查工具和输入，再创建工作区；缺少依赖不触碰旧报告。
[[ "$(uname -s)" == "Darwin" ]] || fail "当前仅支持 macOS 原生 LLVM 覆盖率。"
for tool in xmake xcrun rsync mktemp tee find; do
  command -v "$tool" >/dev/null 2>&1 || fail "缺少工具: $tool"
done
xmake_bin="$(command -v xmake)"
llvm_cov="$(xcrun --find llvm-cov)" || fail "xcrun 找不到 llvm-cov，请安装 Xcode Command Line Tools。"
llvm_profdata="$(xcrun --find llvm-profdata)" || fail "xcrun 找不到 llvm-profdata。"
clang_bin="$(xcrun --find clang)" || fail "xcrun 找不到 clang。"
clangxx_bin="$(xcrun --find clang++)" || fail "xcrun 找不到 clang++。"
for tool in "$llvm_cov" "$llvm_profdata" "$clang_bin" "$clangxx_bin"; do
  [[ -x "$tool" ]] || fail "工具不可执行: $tool"
done
for input in src include test example packaging xmake.lua config.toml sdk/kopidb_pybind.cpp; do
  [[ -e "$repo_dir/$input" ]] || fail "缺少构建输入: $input"
done
[[ ! -L "$report_dir" && ( ! -e "$report_dir" || -d "$report_dir" ) ]] ||
  fail "报告路径必须是普通目录: $report_dir"

work_dir=""
pending_report=""
previous_report=""
published=0
cleanup() {
  local status=$?
  trap - EXIT
  # 替换目录中途失败时恢复旧报告，只清理本脚本创建的路径。
  if [[ -n "$previous_report" && -d "$previous_report/report" && ! -e "$report_dir" ]]; then
    mv "$previous_report/report" "$report_dir" || true
  fi
  if [[ -n "$previous_report" ]]; then
    if [[ -d "$previous_report/report" && "$published" != 1 ]]; then
      printf '旧报告保留在: %s/report\n' "$previous_report" >&2
    else
      rm -rf "$previous_report" || true
    fi
  fi
  [[ -z "$pending_report" ]] || rm -rf "$pending_report" || true
  if [[ -n "$work_dir" ]]; then
    if (( status == 0 )); then
      rm -rf "$work_dir" || true
    else
      rm -rf "$work_dir/source" "$work_dir/runs" || true
      printf '本次未发布成功报告；日志、JSON 和 profile 保留在: %s/evidence\n' "$work_dir" >&2
    fi
  fi
  exit "$status"
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

work_dir="$(mktemp -d "${TMPDIR:-/tmp}/kopidb-coverage.XXXXXX")"
# 统一 /var 与 /private/var 别名，使覆盖率路径和源码选择一致。
work_dir="$(cd "$work_dir" && pwd -P)"
source_dir="$work_dir/source"
evidence_dir="$work_dir/evidence"
mkdir -p "$source_dir/sdk" "$evidence_dir/logs" "$evidence_dir/gtest" "$evidence_dir/profiles"
summary_file="$evidence_dir/run-summary.txt"
{
  printf 'KopiDB LLVM 覆盖率运行\n开始时间: %s\n' "$(date '+%Y-%m-%d %H:%M:%S %z')"
  printf '目标选择: %s\n请求的 GTest 过滤器: %s\n实际 GTest 过滤器: %s\n' \
    "$selection" "$requested_filter" "$test_filter"
  printf '范围: 仅所选测试已链接的 src/include 插桩代码；未链接文件不在分母中。\n'
  printf '进程限制: _exit/异常终止子进程的计数不保证写回，报告不是完整崩溃路径覆盖率。\n'
  printf '运行方式: 各目标独立目录；固定一轮；关闭继承的 GTest 分片/list 模式。\n'
  printf '状态: 尚未完成，只有全部选中目标成功后才发布报告。\n\n'
  "$clang_bin" --version
  "$llvm_cov" --version
  "$llvm_profdata" --version
} > "$summary_file"

printf 'test\treason\n' > "$evidence_dir/excluded-tests.tsv"
if [[ "$selection" == "default" ]]; then
  printf '\n默认排除的大规模/基准用例（排除不是通过，也不是 GTest 跳过）:\n' | tee -a "$summary_file"
  for (( index=0; index<${#default_exclusions[@]}; index++ )); do
    printf '%s\t%s\n' "${default_exclusions[index]}" "${default_exclusion_reasons[index]}" \
      >> "$evidence_dir/excluded-tests.tsv"
    printf '  %s: %s\n' "${default_exclusions[index]}" "${default_exclusion_reasons[index]}" \
      | tee -a "$summary_file"
  done
else
  printf '\n未应用默认排除清单；本次测试范围由所选目标和请求的过滤器决定。\n' | tee -a "$summary_file"
fi

printf '[1/5] 复制当前源码，隔离原仓库 build/.xmake 与测试数据\n'
rsync -a --exclude='.DS_Store' --exclude='__pycache__/' --exclude='build/' --exclude='dist/' \
  "$repo_dir/src" "$repo_dir/include" "$repo_dir/test" \
  "$repo_dir/example" "$repo_dir/packaging" "$source_dir/"
cp "$repo_dir/xmake.lua" "$repo_dir/config.toml" "$source_dir/"
cp "$repo_dir/sdk/kopidb_pybind.cpp" "$source_dir/sdk/"
cp "$source_dir/config.toml" "$evidence_dir/config.toml"

# 由 Xmake 解析目标/程序路径和 GTest JSON，不猜测架构路径，
# 不额外依赖 Python/jq。辅助 Lua 只存在于本次临时工作区。
cat > "$work_dir/metadata.lua" <<'LUA'
import("core.project.config")
import("core.project.project")
import("core.base.json")

function main(action, input, output)
    if action == "targets" then
        config.load()
        local names = {}
        for name, target in pairs(project.targets()) do
            if name:startswith("test_") and target:kind() == "binary" then
                assert(name:match("^[%w_%-]+$"), "测试目标名不能用于文件名: " .. name)
                table.insert(names, name)
            end
        end
        table.sort(names)
        assert(#names > 0, "Xmake 中没有 test_* 二进制目标")
        local lines = {}
        for _, name in ipairs(names) do
            local binary = path.absolute(assert(project.target(name):targetfile()))
            assert(not binary:find("[\r\n\t]"), "测试程序路径含有控制字符")
            table.insert(lines, name .. "\t" .. binary)
        end
        io.writefile(input, table.concat(lines, "\n") .. "\n")
    elseif action == "results" then
        local result = assert(json.loadfile(input), "缺少有效 GTest JSON")
        local selected = assert(tonumber(result.tests), "GTest JSON 缺少 tests")
        local executed, passed, failed, skipped = 0, 0, 0, 0
        for _, suite in ipairs(result.testsuites or {}) do
            for _, test in ipairs(suite.testsuite or {}) do
                if test.status == "RUN" then
                    executed = executed + 1
                    if test.result == "SKIPPED" then
                        skipped = skipped + 1
                    elseif test.failures and #test.failures > 0 then
                        failed = failed + 1
                    else
                        assert(test.result == "COMPLETED", "未知 GTest result")
                        passed = passed + 1
                    end
                end
            end
        end
        assert(failed == tonumber(result.failures), "GTest 失败数与明细不一致")
        io.writefile(output, string.format("%d\t%d\t%d\t%d\t%d\t%d\n",
            selected, executed, passed, failed, skipped, tonumber(result.disabled) or 0))
    else
        raise("未知元数据操作: " .. action)
    end
end
LUA

printf '[2/5] 配置 Apple Clang coverage，并读取 Xmake 测试元数据\n'
(
  cd "$source_dir"
  # Xcode 工具链也支持 Command Line Tools，并负责加入 macOS SDK 路径。
  # 通用 clang 工具链不会补齐这些路径，可能连 <string> 都找不到。
  "$xmake_bin" f -y -p macosx -m coverage --toolchain=xcode \
    --cc="$clang_bin" --cxx="$clangxx_bin" --ld="$clangxx_bin" --sh="$clangxx_bin"
  "$xmake_bin" lua "$work_dir/metadata.lua" targets "$evidence_dir/available-targets.tsv"
) 2>&1 | tee "$evidence_dir/logs/configure.log"

if [[ "$selection" == "default" || "$selection" == "all" ]]; then
  while IFS=$'\t' read -r target binary; do
    requested[requested_count]="$target"
    requested_count=$((requested_count + 1))
  done < "$evidence_dir/available-targets.tsv"
  if [[ "$selection" == "default" ]]; then
    printf '默认常规集合: 全部测试模块，排除上方四项大规模/基准用例。\n'
  else
    printf '已选择全部测试目标，不应用默认的大规模/基准排除清单。\n'
  fi
fi
(( requested_count > 0 )) || fail "没有选中测试目标。"
selected_targets=()
selected_binaries=()
selected_count=0
for requested_target in "${requested[@]}"; do
  found=0
  while IFS=$'\t' read -r target binary; do
    if [[ "$target" == "$requested_target" ]]; then
      found=1
      duplicate=0
      if (( selected_count > 0 )); then
        for selected_target in "${selected_targets[@]}"; do
          [[ "$selected_target" != "$target" ]] || duplicate=1
        done
      fi
      if (( duplicate == 0 )); then
        selected_targets[selected_count]="$target"
        selected_binaries[selected_count]="$binary"
        selected_count=$((selected_count + 1))
        printf '%s\t%s\n' "$target" "$binary" >> "$evidence_dir/selected-targets.tsv"
      fi
      break
    fi
  done < "$evidence_dir/available-targets.tsv"
  if (( found == 0 )); then
    printf '可用目标:\n' >&2
    cat "$evidence_dir/available-targets.tsv" >&2
    fail "未知测试目标: $requested_target"
  fi
done
{
  printf '\n选中目标数: %d\n目标: ' "$selected_count"
  printf '%s ' "${selected_targets[@]}"
  printf '\nGTest 过滤器: %s\n' "$test_filter"
} | tee -a "$summary_file"

printf '[3/5] 仅构建所选测试及其依赖\n'
(
  cd "$source_dir"
  for target in "${selected_targets[@]}"; do
    "$xmake_bin" build -y "$target"
  done
) 2>&1 | tee "$evidence_dir/logs/build.log"

printf '[4/5] 在独立目录逐个运行测试并收集 profile\n'
printf 'target\tselected\texecuted\tpassed\tfailed\tskipped\tdisabled\texit_code\n' > "$evidence_dir/test-results.tsv"
total_executed=0
total_passed=0
total_failed=0
total_skipped=0
total_disabled=0
completed_targets=0
for (( index=0; index<selected_count; index++ )); do
  target="${selected_targets[index]}"
  binary="${selected_binaries[index]}"
  [[ -x "$binary" ]] || fail "Xmake 报告的测试程序不存在或不可执行: $binary"
  run_dir="$work_dir/runs/$target"
  profile_dir="$evidence_dir/profiles/$target"
  mkdir -p "$run_dir" "$profile_dir"
  cp "$source_dir/config.toml" "$run_dir/config.toml"
  printf '\n运行 %s（过滤器: %s）\n' "$target" "$test_filter"
  test_status=0
  if (
    cd "$run_dir"
    unset GTEST_TOTAL_SHARDS GTEST_SHARD_INDEX GTEST_SHARD_STATUS_FILE GTEST_REPEAT GTEST_LIST_TESTS
    export LLVM_PROFILE_FILE="$profile_dir/%p-%m.profraw"
    "$binary" "--gtest_filter=$test_filter" --gtest_repeat=1 --gtest_list_tests=0 \
      "--gtest_output=json:$evidence_dir/gtest/$target.json"
  ) 2>&1 | tee "$evidence_dir/logs/$target.log"; then
    test_status=0
  else
    pipeline_status=("${PIPESTATUS[@]}")
    test_status="${pipeline_status[0]}"
    (( pipeline_status[1] == 0 )) || fail "无法保存 $target 的运行日志。"
  fi
  [[ -s "$evidence_dir/gtest/$target.json" ]] || fail "$target 未生成 GTest JSON（退出码 $test_status）。"
  (
    cd "$source_dir"
    "$xmake_bin" lua "$work_dir/metadata.lua" results \
      "$evidence_dir/gtest/$target.json" "$evidence_dir/gtest/$target.counts.tsv"
  ) 2>&1 | tee -a "$evidence_dir/logs/$target.log"
  IFS=$'\t' read -r matched executed passed failed skipped disabled < "$evidence_dir/gtest/$target.counts.tsv"
  printf '%s\t%d\t%d\t%d\t%d\t%d\t%d\t%d\n' \
    "$target" "$matched" "$executed" "$passed" "$failed" "$skipped" "$disabled" "$test_status" >> "$evidence_dir/test-results.tsv"
  completed_targets=$((completed_targets + 1))
  total_executed=$((total_executed + executed))
  total_passed=$((total_passed + passed))
  total_failed=$((total_failed + failed))
  total_skipped=$((total_skipped + skipped))
  total_disabled=$((total_disabled + disabled))
  printf '%s: 匹配 %d，执行 %d，通过 %d，失败 %d，跳过 %d，禁用 %d，退出码 %d\n' \
    "$target" "$matched" "$executed" "$passed" "$failed" "$skipped" "$disabled" "$test_status" | tee -a "$summary_file"
  (( test_status == 0 && failed == 0 )) || fail "$target 测试失败，不发布覆盖率报告。"
  (( executed > 0 && passed > 0 )) || fail "$target 没有实际完成的测试，请检查 GTest 过滤器。"
  profiles=("$profile_dir/"*.profraw)
  [[ -s "${profiles[0]}" ]] || fail "$target 没有生成有效 LLVM profile，请检查 coverage 插桩。"
done
(( total_executed > 0 )) || fail "实际执行测试数为零。"

printf '[5/5] 合并计数并生成可离线查看的 src/include HTML\n'
profiles=("$evidence_dir/profiles/"*/*.profraw)
"$llvm_profdata" merge -sparse "${profiles[@]}" -o "$evidence_dir/coverage.profdata" \
  2>&1 | tee "$evidence_dir/logs/merge.log"
coverage_args=("${selected_binaries[0]}" "-instr-profile=$evidence_dir/coverage.profdata")
for (( index=1; index<selected_count; index++ )); do
  coverage_args+=("-object=${selected_binaries[index]}")
done
source_files=()
source_count=0
while IFS= read -r -d '' source_file; do
  source_files[source_count]="$source_file"
  source_count=$((source_count + 1))
done < <(find "$source_dir/src" "$source_dir/include" -type f -print0)
(( source_count > 0 )) || fail "源码快照中没有 src/include 文件。"
pending_report="$(mktemp -d "$repo_dir/.coverage-report.XXXXXX")"
"$llvm_cov" report "${coverage_args[@]}" --sources "${source_files[@]}" \
  2> "$evidence_dir/logs/report.log" | tee "$evidence_dir/coverage-summary.txt"
"$llvm_cov" show "${coverage_args[@]}" -format=html -output-dir="$pending_report" \
  -project-title="KopiDB: selected tests, linked src/include only" \
  -show-line-counts-or-regions -show-branches=count --sources "${source_files[@]}" \
  > "$evidence_dir/logs/html.log" 2>&1
[[ -s "$pending_report/index.html" ]] || fail "LLVM 没有生成 HTML 首页。"
{
  printf '\n完成目标: %d/%d\n' "$completed_targets" "$selected_count"
  printf '测试合计: 执行 %d，通过 %d，失败 %d，跳过 %d，禁用 %d\n' \
    "$total_executed" "$total_passed" "$total_failed" "$total_skipped" "$total_disabled"
  printf '结果: 成功；全部所选目标已执行，未扩大为全引擎覆盖率结论。\n'
  printf 'HTML 已嵌入源码并使用报告目录内资源；删除临时源码后仍可查看。\n'
} | tee -a "$summary_file"
cp -R "$evidence_dir/." "$pending_report/"

# HTML 与日志全部写好后再替换；旧报告暂存于同文件系统，失败可恢复。
if [[ -d "$report_dir" ]]; then
  previous_report="$(mktemp -d "$repo_dir/.coverage-previous.XXXXXX")"
  mv "$report_dir" "$previous_report/report"
fi
mv "$pending_report" "$report_dir"
pending_report=""
published=1
printf '\n覆盖率报告: %s/index.html\n运行摘要: %s/run-summary.txt\n' "$report_dir" "$report_dir"
