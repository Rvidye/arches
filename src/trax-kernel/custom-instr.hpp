#pragma once
#include "stdafx.hpp"

#ifndef __riscv
static std::atomic_uint _next_thread;
#endif

// uint32_t inline fchthrd()
// {
// #ifdef __riscv
// 	uint32_t value = 0;
// 	asm volatile("fchthrd %0\n\t" : "=r" (value));
// 	return value;
// #else
// 	return _next_thread++;
// #endif
// }

uint32_t inline fchthrd()
{
#ifdef __riscv
	uint32_t value = 0;
	asm volatile(".insn i 0x000b, 0, %0, x0, 0\n\t" : "=r" (value));
	return value;
#else
	return _next_thread++;
#endif
}

#ifndef __riscv
void reset_fchthrd()
{
 	_next_thread = 0;
}
#endif

inline void ebreak()
{
	#ifdef __riscv
	asm volatile
	(
		"ebreak\n\t"
	);
	#endif
}

rtm::vec4 inline sample2d(Texture2D* texture, rtm::vec2 uv)
{
#ifdef __riscv
	uint32_t addr = (uint64_t)texture;
	register float src0 asm("f0") = *(float*)&addr;
	register float src1 asm("f1") = uv.x;
	register float src2 asm("f2") = uv.y;

	register float dst0 asm("f28");
	register float dst1 asm("f29");
	register float dst2 asm("f30");
	register float dst3 asm("f31");

	asm volatile(".insn i 0xb, 0x6, %0, %4, 0\n\t" : "=f" (dst0), "=f" (dst1), "=f" (dst2), "=f" (dst3) : "f" (src0), "f" (src1), "f" (src2));
	return rtm::vec4(dst0, dst1, dst2, dst3);
#else
	return texture->sample(uv);
#endif
}

inline float _spherisect(const rtm::Ray& ray, const Sphere& sphere)
{
#if defined(__riscv)
	register float src0 asm("f0") = ray.o.x;
	register float src1 asm("f1") = ray.o.y;
	register float src2 asm("f2") = ray.o.z;
	register float src3 asm("f3") = ray.t_min;
	register float src4 asm("f4") = ray.d.x;
	register float src5 asm("f5") = ray.d.y;
	register float src6 asm("f6") = ray.d.z;
	register float src7 asm("f7") = ray.t_max;

	register float src8 asm("f8") = sphere.center.x;
	register float src9 asm("f9") = sphere.center.y;
	register float src10 asm("f10") = sphere.center.z;
	register float src11 asm("f11") = sphere.radius;

	float t;
	asm volatile(".insn u 0xb, %0, 0x18\n\t"
		: "=f" (t)
		: "f" (src0), "f" (src1), "f" (src2), "f" (src3),
		"f" (src4), "f" (src5), "f" (src6), "f" (src7),
		"f" (src8), "f" (src9), "f" (src10), "f" (src11));
	return t;
#else
	return intersect_sphere(ray, sphere);
#endif
}
