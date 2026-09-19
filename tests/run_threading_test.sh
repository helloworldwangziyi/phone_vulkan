#!/bin/sh
set -eu
EVK_TEST_NAME=threading_test
export EVK_TEST_NAME
exec sh "$(dirname -- "$0")/run_ui_runtime_test.sh"
