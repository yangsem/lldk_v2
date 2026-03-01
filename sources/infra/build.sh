#!/bin/bash

set -e

rm -rf build
make clean
bear -- make -j8
