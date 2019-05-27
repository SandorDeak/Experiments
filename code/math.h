#pragma once

#include "platform.h"
#include <math.h>

union v2
{
	struct
	{
		f32 x, y;
	};
	f32 e[2];
};
union v3
{
	struct
	{
		f32 x, y, z;
	};
	struct
	{
		v2 xy;
		f32 _ignored0;
	};
	struct
	{
		f32 _ignored1;
		v2 yz;
	};
	struct
	{
		float r, g, b;
	};
	float e[3];
};
union v4
{
	struct
	{
		float x, y, z, w;
	};
	struct
	{
		float r, g, b, a;
	};
	struct
	{
		v3 xyz;
		f32 _ignored0;
	};
	struct
	{
		v3 rgb;
		f32 _ignored1;
	};
	struct
	{
		v2 xy;
		v2 zw;
	};
	f32 e[4];
	v2 e2[2];
};


v3 V3(v2 xy, f32 z)
{
	return { xy.x, xy.y, z };
}

v3 V3(s32 x, s32 y, s32 z)
{
	return { (f32)x, (f32)y, (f32)z };
}

v3 V3(f32 c)
{
	return { c, c, c };
}

v2 V2(s32 x, s32 y)
{
	return { (f32)x, (f32)y };
}

v2 V2(u32 x, u32 y)
{
	return { (f32)x, (f32)y };
}

v4 V4(v3 xyz, f32 w)
{
	v4 result;
	result.xyz = xyz;
	result.w = w;
	return result;
}

//a+b
inline v2 operator+(v2 a, v2 b)
{
	return { a.x + b.x, a.y + b.y };
}
inline v3 operator+(v3 a, v3 b)
{
	return { a.x + b.x, a.y + b.y, a.z + b.z };
}
inline v4 operator+(v4 a, v4 b)
{
	return { a.x + b.x, a.y + b.y, a.z + b.z, a.w + b.w };
}

//a-b
inline v2 operator-(v2 a, v2 b)
{
	return { a.x - b.x, a.y - b.y };
}
inline v3 operator-(v3 a, v3 b)
{
	return { a.x - b.x, a.y - b.y, a.z - b.z };
}
inline v4 operator-(v4 a, v4 b)
{
	return { a.x - b.x, a.y - b.y, a.z - b.z, a.w - b.w };
}

//s*a
inline v2 operator*(float s, v2 a)
{
	return { s*a.x, s*a.y };
}
inline v3 operator*(float s, v3 a)
{
	return { s*a.x, s*a.y, s*a.z };
}
inline v4 operator*(float s, v4 a)
{
	return { s*a.x, s*a.y, s*a.z, s*a.w };
}

//a*s
inline v2 operator*(v2 a, float s)
{
	return { s*a.x, s*a.y };
}
inline v3 operator*(v3 a, float s)
{
	return { s*a.x, s*a.y, s*a.z };
}
inline v4 operator*(v4 a, float s)
{
	return { s*a.x, s*a.y, s*a.z, s*a.w };
}

//-a
inline v2 operator-(v2 a)
{
	return -1.f*a;
}
inline v3 operator-(v3 a)
{
	return -1.f*a;
}
inline v4 operator-(v4 a)
{
	return -1.f*a;
}

//a/s
inline v2 operator/(v2 a, float s)
{
	return { a.x / s, a.y / s };
}
inline v3 operator/(v3 a, float s)
{
	return { a.x / s, a.y / s, a.z / s };
}
inline v4 operator/(v4 a, float s)
{
	return { a.x / s, a.y / s, a.z / s, a.w / s };
}


//dot(a, b)
inline float dot(v2 a, v2 b)
{
	return a.x * b.x + a.y*b.y;
}
inline float dot(v3 a, v3 b)
{
	return a.x * b.x + a.y*b.y + a.z*b.z;
}
inline float dot(v4 a, v4 b)
{
	return a.x * b.x + a.y*b.y + a.z*b.z + a.w*b.w;
}

//cross(a,b)
inline float cross(v2 a, v2 b)
{
	return a.x * b.y - a.y*b.x;
}
inline v3 cross(v3 a, v3 b)
{

}

//a+=b
inline v2& operator+=(v2& a, v2 b)
{
	a = a + b;
	return a;
}
inline v3& operator+=(v3& a, v3 b)
{
	a = a + b;
	return a;
}inline v4& operator+=(v4& a, v4 b)
{
	a = a + b;
	return a;
}

//a-=b
inline v2& operator-=(v2& a, v2 b)
{
	a = a - b;
	return a;
}
inline v3& operator-=(v3& a, v3 b)
{
	a = a - b;
	return a;
}
inline v4& operator-=(v4& a, v4 b)
{
	a = a - b;
	return a;
}

//a*=s
inline v2& operator*=(v2& a, float s)
{
	a = a * s;
	return a;
}
inline v3& operator*=(v3& a, float s)
{
	a = a * s;
	return a;
}
inline v4& operator*=(v4& a, float s)
{
	a = a * s;
	return a;
}

//a/=s
inline v2& operator/=(v2& a, float s)
{
	a = a / s;
	return a;
}
inline v3& operator/=(v3& a, float s)
{
	a = a / s;
	return a;
}
inline v4& operator/=(v4& a, float s)
{
	a = a / s;
	return a;
}

//a*b
inline v2 operator*(v2 a, v2 b)
{
	return { a.x*b.x, a.y*b.y };
}
inline v3 operator*(v3 a, v3 b)
{
	return { a.x*b.x, a.y*b.y, a.z*b.z };
}
inline v4 operator*(v4 a, v4 b)
{
	return { a.x*b.x, a.y*b.y, a.z*b.z, a.w*b.w };
}

//a/b
inline v2 operator/(v2 a, v2 b)
{
	return { a.x / b.x, a.y / b.y };
}
inline v3 operator/(v3 a, v3 b)
{
	return { a.x / b.x, a.y / b.y, a.z / b.z };
}
inline v4 operator/(v4 a, v4 b)
{
	return { a.x / b.x, a.y / b.y, a.z / b.z, a.w / b.w };
}

//length(a)
inline float length(v2 a)
{
	return sqrtf(dot(a, a));
}
inline float length(v3 a)
{
	return sqrtf(dot(a, a));
}
inline float length(v4 a)
{
	return sqrtf(dot(a, a));
}

//normaliza(a)
inline v2 normalize(v2 a)
{
	return a / length(a);
}
inline v3 normalize(v3 a)
{
	return a / length(a);
}
inline v4 normalize(v4 a)
{
	return a / length(a);
}

//normalizeSafe(a)
inline v2 normalizeSafe(v2 a)
{
	v2 result = {};
	f32 l = length(a);
	if (l > 0.f)
	{
		result = a / l;
	}
	return result;
}
inline v3 normalizeSafe(v3 a)
{
	v3 result = {};
	f32 l = length(a);
	if (l > 0.f)
	{
		result = a / l;
	}
	return result;

}
inline v4 normalizeSafe(v4 a)
{
	v4 result = {};
	f32 l = length(a);
	if (l > 0.f)
	{
		result = a / l;
	}
	return result;

}


//lengthSq(a)
inline float lengthSq(v2 a)
{
	return dot(a, a);
}
inline float lengthSq(v3 a)
{
	return dot(a, a);
}
inline float lengthSq(v4 a)
{
	return dot(a, a);
}

//matix
union m4
{
	f32 e[4][4];
	v4 c[4];
	struct
	{
		v3 xAxis;
		f32 _ignored0;
		v3 yAxis;
		f32 _ignored1;
		v3 zAxis;
		f32 _ignored2;
		v3 translation;
		f32 _ignored3;
	};
};


inline m4 operator*(m4 a, m4 b)
{
	m4 r = {};
	for (s32 j = 0; j < 4; ++j)
	{
		for (s32 i = 0; i < 4; ++i)
		{
			for (s32 k = 0; k < 4; ++k)
			{
				r.e[j][i] += a.e[k][i] * b.e[j][k];
			}
		}
	}
	return r;
}
inline m4 rotationX(f32 angle)
{
	m4 r;
	f32 c = cosf(angle);
	f32 s = sinf(angle);
	r.c[0] = { 1.f, 0.f, 0.f, 0.f };
	r.c[1] = { 0.f, c, s, 0.f };
	r.c[2] = { 0.f, -s, c, 0.f };
	r.c[3] = { 0.f, 0.f, 0.f, 1.f };
	return r;
}
inline m4 rotationY(f32 angle)
{
	m4 r;
	f32 c = cosf(angle);
	f32 s = sinf(angle);
	r.c[0] = { c, 0.f, -s, 0.f };
	r.c[1] = { 0.f, 1.f, 0.f, 0.f };
	r.c[2] = { s, 0.f, c, 0.f };
	r.c[3] = { 0.f, 0.f, 0.f, 1.f };
	return r;
}
inline m4 rotationZ(f32 angle)
{
	m4 r;
	f32 c = cosf(angle);
	f32 s = sinf(angle);
	r.c[0] = { c, s, 0.f, 0.f };
	r.c[1] = { -s, c, 0.f, 0.f };
	r.c[2] = { 0.f, 0.f, 1.f, 0.f };
	r.c[3] = { 0.f, 0.f, 0.f, 1.f };
	return r;
}
inline m4 transpose(m4 m)
{
	m4 r;
	for (s32 j = 0; j < 4; ++j)
	{
		for (s32 i = 0; i < 4; ++i)
		{
			r.e[j][i] = m.e[i][j];
		}
	}
	return r;
}
static v4 operator*(m4 m, v4 v)
{
	v4 r = m.c[0] * v.x + m.c[1] * v.y + m.c[2] * v.z + m.c[3] * v.w;
	return r;
}
inline m4 projection(f32 aspectRatio, f32 fieldOfView, f32 zNear, f32 zFar)
{
	m4 r;
	f32 ctg = 1.f / tanf(fieldOfView*0.5f);
	f32 a = zFar / (zNear - zFar);
	f32 b = zNear * zFar / (zNear - zFar);
	r.c[0] = { ctg, 0.f, 0.f, 0.f };
	r.c[1] = { 0.f, aspectRatio*ctg, 0.f, 0.f };
	r.c[2] = { 0.f, 0.f, a, -1.f };
	r.c[3] = { 0, 0.f, b, 0.f };
	return r;
}
inline m4 invertOrtho3Translation(m4 m)
{
	m4 r = m;
	r.translation = {};
	r = transpose(r);
	r.translation = -m.translation.x * r.xAxis - m.translation.y * r.yAxis - m.translation.z * r.zAxis;
	return r;
}
inline m4 makeTransform(v3 xAxis, v3 yAxis, v3 zAxis, v3 pos)
{
	m4 r = {};
	r.e[3][3] = 1.f;
	r.xAxis = xAxis;
	r.yAxis = yAxis;
	r.zAxis = zAxis;
	r.translation = pos;
	return r;

}
inline m4 makeInversTransform(v3 xAxis, v3 yAxis, v3 zAxis, v3 pos)
{
	m4 r = makeTransform(xAxis, yAxis, zAxis, pos);
	r = invertOrtho3Translation(r);
	return r;
}

inline m4 identityM4()
{
	m4 r = {};
	r.e[0][0] = 1.f;
	r.e[1][1] = 1.f;
	r.e[2][2] = 1.f;
	r.e[3][3] = 1.f;
	return r;
}

inline m4 translation(v3 translation)
{
	m4 r = identityM4();
	r.translation += translation;
	return r;
}

inline v3 spherePoint(f32 u, f32 v)
{
	u *= 2.f * pi32;
	v *= pi32;

	f32 cos_u = cosf(u);
	f32 sin_u = sinf(u);

	f32 cos_v = cosf(v);
	f32 sin_v = sinf(v);

	v3 result =
	{
		sin_v * cos_u,
		cos_v,
		-sin_v * sin_u,
	};

	return result;
}

inline v3 dspherePointdu(f32 u, f32 v)
{
	u *= 2.f * pi32;
	v *= pi32;

	f32 cos_u = cosf(u);
	f32 sin_u = sinf(u);

	f32 cos_v = cosf(v);
	f32 sin_v = sinf(v);

	v3 result =
	{
		-2.f*pi32*sin_u*sin_v,
		0.f,
		-2.f*pi32*cos_u*sin_v
	};

	return result;
}

inline v3 dspherePointdv(f32 u, f32 v)
{
	u *= 2.f * pi32;
	v *= pi32;

	f32 cos_u = cosf(u);
	f32 sin_u = sinf(u);

	f32 cos_v = cosf(v);
	f32 sin_v = sinf(v);

	v3 result =
	{
		cos_u*pi32*cos_v,
		-pi32 * sin_v,
		-sin_u*pi32*cos_v
	};

	return result;
}

inline f32 smoothStep(f32 t)
{
	return t * t*t*(10.f + t * (6.f*t - 15.f));
}

inline f32 smoothBlend(f32 a, f32 b, f32 t)
{
	//return smoothStep(1.f - t) * a + smoothStep(t) * b;
	f32 s = smoothStep(t);
	return a + s * (b - a);
}

inline f32 lerp(f32 a, f32 b, f32 t)
{
	return (1.f - t)*a + t * b;
}
inline v2 lerp(v2 a, v2 b, f32 t)
{
	return (1.f - t)*a + t * b;
}
inline v3 lerp(v3 a, v3 b, f32 t)
{
	return (1.f - t)*a + t * b;
}
inline v4 lerp(v4 a, v4 b, f32 t)
{
	return (1.f - t)*a + t * b;
}

inline f32 clamp(f32 min, f32 max, f32 value)
{
	f32 result = MIN(MAX(value, min), max);
	return result;
}
inline v2 clamp(f32 min, f32 max, v2 value)
{
	return { clamp(min, max, value.x), clamp(min, max, value.y) };
}
inline v3 clamp(f32 min, f32 max, v3 value)
{
	return { clamp(min, max, value.x), clamp(min, max, value.y), clamp(min, max, value.z) };
}
inline v4 clamp(f32 min, f32 max, v4 value)
{
	return { clamp(min, max, value.x), clamp(min, max, value.y), clamp(min, max, value.z), clamp(min, max, value.w) };

}

inline f32 saturate(f32 a)
{
	return MIN(MAX(a, 0.f), 1.f);
}

inline v4 saturate(v4 v)
{
	return 
	{
		saturate(v.x),
		saturate(v.y),
		saturate(v.z),
		saturate(v.w)
	};
}