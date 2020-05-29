#! /bin/bash

cmake \
    -G "Unix Makefiles" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_CXX_FLAGS=-m32 \
    -DCMAKE_SHARED_LINKER_FLAGS=-m32 \
    -DBUILD_TESTING=FALSE \
    -DEMBREE_ISPC_SUPPORT=TRUE \
    -DEMBREE_STATIC_LIB=TRUE \
    -DEMBREE_TASKING_SYSTEM=INTERNAL \
    -DEMBREE_TUTORIALS=FALSE \
    -DEMBREE_IGNORE_CMAKE_CXX_FLAGS=FALSE \
    ../..

cmake --build .
