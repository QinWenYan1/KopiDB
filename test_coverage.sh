#! /usr/bin/bash

rm -rf build/*
rm -rf coverage-report

xmake f -m coverage
xmake
xmake run-all-tests

lcov --capture --directory . --output-file coverage.info
lcov --remove coverage.info '/usr/*' '*/.xmake/*' '*/test/*' -o coverage.cleaned.info
genhtml -o coverage-report coverage.cleaned.info
