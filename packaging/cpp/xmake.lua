-- 此文件由安装流程复制到 SDK 根目录，供用户的 includes("SDK路径") 读取。
-- os.scriptdir() 指向本文件所在目录，不依赖用户从哪个工作目录运行 xmake。
local sdk_dir = os.scriptdir()
local lib_dir = path.join(sdk_dir, "lib")

target("kopidb")
    -- phony 只提供依赖配置，不编译数据库源码，也不生成另一份库。
    set_kind("phony")
    set_default(false)

    -- public 将这些要求传递给 add_deps("kopidb") 的程序目标。
    -- 只影响使用 KopiDB 的目标，不修改用户项目的全局设置。
    set_languages("c++20", {public = true})
    add_includedirs(path.join(sdk_dir, "include", "kopidb"), {public = true})
    -- 目录与库名分开传递，避免 Xmake 3.1.0 把带空格的完整库路径拆开。
    add_linkdirs(lib_dir, {public = true})
    add_links("kopidb", {public = true})
    add_syslinks("pthread", {public = true})

    -- 链接时找到库还不够；运行程序时也需要通过这个目录找到共享库。
    if lib_dir:find("%s") then
        -- Xmake 3.1.0 的 rpath 自动探测会拆开带空格的路径，导致漏掉运行路径。
        -- 这里把路径作为独立参数传给链接器，保留其中的空格。
        -- 额外的空字符串会被忽略，但可避免 3.1.0 对单个参数组的解包问题：
        -- 没有它时，第一个用户目标能继承这些参数，后续目标可能丢失它们。
        add_ldflags({"-Xlinker", "-rpath", "-Xlinker", lib_dir}, "",
                    {public = true, force = true, expand = false})
        add_shflags({"-Xlinker", "-rpath", "-Xlinker", lib_dir}, "",
                    {public = true, force = true, expand = false})
    else
        add_rpathdirs(lib_dir, {public = true})
    end
target_end()
