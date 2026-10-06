// "plugin/" TU: JUCE code built with different FP flags that happens to call the same inline helper.
#include "mac.h"
float PluginSide(float a, float b, float c) { return Mac(a, b, c); }
