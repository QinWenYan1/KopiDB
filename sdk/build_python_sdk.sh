#!/bin/bash
cd .. && xmake f -m release && xmake build kopidb_pybind
cd -
cp -r ../build/lib ./kopidb/kopidb/core

cd kopidb
rm -rf build dist kopidb.egg-info

python3 setup.py sdist bdist_wheel

pip install -e .