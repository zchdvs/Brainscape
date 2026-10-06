#include <cfloat>
#if !defined(FLT_EVAL_METHOD) || FLT_EVAL_METHOD != 0
#error "FLT_EVAL_METHOD must be 0"
#endif
#if defined(_MSC_VER) && !defined(__clang__) && (defined(_M_FP_FAST) || defined(_M_FP_CONTRACT))
#error "fast/contract"
#endif
int main() { return 0; }
