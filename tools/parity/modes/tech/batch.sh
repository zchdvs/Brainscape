#!/bin/bash
# T: this directory (the probes, mounted as /w); B: the repository (mounted read-only as /b).
# On Windows (Git Bash) docker needs native paths: pwd -W.
T=$(cd "$(dirname "$0")" && (pwd -W 2>/dev/null || pwd))
B=$(cd "$(dirname "$0")/../../../.." && (pwd -W 2>/dev/null || pwd))
export MSYS_NO_PATHCONV=1
docker run --rm -v "$T:/w" -v "$B:/b:ro" --entrypoint bash infinitime/infinitime-build:latest /w/linux_run.sh gcc11 g++-11 full
docker run --rm -v "$T:/w" -v "$B:/b:ro" --entrypoint bash python:3.11 /w/linux_run.sh gcc14 g++-14 full
docker run --rm -v "$T:/w" -v "$B:/b:ro" --entrypoint bash infinitime/infinitime-build:latest /w/linux_run.sh clang14 clang++-14 full -DNO_STD_CHARCONV
echo batch complete
