#!/bin/bash

DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"

cd ${DIR}

if [ ! -d "testsuite" ]; then 
  git clone https://github.com/softdevteam/LuaJIT-test-cleanup testsuite
fi
