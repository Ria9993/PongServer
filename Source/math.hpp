#pragma once

#include <cmath>
#include <stdint.h>
#include <cfloat>

struct vec2
{
    float x;
    float y;

    inline vec2 operator+(const vec2& rhs) const {
        vec2 ret;
        ret.x = x + rhs.x;
        ret.y = y + rhs.y;
        return ret;
    }

    inline vec2 operator-() const {
        vec2 ret;
        ret.x = -x;
        ret.y = -y;
        return ret;
    }

    inline vec2 operator-(const vec2& rhs) const {
        vec2 ret;
        ret.x = x - rhs.x;
        ret.y = y - rhs.y;
        return ret;
    } 

    inline vec2 operator*(const float rhs) const {
        vec2 ret;
        ret.x = x * rhs;
        ret.y = y * rhs;
        return ret;
    }

    inline float length() const {
        return sqrtf(x * x +y * y);
    }

    inline float squared_length() const {
        return x * x + y * y;
    }

    static inline float dot(const vec2& a, const vec2& b) {
        return a.x * b.x + a.y * b.y;
    }
    
    static inline vec2 normalize(const vec2& a) {
        const float length = sqrtf(a.x * a.x + a.y * a.y);
        vec2 ret;
        ret.x = a.x / length;
        ret.y = a.y / length;
        return ret;
    }

    static inline float cross(const vec2& a, const vec2& b) {
        return a.x * b.y - a.y * b.x;
    }

    friend vec2 operator*(const float a, const vec2 b);
};

inline vec2 operator*(const float a, const vec2 b) {
    vec2 ret;
    ret.x = b.x * a;
    ret.y = b.y * a;
    return ret;
}


// Math Operation Helper
inline uint32_t saturate_u32add (uint32_t a, uint32_t b, uint32_t max = UINT32_MAX)
{
    uint32_t sum = a + b;
    if (sum < a || sum < b) {
        return max;
    }
    return (sum > max) ? max : sum;
}

inline uint32_t saturate_u32sub (uint32_t a, uint32_t b, uint32_t min = 0)
{
    return (a < b) ? min : a - b;
}

inline vec2 LineNormalVector(const vec2 p1, const vec2 p2, bool bClockwise = false)
{
    vec2 normal;
    normal.x = p2.y - p1.y;
    normal.y = p1.x - p2.x;
    if (bClockwise) {
        normal.x = -normal.x;
        normal.y = -normal.y;
    }
    return vec2::normalize(normal);
}
