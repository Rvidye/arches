#pragma once
#include "stdafx.hpp"

#define TRAX_USE_RT_CORE 1
#define TRAX_USE_HARDWARE_INTERSECTORS 0
#define TRAX_KERNEL_ARGS_ADDRESS 256ull

#define USE_HECWBVH_V1 0

namespace rtm {
#if USE_HECWBVH_V1
typedef rtm::HECWBVH CWBVH;
#else
typedef rtm::HE2CWBVH CWBVH;
#endif
}

struct Sphere
{
	rtm::vec3 center;
	float radius;
	rtm::vec3 color;
};

struct TRaXKernelArgs
{
	uint32_t framebuffer_width;
	uint32_t framebuffer_height;
	uint32_t framebuffer_size;
	uint32_t* framebuffer;

	bool pregen_rays;

	rtm::Camera camera;
	rtm::vec3 light_dir;
	rtm::Ray* rays;
	rtm::CWBVH::Node* nodes;
	rtm::FTB* ft_blocks;

	//mesh data
	rtm::uvec3* vertex_indices;
	rtm::uvec3* normal_indices;
	rtm::uvec3* tex_coord_indices;

	rtm::vec3* vertices;
	rtm::vec3* normals;
	rtm::vec2* tex_coords;

	uint* material_indices;
	rtm::Material* materials;
};
