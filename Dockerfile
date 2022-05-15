# Build Stage
FROM --platform=linux/amd64 ubuntu:20.04 as builder

RUN apt-get update
RUN apt-get install -y clang gdb lldb cmake ninja-build

ADD . /btstack
WORKDIR /btstack/test/fuzz/build
RUN cmake ..
RUN make

FROM --platform=linux/amd64 ubuntu:20.04 

COPY --from=builder /btstack/test/fuzz/build/fuzz_ad_parser /
