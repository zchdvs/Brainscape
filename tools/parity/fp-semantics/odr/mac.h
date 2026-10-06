#pragma once
// Stand-in for an engine header with inline math (GrainMath.h / Smoother.h / PostChain.h).
#if defined(_MSC_VER)
#define NOINL __declspec(noinline)
#else
#define NOINL __attribute__((noinline))
#endif
NOINL inline float Mac(float a, float b, float c) { return a * b + c; }
