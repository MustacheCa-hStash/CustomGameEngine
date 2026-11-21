// Shared by the camera sky, probe tracer and CPU sky projection.
#ifdef __cplusplus
#include <cmath>
#define SKY_EXP std::exp
#define SKY_INLINE inline
#else
#define SKY_EXP exp
#define SKY_INLINE
#endif
static const float meadowSunStrength = 1.65f;
static const float hazeRed = 1.0f, hazeGreen = 0.95f, hazeBlue = 0.86f;
SKY_INLINE float circumsolarRadiance(float cosine)
{
    return 1.10f * SKY_EXP(5.8f * (cosine - 1.0f)) + 3.2f * SKY_EXP(32.0f * (cosine - 1.0f));
}
#undef SKY_INLINE
#undef SKY_EXP
