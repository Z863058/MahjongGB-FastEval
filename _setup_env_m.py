"""Build script for the C++ accelerated mahjong environment.

Usage (on the Docker container / Linux):
    pip install pybind11
    python _setup_env_m.py build_ext --inplace

This produces env_m_fast.cpython-3XX-*.so (Linux) or env_m_fast.pyd (Windows)
in the project root.
"""

import sys
from setuptools import setup, Extension
import pybind11

if sys.platform == "win32":
    extra_compile_args = ["/O2", "/std:c++17"]
    extra_link_args = []
else:
    extra_compile_args = ["-O3", "-std=c++17", "-fvisibility=hidden", "-flto"]
    extra_link_args = ["-flto"]

ext_env = Extension(
    "env_m_fast",
    sources=["env_m_cpp/env_m_fast.cpp"],
    include_dirs=[pybind11.get_include()],
    language="c++",
    extra_compile_args=extra_compile_args,
    extra_link_args=extra_link_args,
)

ext_shanten = Extension(
    "shanten_fast",
    sources=["env_m_cpp/shanten_fast.cpp"],
    include_dirs=[pybind11.get_include()],
    language="c++",
    extra_compile_args=extra_compile_args,
    extra_link_args=extra_link_args,
)

setup(
    name="mahjong_cpp_extensions",
    version="1.1",
    description="C++ accelerated Chinese Standard Mahjong environment & shanten calculator",
    ext_modules=[ext_env, ext_shanten],
)
