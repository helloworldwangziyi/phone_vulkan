#!/bin/sh
set -eu

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$repo_root"

sum_matches() {
    pattern=$1
    shift
    rg -U -P --count-matches "$pattern" "$@" 2>/dev/null |
        awk -F: '{ total += $NF } END { print total + 0 }'
}

cpp_all=$(sum_matches \
    'EVK_LOG[VDIWE]\s*\(' \
    --glob '!core/include/evk/log.h' core platform samples)
cpp_valid=$(sum_matches \
    'EVK_LOG[VDIWE]\s*\(\s*"[a-z][a-z0-9_]*"\s*,\s*"[a-z][a-z0-9_]*(?:[ "]|$)' \
    --glob '!core/include/evk/log.h' core platform samples)

if [ "$cpp_all" -ne "$cpp_valid" ]; then
    echo "invalid EVK_LOG call: expected feature and snake_case event" >&2
    rg -n --glob '!core/include/evk/log.h' \
        'EVK_LOG[VDIWE]\s*\(' core platform samples >&2
    exit 1
fi

direct_all=$(sum_matches \
    '(?:NSLog|hilog\.(?:debug|info|warn|error|fatal))\s*\(' \
    core/src platform samples)
direct_valid=$(sum_matches \
    '(?:NSLog\s*\(\s*@"|hilog\.(?:debug|info|warn|error|fatal)\s*\(\s*DOMAIN\s*,\s*TAG\s*,\s*'"'"')\[evk\]\[[a-z][a-z0-9_]*\]\[[a-z][a-z0-9_]*' \
    core/src platform samples)

if [ "$direct_all" -ne "$direct_valid" ]; then
    echo "invalid platform log: expected [evk][feature][event ...]" >&2
    rg -n '\b(?:NSLog|hilog\.(?:debug|info|warn|error|fatal))\s*\(' \
        core/src platform samples >&2
    exit 1
fi

# 业务代码只能走上述两类入口。HarmonyOS 的 OH_LOG_Print 是 spdlog sink，
# 接收的 text 已由 EVK_LOG 封装生成，因此不属于旁路业务日志。
forbidden_pattern='(?:\b(?:printf|fprintf|puts|fputs)\s*\(|\bstd::(?:cout|cerr|clog)\b|\bSPDLOG_(?:TRACE|DEBUG|INFO|WARN|ERROR|CRITICAL)\s*\(|\bspdlog::(?:trace|debug|info|warn|error|critical|log)\s*\(|\bLog\.(?:v|d|i|w|e)\s*\(|\bconsole\.(?:log|debug|info|warn|error)\s*\(|\b__android_log_(?:print|write)\s*\(|\bos_log(?:_with_type)?\s*\()'

if rg -n -P \
    --glob '*.{c,cc,cpp,cxx,h,hpp,m,mm,java,kt,ets,ts}' \
    "$forbidden_pattern" core platform samples; then
    echo "direct logging API found: use EVK_LOG or a validated platform log" >&2
    exit 1
fi

echo "log format check passed: $cpp_all C++ logs, $direct_all platform logs"
