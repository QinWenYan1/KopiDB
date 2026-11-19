-- 定义项目
set_project("kopidb")
set_version("0.0.1")
set_languages("c++20")

add_rules("mode.debug", "mode.release")

-- macOS 直接使用 Apple Clang 配套的 LLVM 工具收集覆盖率、生成网页。
-- Xmake 内置 mode.coverage 使用 gcov 的 --coverage，不能与这套参数混用。
-- 只影响 coverage 构建；日常 debug/release 和发布的 SDK 不带这些统计。
if is_plat("macosx") then
    if is_mode("coverage") then
        set_symbols("debug")
        set_optimize("none")
        set_policy("build.ccache", false)
        add_cxflags("-fprofile-instr-generate", "-fcoverage-mapping")
        add_ldflags("-fprofile-instr-generate")
        add_shflags("-fprofile-instr-generate")
    end
else
    -- 其他平台保留 Xmake 原有的 gcov 配置；本轮脚本仅验收 macOS。
    add_rules("mode.coverage")
end

-- add_repositories("local-repo build")  -- 作者本地的私有仓库残留，build/ 不在 git 中，禁用

add_requires("gtest")
add_requires("pybind11")
add_requires("spdlog", { system = false })
add_requires("toml11", { system = false })

if is_mode("debug") then
    add_defines("LSM_DEBUG")
end

target("logger")
    set_kind("static")
    add_files("src/logger/*.cpp")
    add_packages("spdlog")
    add_includedirs("include", {public = true})
    
target("config")
    set_kind("static")
    add_files("src/config/*.cpp")
    add_packages("toml11", "spdlog")
    add_includedirs("include", {public = true})

target("utils")
    set_kind("static")
    add_files("src/utils/*.cpp")
    add_packages("toml11", "spdlog")
    add_includedirs("include", {public = true})

target("vlog")
    set_kind("static")
    add_deps("utils", "config")
    add_files("src/vlog/*.cpp")
    add_packages("toml11", "spdlog")
    add_includedirs("include", {public = true})

target("iterator")
    set_kind("static")
    add_files("src/iterator/*.cpp")
    add_packages("toml11", "spdlog")
    add_includedirs("include", {public = true})

target("skiplist")
    set_kind("static")
    add_files("src/skiplist/*.cpp")
    add_packages("toml11", "spdlog")
    add_includedirs("include", {public = true})

target("memtable")
    set_kind("static")
    add_deps("skiplist", "iterator", "config", "sst")
    add_packages("toml11", "spdlog")
    add_files("src/memtable/*.cpp")
    add_includedirs("include", {public = true})

target("block")
    set_kind("static")
    add_deps("config")
    add_files("src/block/*.cpp")
    add_packages("toml11", "spdlog")
    add_includedirs("include", {public = true})

target("sst")
    set_kind("static")
    add_deps("block", "utils", "iterator", "vlog")
    add_files("src/sst/*.cpp")
    add_packages("toml11", "spdlog")
    add_includedirs("include", {public = true})

target("wal")
    set_kind("static")
    add_deps("sst", "memtable")
    add_files("src/wal/*.cpp")
    add_packages("toml11", "spdlog")
    add_includedirs("include", {public = true})

target("kopidb")
    set_kind("static")
    add_deps("sst", "memtable", "wal", "logger")
    add_files("src/lsm/*.cpp")
    add_packages("toml11", "spdlog")
    add_includedirs("include", {public = true})

-- ============ 共享库目标（供外部使用） ============

target("kopidb_shared")
    set_kind("shared")
    -- 构建目标区分静态模块与共享库；对外库文件统一使用 kopidb 名称。
    set_basename("kopidb")
    add_files("src/logger/*.cpp", "src/config/*.cpp", "src/utils/*.cpp",
              "src/vlog/*.cpp",
              "src/iterator/*.cpp", "src/skiplist/*.cpp", "src/memtable/*.cpp",
              "src/block/*.cpp", "src/sst/*.cpp", "src/wal/*.cpp", "src/lsm/*.cpp")
    add_packages("toml11", "spdlog")
    add_includedirs("include", {public = true})  -- 确保包含路径正确
    set_targetdir("$(buildir)/lib")

    if is_plat("windows") then
        set_extension(".dll")
        add_defines("KOPIDB_EXPORTS")
        add_cxxflags("/LD")
    else
        set_extension(".so")
    end

    on_install(function (target)
        -- installdir() 查询安装根目录，不会创建目录。
        -- 本例得到：/tmp/kopidb-sdk
        local install_dir = target:installdir()
        -- 本例得到：/tmp/kopidb-sdk/include/kopidb
        local include_dir = path.join(install_dir, "include", "kopidb")
        -- 本例得到：/tmp/kopidb-sdk/lib
        local lib_dir = path.join(install_dir, "lib")

        -- 先创建目录，再复制 include 下的内容
        -- 重复安装时仍保持 kopidb/lsm/...，不额外嵌套 include
        os.mkdir(include_dir)
        os.cp("include/*", include_dir)

        -- 首次安装时 lib 目录可能不存在，必须先创建
        -- 复制时指定完整文件名，避免把 lib 当作库文件名
        os.mkdir(lib_dir)

        -- 查询当前目标编译出来的文件在哪里
        -- 我们项目在 macOS 下，例如：
        -- build/lib/libkopidb.so
        --
        -- 这行只是取得路径，不会重新编译库
        local libfile = target:targetfile()

        -- 判断当前构建平台是否为 Windows
        -- 是 Windows，执行 then 到 else 之间的代码
        if is_plat("windows") then
            local bin_dir = path.join(install_dir, "bin")
            os.mkdir(bin_dir)
            os.cp(libfile, path.join(bin_dir, path.filename(libfile)))

            -- 从构建产物取得导入库路径，避免与目标名或库文件名改动脱节。
            local implib = target:artifactfile("implib")
            if implib and os.isfile(implib) then
                os.cp(implib, path.join(lib_dir, path.filename(implib)))
            end
        else
            -- 非 Windows 平台执行这里，我们当前的 macOS 就走这一支
            --
            -- libfile：
            --   build/lib/libkopidb.so
            --
            -- path.filename(libfile)：
            --   libkopidb.so
            --
            -- 拼出的完整目标路径：
            --   /tmp/kopidb-sdk/lib/libkopidb.so
            --
            -- 将编译好的库复制到这个位置
            os.cp(libfile, path.join(lib_dir, path.filename(libfile)))
        end

        -- 安装供用户项目读取的接入配置；它们只引用已经编译好的库。
        -- 这里复制的是 packaging/cpp 下的文件，不是编译引擎的根 xmake.lua。
        -- 两份配置放在 SDK 根目录，用户解压后无需修改里面的路径。
        os.cp("packaging/cpp/xmake.lua", path.join(install_dir, "xmake.lua"))
        os.cp("packaging/cpp/KopiDBConfig.cmake",
              path.join(install_dir, "KopiDBConfig.cmake"))
    end)

-- ============ 测试目标 ============

target("test_config")
    set_kind("binary")
    set_group("tests")
    add_files("test/test_config.cpp")
    add_deps("logger", "config")
    add_packages("gtest", "toml11", "spdlog")
    add_includedirs("include", {public = true})

target("test_skiplist")
    set_kind("binary")
    set_group("tests")
    add_files("test/test_skiplist.cpp")
    add_deps("logger", "skiplist")
    add_packages("gtest", "toml11", "spdlog")
    add_includedirs("include", {public = true})

target("test_memtable")
    set_kind("binary")
    set_group("tests")
    add_files("test/test_memtable.cpp")
    add_deps("logger", "memtable")
    add_packages("gtest", "toml11", "spdlog")
    add_includedirs("include", {public = true})

target("test_block")
    set_kind("binary")
    set_group("tests")
    add_files("test/test_block.cpp")
    add_deps("logger", "block")
    add_packages("gtest", "toml11", "spdlog")
    add_includedirs("include", {public = true})

target("test_blockmeta")
    set_kind("binary")
    set_group("tests")
    add_files("test/test_blockmeta.cpp")
    add_deps("logger", "block")
    add_packages("gtest", "toml11", "spdlog")
    add_includedirs("include", {public = true})

target("test_utils")
    set_kind("binary")
    set_group("tests")
    add_files("test/test_utils.cpp")
    add_deps("logger", "utils")
    add_packages("gtest", "toml11", "spdlog")
    add_includedirs("include", {public = true})

target("test_sst")
    set_kind("binary")
    set_group("tests")
    add_files("test/test_sst.cpp")
    add_deps("logger", "sst")
    add_packages("gtest", "toml11", "spdlog")
    add_includedirs("include", {public = true})

target("test_lsm")
    set_kind("binary")
    set_group("tests")
    add_files("test/test_lsm.cpp")
    add_deps("logger", "kopidb", "memtable", "iterator")
    add_packages("gtest", "toml11", "spdlog")
    add_includedirs("include", {public = true})

target("test_block_cache")
    set_kind("binary")
    set_group("tests")
    add_files("test/test_block_cache.cpp")
    add_deps("logger", "block")
    add_includedirs("include", {public = true})
    add_packages("gtest", "toml11", "spdlog")

target("test_compact")
    set_kind("binary")
    set_group("tests")
    add_files("test/test_compact.cpp")
    add_deps("logger", "kopidb", "memtable", "iterator")
    add_packages("gtest", "toml11", "spdlog")
    add_includedirs("include", {public = true})

target("test_wal")
    set_kind("binary")
    set_group("tests")
    add_files("test/test_wal.cpp")
    add_deps("logger", "wal", "kopidb")
    add_includedirs("include", {public = true})
    add_packages("gtest", "toml11", "spdlog")

target("test_wisckey")
    set_kind("binary")
    set_group("tests")
    add_files("test/test_wisckey.cpp")
    add_deps("logger", "kopidb", "memtable", "iterator")
    add_packages("gtest", "toml11", "spdlog")
    add_includedirs("include", {public = true})

-- ============ 可执行目标 ============

target("example")
    set_kind("binary")
    add_files("example/main.cpp")
    add_deps("logger", "config", "utils", "iterator", "skiplist", 
             "memtable", "block", "sst", "wal", "kopidb")
    add_includedirs("include")  -- 显式添加包含路径
    set_targetdir("$(buildir)/bin")

target("debug")
    set_kind("binary")
    add_files("example/debug.cpp")
    add_deps("logger", "config", "utils", "iterator", "skiplist", 
             "memtable", "block", "sst", "wal", "kopidb")
    add_includedirs("include")  -- 显式添加包含路径
    set_targetdir("$(buildir)/bin")

-- ============ Python 绑定 ============

-- 内部扩展的构建目标、输出文件与 PYBIND11_MODULE 名称保持一致。
if is_plat("windows") then
    target("kopidb_pybind")
        set_kind("shared")
        add_files("sdk/kopidb_pybind.cpp")
        add_packages("pybind11")
        add_deps("kopidb")  -- Windows下使用原来的依赖
        add_includedirs("include", {public = true})
        set_targetdir("$(buildir)/lib")
        set_filename("kopidb_pybind.pyd")
        add_cxxflags("/LD")
else
    -- Unix/Linux/macOS平台
    target("kopidb_pybind")
        set_kind("shared")
        add_files("sdk/kopidb_pybind.cpp")
        add_packages("pybind11")
        add_deps("kopidb_shared")  -- Unix下使用共享库依赖
        add_includedirs("include", {public = true})
        set_targetdir("$(buildir)/lib")
        set_filename("kopidb_pybind.so")
        add_ldflags("-Wl,-rpath,$ORIGIN")
        add_defines("KOPIDB_EXPORT=__attribute__((visibility(\"default\")))")
        add_cxxflags("-fvisibility=hidden")
end

-- ============ 测试任务 ============

task("run-all-tests")
    set_category("plugin")
    set_menu {
        usage = "xmake run-all-tests",
        description = "Build and run all test binaries"
    }

    on_run(function ()
        import("core.project.project")

        local targets = project.targets()
        local test_targets = {}

        for name, _ in pairs(targets) do
            if name:startswith("test_") then
                table.insert(test_targets, name)
            end
        end

        table.sort(test_targets)

        if #test_targets == 0 then
            print("\27[33m[Warning] No test targets found.\27[0m")
            return
        end

        for _, name in ipairs(test_targets) do
            print("\27[32m>> Running\27[0m " .. name)
            os.execv("xmake", {"run", name})
            print("")
        end

        print("\27[32mAll tests finished.\27[0m")
    end)
