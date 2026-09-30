# libFuzzer harnesses for the parsers that consume untrusted network input.
#
# Off by default: libFuzzer is a Clang feature, and enabling it changes the flags
# of the executable under test, so the normal build must be unaffected. Enable
# explicitly, e.g.:
#
#   cmake -S . -B build-fuzz -G Ninja \
#         -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_BUILD_TYPE=RelWithDebInfo \
#         -DCASTMIRROR_BUILD_FUZZERS=ON
#
# For useful sanitizer coverage, configure the whole project with
#   -DCMAKE_CXX_FLAGS="-fsanitize=fuzzer-no-link,address"
# so the statically linked castcore is instrumented too; by default only these
# harness translation units carry the sanitizer.
#
# Each target's entry point is the standard `LLVMFuzzerTestOneInput`.

if(NOT CASTMIRROR_BUILD_FUZZERS)
  return()
endif()

if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  message(WARNING
      "CASTMIRROR_BUILD_FUZZERS=ON but the compiler is "
      "${CMAKE_CXX_COMPILER_ID}; libFuzzer requires Clang. Skipping fuzz targets.")
  return()
endif()

set(FUZZ_FLAGS -fsanitize=fuzzer,address -fno-omit-frame-pointer -g)

foreach(harness rtcp_parser mdns_parser)
  add_executable(fuzz_${harness} fuzz_${harness}.cc)
  target_link_libraries(fuzz_${harness} PRIVATE castcore)
  target_compile_options(fuzz_${harness} PRIVATE ${FUZZ_FLAGS})
  target_link_options(fuzz_${harness} PRIVATE ${FUZZ_FLAGS})

  # A bounded smoke run so `ctest` catches a crash even without a corpus.
  add_test(NAME fuzz_${harness}_smoke
           COMMAND fuzz_${harness} -runs=200000 -max_total_time=60)
endforeach()
