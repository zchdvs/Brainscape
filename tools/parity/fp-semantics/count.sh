#!/bin/sh
# usage: count.sh <asm file> <regex of fused mnemonics>
awk -v re="$2" '
  /^(_)?k_[a-z_]+:/ { fn=$1; sub(":","",fn); sub("^_","",fn); next }
  fn != "" && $0 ~ re { c[fn]++ }
  END { n=split("k_one_expr k_two_stmt k_smoother k_stdfma k_reduce k_scale",F," "); s="";
        for(i=1;i<=n;i++){ s=s sprintf("%s=%d ", F[i], c[F[i]]+0) } print s }' "$1"
