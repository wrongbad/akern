#!/bin/sh
here=$(cd "$(dirname "$0")" && pwd)
export AKERN_HOME="$here/build" PATH="$here/build/bin:$PATH"
exec akern "$@"
