#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
#
# Checks that an arm-zephyr-eabi toolchain has libstdc++ with posix gthreads.
# Usage: check_cpp_gthreads.sh <toolchain dir, e.g. gnu/arm-zephyr-eabi>

TARGET=arm-zephyr-eabi
BIN="$1/bin/${TARGET}"
FAILED=0

fail() {
  echo "FAIL: $*"
  FAILED=1
}

${BIN}-gcc -v 2>&1 | grep -q '^Thread model: posix$' || fail "thread model is not posix"

TEST_CC=$(mktemp --suffix=.cc)
trap 'rm -f "${TEST_CC}"' EXIT
cat > "${TEST_CC}" <<'EOF'
#include <mutex>
#include <type_traits>
#ifndef _GLIBCXX_HAS_GTHREADS
#error "_GLIBCXX_HAS_GTHREADS is not defined"
#endif
#ifndef _GLIBCXX_HAVE_TLS
#error "_GLIBCXX_HAVE_TLS is not defined"
#endif
#ifndef _GLIBCXX_USE_CLOCK_MONOTONIC
#error "_GLIBCXX_USE_CLOCK_MONOTONIC is not defined"
#endif
#ifndef _GLIBCXX_USE_NANOSLEEP
#error "_GLIBCXX_USE_NANOSLEEP is not defined"
#endif
#if !_GLIBCXX_USE_CXX11_ABI
#error "_GLIBCXX_USE_CXX11_ABI is not 1"
#endif
#if _GLIBCXX_GTHREAD_USE_WEAK
#error "_GLIBCXX_GTHREAD_USE_WEAK is not 0"
#endif
#ifdef __GTHREAD_MUTEX_INIT
#error "__GTHREAD_MUTEX_INIT is defined"
#endif
#ifdef _GLIBCXX_USE_PTHREAD_RWLOCK_T
#error "_GLIBCXX_USE_PTHREAD_RWLOCK_T is defined"
#endif
static_assert(!std::is_trivially_destructible_v<std::mutex>, "std::mutex has no destructor");
EOF

for CPU in "-mcpu=cortex-m4 -mfloat-abi=hard -mfpu=fpv4-sp-d16;thumb/v7e-m+fp/hard" \
           "-mcpu=cortex-m33 -mfloat-abi=hard -mfpu=fpv5-sp-d16;thumb/v8-m.main+fp/hard"; do
  for OPT in "-O2;" "-Os;/space"; do
    FLAGS="${CPU%;*} ${OPT%;*}"
    EXPECTED="${CPU#*;}${OPT#*;}"
    DIR=$(${BIN}-g++ ${FLAGS} -print-multi-directory)
    echo "Checking ${DIR}"
    [ "${DIR}" == "${EXPECTED}" ] || fail "${FLAGS}: multilib ${DIR}, expected ${EXPECTED}"

    ${BIN}-g++ ${FLAGS} -std=c++17 -fsyntax-only "${TEST_CC}" 2>&1 | grep 'error:' | sed "s|^|FAIL: ${DIR}: |"
    [ "${PIPESTATUS[0]}" == "0" ] || FAILED=1

    LIB=$(${BIN}-g++ ${FLAGS} -print-file-name=libstdc++.a)
    ${BIN}-nm -A "${LIB}" 2>/dev/null | grep -q '^[^:]*:eh_alloc\.o:.*emergency_pool' && fail "${DIR}: emergency_pool in eh_alloc.o"
    ${BIN}-nm -A "${LIB}" 2>/dev/null | grep -q '^[^:]*:functexcept\.o: *U __cxa_throw$' || fail "${DIR}: functexcept.o does not call __cxa_throw"
    ${BIN}-nm -A "${LIB}" 2>/dev/null | grep -q '^[^:]*:guard\.o: *U pthread_once$' || fail "${DIR}: guard.o does not call pthread_once"
    ${BIN}-readelf -sW "${LIB}" 2>/dev/null | awk '/^File:/ { f = ($2 ~ /\(eh_globals\.o\)$/) } f && $4 == "TLS"' | grep -q . \
      || fail "${DIR}: eh_globals.o has no TLS symbols"
  done
done

[ "${FAILED}" == "0" ] && echo "All checks passed"
exit ${FAILED}
