#pragma once
#include "stdafx.hpp"

#include "rtm/raster.hpp"

#define RASTER_KERNEL_ARGS_ADDRESS 256ull

struct RasterVertex
{
	rtm::vec3 position;
	rtm::vec3 normal;
	rtm::vec2 uv;
};
static_assert(sizeof(RasterVertex) == 32, "a vertex is one 32 B cache sector");

enum RasterAttribute : uint32_t
{
	ATTR_U, ATTR_V,
	ATTR_NX, ATTR_NY, ATTR_NZ,
	NUM_RASTER_ATTRIBUTES
};
static_assert(NUM_RASTER_ATTRIBUTES <= rtm::raster::MAX_ATTRIBUTES, "");

struct RasterDraw
{
	uint32_t material_id;
	uint32_t num_attributes;
	uint32_t pad[2];
};

enum RasterCommandOp : uint32_t
{
	RASTER_CMD_END = 0,
	RASTER_CMD_DRAW_INDEXED = 1,
};

struct RasterCommand
{
	uint32_t op;
	uint32_t draw_id;
	uint32_t first_index;
	uint32_t index_count;
};
static_assert(sizeof(RasterCommand) == 16, "");

struct RasterKernelArgs
{
	uint32_t framebuffer_width;
	uint32_t framebuffer_height;
	uint32_t framebuffer_size;
	uint32_t* framebuffer;

	rtm::raster::mat4 view_proj;
	float near_plane;

	const RasterVertex* vertices;
	const uint32_t* indices;
	const RasterCommand* commands;
	const RasterDraw* draws;
	const rtm::Material* materials;
};

