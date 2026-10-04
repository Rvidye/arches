#pragma once

//Fixed function rasteriation maths
/*
* Conventions
* 
* Raster Space: x to the right, y to framebuffer row 0. PNGs are written row-flipped
*
* Clip Space: w is distance along the camera's view axis, x/w and y/w lie in [-1, 1] on screen
* 
* Fixed point: Screen position snap to 1/256 pixel (from D3D Speac) rounding to nearest
*/

#include "vec2.hpp"
#include "vec3.hpp"
#include "vec4.hpp"

#ifndef __riscv
#include "camera.hpp"
#include <cmath>
#include <cstdint>
#include <utility>
#endif // !__riscv

namespace rtm {
namespace raster {

	//4x4 row-major matrix
	struct mat4
	{
		float m[4][4];

		vec4 operator*(const vec4& v) const
		{
			return vec4(
				m[0][0] * v.x + m[0][1] * v.y + m[0][2] * v.z + m[0][3] * v.w,
				m[1][0] * v.x + m[1][1] * v.y + m[1][2] * v.z + m[1][3] * v.w,
				m[2][0] * v.x + m[2][1] * v.y + m[2][2] * v.z + m[2][3] * v.w,
				m[3][0] * v.x + m[3][1] * v.y + m[3][2] * v.z + m[3][3] * v.w);
		}
	};

	//A vertex shader's output
	constexpr uint32_t MAX_ATTRIBUTES = 12;

	struct ClipVertex
	{
		vec4 position;
		float attr[MAX_ATTRIBUTES];
	};

#ifndef __riscv

	constexpr int SUBPIXEL_BITS = 8;
	constexpr int64_t SUBPIXEL_ONE = int64_t(1) << SUBPIXEL_BITS;
	constexpr int64_t PIXEL_CENTRE = SUBPIXEL_ONE / 2;

	constexpr float GUARD_BAND = 8.0f;

	constexpr uint32_t NUM_CLIP_PLANES = 5;
	constexpr uint32_t MAX_CLIPPED_VERTICES = 3 + NUM_CLIP_PLANES;

	// projection
	inline mat4 view_projection(const Camera& camera, float near)
	{
		auto dot = [](const vec3& a, const vec3& b) { return (double)a.x * b.x + (double)a.y * b.y + (double)a.z * b.z; };

		const vec3& X = camera._x;
		const vec3& Y = camera._y;
		const vec3& Z = camera._z;
		const vec3& O = camera._position;

		const double sx = 2.0 / dot(X, X);
		const double sy = 2.0 / dot(Y, Y);

		mat4 r;
		r.m[0][0] = (float)(sx * X.x);	r.m[0][1] = (float)(sx * X.y);	r.m[0][2] = (float)(sx * X.z);	r.m[0][3] = (float)(-sx * dot(X, O));
		r.m[1][0] = (float)(sy * Y.x);	r.m[1][1] = (float)(sy * Y.y);	r.m[1][2] = (float)(sy * Y.z);	r.m[1][3] = (float)(-sy * dot(Y, O));
		r.m[2][0] = 0.0f;				r.m[2][1] = 0.0f;				r.m[2][2] = 0.0f;				r.m[2][3] = near;
		r.m[3][0] = -Z.x;				r.m[3][1] = -Z.y;				r.m[3][2] = -Z.z;				r.m[3][3] = (float)dot(Z, O);
		return r;
	}

	// clipping
	inline float clip_distance(const vec4& p, uint32_t plane, float near)
	{
		switch (plane)
		{
		case 0: return p.w - near;
		case 1: return p.x + GUARD_BAND * p.w;
		case 2: return GUARD_BAND * p.w - p.x;
		case 3: return p.y + GUARD_BAND * p.w;
		default: return GUARD_BAND * p.w - p.y;
		}
	}

	inline ClipVertex lerp(const ClipVertex& a, const ClipVertex& b, float t)
	{
		ClipVertex r{};
		r.position = vec4(	a.position.x + (b.position.x - a.position.x) * t,
							a.position.y + (b.position.y - a.position.y) * t, 
							a.position.z + (b.position.z - a.position.z) * t, 
							a.position.w + (b.position.w - a.position.w) * t);
		for (uint32_t i = 0; i < MAX_ATTRIBUTES; ++i)
			r.attr[i] = a.attr[i] + (b.attr[i] - a.attr[i]) * t;
		return r;
	}

	inline uint32_t clip_triangle(const ClipVertex in[3], float near, ClipVertex out[MAX_CLIPPED_VERTICES])
	{
		uint32_t codes[3] = {0, 0, 0};
		for (uint32_t i = 0; i < 3; ++i)
			for (uint32_t p = 0; p < NUM_CLIP_PLANES; ++p)
				if (clip_distance(in[i].position, p, near) < 0.0f)
					codes[i] |= 1u << p;

		if ((codes[0] | codes[1] | codes[2]) == 0)
		{
			out[0] = in[0];
			out[1] = in[1];
			out[2] = in[2];
			return 3;
		}
		if (codes[0] & codes[1] & codes[2]) return 0;

		ClipVertex buffers[2][MAX_CLIPPED_VERTICES];
		uint32_t count = 3, current = 0;
		buffers[0][0] = in[0];
		buffers[0][1] = in[1];
		buffers[0][2] = in[2];

		const uint32_t planes = codes[0] | codes[1] | codes[2];
		for (uint32_t p = 0; p < NUM_CLIP_PLANES; ++p)
		{
			if (!(planes & (1u << p))) continue;

			const ClipVertex* src = buffers[current];
			ClipVertex* dst = buffers[current ^ 1];
			uint32_t n = 0;
			for (uint32_t i = 0; i < count; ++i)
			{
				const ClipVertex& a = src[i];
				const ClipVertex& b = src[(i + 1) % count];
				const float da = clip_distance(a.position, p, near);
				const float db = clip_distance(b.position, p, near);
				const bool a_in = da >= 0.0f, b_in = db >= 0.0f;

				if (a_in) dst[n++] = a;
				if (a_in != b_in)
				{
					if (a_in) dst[n++] = lerp(a, b, da / (da - db));
					else dst[n++] = lerp(b, a, db / (db - da));
				}
			}

			count = n;
			current ^= 1;
			if (count < 3) return 0;
		}

		for (uint32_t i = 0; i < count; ++i)
			out[i] = buffers[current][i];
		return count;
	}

	//snapping / setup
	inline int64_t snap_to_fixed(double pixels)
	{
		return (int64_t)std::nearbyint(pixels * (double)SUBPIXEL_ONE);
	}

	struct ScreenVertex
	{
		int64_t x, y;
		double inv_w;
		float attr[MAX_ATTRIBUTES];
	};

	inline ScreenVertex to_screen(const ClipVertex& v, uint32_t width, uint32_t height)
	{
		ScreenVertex s;
		s.inv_w = 1.0 / (double)v.position.w;
		s.x = snap_to_fixed(((double)v.position.x * s.inv_w * 0.5 + 0.5) * width);
		s.y = snap_to_fixed(((double)v.position.y * s.inv_w * 0.5 + 0.5) * height);
		for (uint32_t i = 0; i < MAX_ATTRIBUTES; ++i)
			s.attr[i] = v.attr[i];
		return s;
	}

	inline int64_t edge_function(const ScreenVertex& a, const ScreenVertex& b, int64_t px, int64_t py)
	{
		return (b.x - a.x) * (py - a.y) - (b.y - a.y) * (px - a.x);
	}

	inline bool is_top_left(const ScreenVertex& a, const ScreenVertex& b)
	{
		const int64_t dx = b.x - a.x, dy = b.y - a.y;
		return dy < 0 || (dy == 0 && dx > 0);
	}

	inline int64_t floor_div(int64_t a, int64_t b) //b > 0
	{
		return a >= 0 ? a / b : -((-a + b - 1) / b);
	}

	struct TriangleSetup
	{
		ScreenVertex v[3];
		int64_t area2;    
		int64_t bias[3];  
		int32_t px0, py0; 
		int32_t px1, py1;
	};

	//Returns false when there is nothing to rasterize: zero area, or no pixel centre in the bounds.
	inline bool setup_triangle(const ScreenVertex& a, const ScreenVertex& b, const ScreenVertex& c, uint32_t width, uint32_t height, TriangleSetup& t)
	{
		t.v[0] = a; t.v[1] = b; t.v[2] = c;
		int64_t area2 = edge_function(t.v[0], t.v[1], t.v[2].x, t.v[2].y);
		if (area2 == 0) return false;
		if (area2 < 0) //double-sided: flip the winding instead of culling
		{
			std::swap(t.v[1], t.v[2]);
			area2 = -area2;
		}
		t.area2 = area2;

		for (uint32_t i = 0; i < 3; ++i)
			t.bias[i] = is_top_left(t.v[i], t.v[(i + 1) % 3]) ? 0 : -1;

		int64_t min_x = t.v[0].x, max_x = t.v[0].x, min_y = t.v[0].y, max_y = t.v[0].y;
		for (uint32_t i = 1; i < 3; ++i)
		{
			if (t.v[i].x < min_x) min_x = t.v[i].x;
			if (t.v[i].x > max_x) max_x = t.v[i].x;
			if (t.v[i].y < min_y) min_y = t.v[i].y;
			if (t.v[i].y > max_y) max_y = t.v[i].y;
		}

		//Pixel p is a candidate when its centre p * 256 + 128 lies within [min, max].
		int64_t px0 = floor_div(min_x - PIXEL_CENTRE + SUBPIXEL_ONE - 1, SUBPIXEL_ONE);
		int64_t px1 = floor_div(max_x - PIXEL_CENTRE, SUBPIXEL_ONE);
		int64_t py0 = floor_div(min_y - PIXEL_CENTRE + SUBPIXEL_ONE - 1, SUBPIXEL_ONE);
		int64_t py1 = floor_div(max_y - PIXEL_CENTRE, SUBPIXEL_ONE);
		if (px0 < 0) px0 = 0;
		if (py0 < 0) py0 = 0;
		if (px1 > (int64_t)width - 1) px1 = (int64_t)width - 1;
		if (py1 > (int64_t)height - 1) py1 = (int64_t)height - 1;
		if (px0 > px1 || py0 > py1) return false;

		t.px0 = (int32_t)px0; t.px1 = (int32_t)px1;
		t.py0 = (int32_t)py0; t.py1 = (int32_t)py1;
		return true;
	}

	inline bool coverage(const TriangleSetup& t, int32_t px, int32_t py, int64_t e[3])
	{
		const int64_t x = (int64_t)px * SUBPIXEL_ONE + PIXEL_CENTRE;
		const int64_t y = (int64_t)py * SUBPIXEL_ONE + PIXEL_CENTRE;
		e[0] = edge_function(t.v[1], t.v[2], x, y);
		e[1] = edge_function(t.v[2], t.v[0], x, y);
		e[2] = edge_function(t.v[0], t.v[1], x, y);
		return e[0] + t.bias[1] >= 0 && e[1] + t.bias[2] >= 0 && e[2] + t.bias[0] >= 0;
	}

	//plane equation
	struct PlaneEquation
	{
		float a, b, c;
	};

	struct TrianglePlanes
	{
		int64_t x0, y0;                      
		uint32_t num_attributes;
		PlaneEquation depth;                 
		PlaneEquation inv_w;                 
		PlaneEquation attr[MAX_ATTRIBUTES];  
	};

	inline PlaneEquation plane_through(double q0, double q1, double q2, double dx1, double dy1, double dx2, double dy2, double det)
	{
		PlaneEquation p;
		p.a = (float)(((q1 - q0) * dy2 - (q2 - q0) * dy1) / det);
		p.b = (float)(((q2 - q0) * dx1 - (q1 - q0) * dx2) / det);
		p.c = (float)q0;
		return p;
	}

	inline TrianglePlanes setup_planes(const TriangleSetup& t, uint32_t num_attributes, float near)
	{
		TrianglePlanes p;
		p.x0 = t.v[0].x;
		p.y0 = t.v[0].y;
		p.num_attributes = num_attributes;

		const double dx1 = (double)(t.v[1].x - t.v[0].x) / SUBPIXEL_ONE, dy1 = (double)(t.v[1].y - t.v[0].y) / SUBPIXEL_ONE;
		const double dx2 = (double)(t.v[2].x - t.v[0].x) / SUBPIXEL_ONE, dy2 = (double)(t.v[2].y - t.v[0].y) / SUBPIXEL_ONE;
		const double det = dx1 * dy2 - dx2 * dy1;
		const double w0 = t.v[0].inv_w, w1 = t.v[1].inv_w, w2 = t.v[2].inv_w;

		p.inv_w = plane_through(w0, w1, w2, dx1, dy1, dx2, dy2, det);
		p.depth = plane_through(near * w0, near * w1, near * w2, dx1, dy1, dx2, dy2, det);
		for (uint32_t i = 0; i < num_attributes; ++i)
			p.attr[i] = plane_through(t.v[0].attr[i] * w0, t.v[1].attr[i] * w1, t.v[2].attr[i] * w2, dx1, dy1, dx2, dy2, det);
		return p;
	}

	inline void pixel_offset(const TrianglePlanes& p, int32_t px, int32_t py, float& dx, float& dy)
	{
		dx = (float)((int64_t)px * SUBPIXEL_ONE + PIXEL_CENTRE - p.x0) / (float)SUBPIXEL_ONE;
		dy = (float)((int64_t)py * SUBPIXEL_ONE + PIXEL_CENTRE - p.y0) / (float)SUBPIXEL_ONE;
	}

	inline float evaluate(const PlaneEquation& q, float dx, float dy)
	{
		return q.c + q.a * dx + q.b * dy;
	}

	inline float interpolate_depth(const TrianglePlanes& p, int32_t px, int32_t py)
	{
		float dx, dy;
		pixel_offset(p, px, py, dx, dy);
		return evaluate(p.depth, dx, dy);
	}

	inline float interpolate_attribute(const TrianglePlanes& p, uint32_t i, int32_t px, int32_t py)
	{
		float dx, dy;
		pixel_offset(p, px, py, dx, dy);
		return evaluate(p.attr[i], dx, dy) / evaluate(p.inv_w, dx, dy);
	}

	//depth rule

	constexpr float CLEAR_DEPTH = 0.0f;
	constexpr uint32_t CLEAR_SEQ = ~0u;

	inline bool depth_test(float depth, uint32_t seq, float stored_depth, uint32_t stored_seq)
	{
		return depth > stored_depth || (depth == stored_depth && seq < stored_seq);
	}

#endif // !__riscv
} // namepspace raster
}// namespace rtm

