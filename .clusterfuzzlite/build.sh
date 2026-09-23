#!/bin/bash -eu
# Builds the fuzz/ targets with ClusterFuzzLite's compiler flags and engine.
#
# The runner image has none of the build image's libraries, so everything is
# linked statically: SQLite from its amalgamation, which the SQLite module's
# CMake already fetches when the system has no headers, and ICU from source
# against libc++. ICU is built without sanitizer flags; it is a dependency,
# not code under test. Its archives are repeated at the end of the link line,
# after the ones CMake orders, so that i18n's references into uc resolve.
# HTTPS is off, as the fuzz targets never need it, and GGML_NATIVE is off
# because the fuzzers run on another machine.
icu="$WORK/icu"
if [ ! -f "$icu/lib/libicuuc.a" ]; then
  (cd "$SRC/icu/source" &&
    CFLAGS="-O1" CXXFLAGS="-O1 -stdlib=libc++" LDFLAGS="-stdlib=libc++" \
      ./configure --prefix="$icu" --enable-static --disable-shared --disable-tests \
        --disable-samples --disable-extras --disable-icuio --disable-layoutex \
        --with-data-packaging=static &&
    make -j"$(nproc)" install)
fi

cmake -S "$SRC/decision-query" -B "$WORK/build" -G Ninja \
  -DDQ_POSTGRES=OFF -DDQ_CUDA=OFF -DGGML_NATIVE=OFF -DGGML_OPENMP=OFF \
  -DDQ_BUILD_TESTS=OFF -DDQ_BUILD_FUZZ_TESTS=ON \
  -DDQ_LIB_FUZZING_ENGINE="$LIB_FUZZING_ENGINE" \
  -DICU_ROOT="$icu" \
  -DCMAKE_DISABLE_FIND_PACKAGE_OpenSSL=ON \
  -DCMAKE_CXX_STANDARD_LIBRARIES="$icu/lib/libicui18n.a $icu/lib/libicuuc.a $icu/lib/libicudata.a -ldl -lpthread -lm"
cmake --build "$WORK/build" --target fuzz_options fuzz_endpoint fuzz_sql

for target in fuzz_options fuzz_endpoint fuzz_sql; do
  cp "$WORK/build/fuzz/$target" "$OUT/"
  (cd "$SRC/decision-query/fuzz/corpus/$target" && zip -q "$OUT/${target}_seed_corpus.zip" ./*)
done
