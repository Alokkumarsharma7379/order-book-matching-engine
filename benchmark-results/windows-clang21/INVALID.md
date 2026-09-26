This initial development run is invalid for reporting: mean latency was formatted
as zero when streaming a long double with this Windows toolchain. The benchmark
now accumulates/formats a double, preserves more duration precision, and its runner
rejects nonpositive mean latency. Use the verified rerun in `../windows-clang21-verified/`.
