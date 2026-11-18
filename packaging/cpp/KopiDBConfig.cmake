# 此文件由安装流程复制到 SDK 根目录，供用户的 find_package(KopiDB) 读取。
# 用户可以将整个 SDK 解压到其他目录；路径始终以本配置文件的位置为准。
if(TARGET KopiDB::KopiDB)
    return()
endif()

# 线程依赖也随库传递给使用方，不要求用户再手写线程链接参数。
include(CMakeFindDependencyMacro)
find_dependency(Threads)

# IMPORTED 表示引用已经编译好的共享库，不会重新构建数据库源码。
add_library(KopiDB::KopiDB SHARED IMPORTED)
set_target_properties(KopiDB::KopiDB PROPERTIES
    IMPORTED_LOCATION "${CMAKE_CURRENT_LIST_DIR}/lib/libkopidb.so"
    INTERFACE_INCLUDE_DIRECTORIES "${CMAKE_CURRENT_LIST_DIR}/include/kopidb"
    INTERFACE_COMPILE_FEATURES cxx_std_20
    INTERFACE_LINK_LIBRARIES Threads::Threads
)

# 当前 macOS 共享库使用这个 install name。CMake 根据库的位置生成
# 用户程序的构建运行路径，使其直接运行时也能找到 SDK 中的共享库。
if(APPLE)
    set_target_properties(KopiDB::KopiDB PROPERTIES
        IMPORTED_SONAME "@rpath/libkopidb.so"
    )
endif()
