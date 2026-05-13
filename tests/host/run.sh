#!/usr/bin/env bash
set -euo pipefail

mode="${1:-all}"
repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
build_dir="${repo_root}/.cache/host-build"

if [[ -x /opt/homebrew/opt/llvm/bin/clang && -x /opt/homebrew/opt/llvm/bin/clang++ ]]; then
  export CC="${CC:-/opt/homebrew/opt/llvm/bin/clang}"
  export CXX="${CXX:-/opt/homebrew/opt/llvm/bin/clang++}"
elif command -v clang >/dev/null 2>&1 && command -v clang++ >/dev/null 2>&1; then
  export CC="${CC:-$(command -v clang)}"
  export CXX="${CXX:-$(command -v clang++)}"
fi

configure() {
  local generator="Unix Makefiles"
  local generator_args=(-G "${generator}")
  if command -v ninja >/dev/null 2>&1; then
    generator="Ninja"
    generator_args=(-G "${generator}")
  fi

  if [[ -f "${build_dir}/CMakeCache.txt" ]] &&
     ! grep -q "^CMAKE_GENERATOR:INTERNAL=${generator}$" "${build_dir}/CMakeCache.txt"; then
    rm -rf "${build_dir}"
  fi

  cmake -S "${repo_root}/tests/host" -B "${build_dir}" "${generator_args[@]}" \
    -DHOST_SANITIZERS=ON \
    -DHOST_FUZZING=ON \
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
}

build() {
  cmake --build "${build_dir}"
}

test_units() {
  ctest --test-dir "${build_dir}" --output-on-failure
}

fuzz_smoke() {
  "${build_dir}/host_secplus_fuzz" -runs=100000 -max_len=64
  "${build_dir}/host_hap_tlv_fuzz" -runs=100000 -max_len=768
  "${build_dir}/host_app_events_fuzz" -runs=50000 -max_len=512
}

analyze() {
  mkdir -p "${repo_root}/.cache/host-analysis"
  cppcheck --project="${build_dir}/compile_commands.json" \
    --enable=warning,style,performance,portability \
    --inline-suppr \
    --suppress=missingIncludeSystem \
    --template=gcc \
    2> "${repo_root}/.cache/host-analysis/cppcheck.txt"
}

case "${mode}" in
  configure)
    configure
    ;;
  build)
    configure
    build
    ;;
  test)
    configure
    build
    test_units
    ;;
  fuzz)
    configure
    build
    fuzz_smoke
    ;;
  analyze)
    configure
    build
    analyze
    ;;
  all)
    configure
    build
    test_units
    fuzz_smoke
    analyze
    ;;
  *)
    echo "usage: $0 [configure|build|test|fuzz|analyze|all]" >&2
    exit 2
    ;;
esac
