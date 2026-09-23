# ClusterFuzzLite build image; see https://google.github.io/clusterfuzzlite/build-integration/
FROM gcr.io/oss-fuzz-base/base-builder:ubuntu-24-04
RUN apt-get update && apt-get install -y ninja-build nlohmann-json3-dev zip
# The fuzzers link libc++, so ICU is built from source in build.sh rather than
# taken from Ubuntu's libstdc++ packages. 74.2 matches ubuntu-24.04's libicu-dev.
# The checksum is the one published in the release's SHASUM512.txt.
RUN curl -sSLo /tmp/icu.tgz https://github.com/unicode-org/icu/releases/download/release-74-2/icu4c-74_2-src.tgz \
    && echo "e6c7876c0f3d756f3a6969cad9a8909e535eeaac352f3a721338b9cbd56864bf7414469d29ec843462997815d2ca9d0dab06d38c37cdd4d8feb28ad04d8781b0  /tmp/icu.tgz" | sha512sum -c - \
    && tar -xzf /tmp/icu.tgz -C $SRC && rm /tmp/icu.tgz
COPY . $SRC/decision-query
WORKDIR $SRC/decision-query
COPY .clusterfuzzlite/build.sh $SRC/
