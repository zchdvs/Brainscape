// A kernel every contracting compiler fuses: the audit_fused.py self-test compiles it
// with contraction on and requires the pattern to find a fused instruction in it.
float FusedSelfTestF(float a, float b, float c) { return a * b + c; }
double FusedSelfTestD(double a, double b, double c) { return a * b - c; }
void FusedSelfTestLoop(float* y, const float* x, float k, int n) {
  for (int i = 0; i < n; ++i) y[i] = y[i] * k + x[i];
}
