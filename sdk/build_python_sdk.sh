#!/bin/bash
cd .. && xmake f -m release && xmake build lsm_pybind
cd -
cp -r ../build/lib ./tinylsm/tinylsm/core

cd tinylsm
rm -rf build dist tinylsm.egg-info

python3 setup.py sdist bdist_wheel

pip install -e .