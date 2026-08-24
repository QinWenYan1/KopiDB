import os
import platform
from setuptools import setup, find_packages
from setuptools.command.build_py import build_py

# 版本和基本描述
VERSION = "0.1.0"
DESCRIPTION = "A Python binding for Tiny LSM Tree Storage Engine"

class CustomBuild(build_py):
    def run(self):
        # 使用绝对路径确保准确性
        build_lib_dir = os.path.abspath(os.path.join(os.path.dirname(__file__), "../../build/lib"))
        package_lib_dir = os.path.join(self.build_lib, "tinylsm/core/lib")
        
        # 创建目标目录
        self.mkpath(package_lib_dir)
        
        # 动态库文件列表
        lib_files = [
            ("liblsm_shared.so", "liblsm_shared.so"),
            ("lsm_pybind.so", "lsm_pybind.so")
        ]
        
        # 复制文件
        for src_name, dst_name in lib_files:
            src = os.path.join(build_lib_dir, src_name)
            dst = os.path.join(package_lib_dir, dst_name)
            if os.path.exists(src):
                self.copy_file(src, dst)
            else:
                raise FileNotFoundError(f"Missing library file: {src}")
        
        super().run()

setup(
    name="tinylsm",
    version=VERSION,
    description=DESCRIPTION,
    packages=find_packages(),
package_data={
    "tinylsm.core": [
        "lib/*.so",
        "lib/*.dylib",
        "lib/*.dll",
        "lib/*.pyd"
    ]
},
    cmdclass={"build_py": CustomBuild},
    install_requires=["pybind11>=2.10"],  # 需声明绑定依赖
    python_requires=">=3.8",
    author="Your Name",
    classifiers=[
        "Development Status :: 3 - Alpha",
        "Programming Language :: Python :: 3",
    ]
)