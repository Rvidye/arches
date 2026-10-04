#include "stdafx.hpp"
#include "include.hpp"
#include "sim-config.hpp"
#include "stbi/stb_image_write.h"

#include <chrono>
#include <unordered_map>

using namespace rtm::raster;

//scene setup 

struct Scene
{
	std::vector<RasterVertex> vertices;
	std::vector<uint32_t> indices;
	std::vector<RasterDraw> draws;
	std::vector<RasterCommand> commands;
};

static Scene build_scene(const rtm::Mesh& mesh)
{
	struct Key
	{
		uint32_t v, n, t;
		bool operator==(const Key&) const = default;
	};

	struct KeyHash
	{
		size_t operator()(const Key& k) const { return std::hash<uint64_t>()(((uint64_t)k.v << 32 | k.n) * 0x9E3779B97F4A7C15ull ^ k.t); }
	};

	uint32_t num_materials = (uint32_t)mesh.materials.size();
	for (uint32_t m : mesh.material_indices) num_materials = std::max(num_materials, m + 1);
	std::vector<std::vector<uint32_t>> by_material(num_materials);
	for (uint32_t tri = 0; tri < mesh.vertex_indices.size(); ++tri)
		by_material[mesh.material_indices[tri]].push_back(tri);

	Scene scene;
	std::unordered_map<Key, uint32_t, KeyHash> vertex_ids;
	for (uint32_t m = 0; m < num_materials; ++m)
	{
		if (by_material[m].empty()) continue;
		scene.commands.push_back({ RASTER_CMD_DRAW_INDEXED, (uint32_t)scene.draws.size(), (uint32_t)scene.indices.size(), (uint32_t)by_material[m].size() * 3 });
		scene.draws.push_back({ m, NUM_RASTER_ATTRIBUTES });

		for (uint32_t tri : by_material[m])
			for (uint32_t k = 0; k < 3; ++k)
			{
				const Key key{ mesh.vertex_indices[tri][k], mesh.normal_indices[tri][k], mesh.tex_coord_indices[tri][k] };
				auto [it, added] = vertex_ids.emplace(key, (uint32_t)scene.vertices.size());
				if (added) scene.vertices.push_back({ mesh.vertices[key.v], mesh.normals[key.n], mesh.tex_coords[key.t] });
				scene.indices.push_back(it->second);
			}
	}
	scene.commands.push_back({ RASTER_CMD_END, 0, 0, 0 });
	return scene;
}

// shaders
static uint32_t encode_pixel(rtm::vec3 c)
{
	c = rtm::clamp(c, 0.0f, 1.0f);
	return (uint32_t)(c.r * 255.0f + 0.5f) | (uint32_t)(c.g * 255.0f + 0.5f) << 8 | (uint32_t)(c.b * 255.0f + 0.5f) << 16 | 0xffu << 24;
}

static rtm::vec4 sample2d(const Texture2D& texture, const rtm::vec2& uv)
{
	auto texel = [&](float dx, float dy) { return Texture2D::decode_texel(*texture.get_texel_addr(texture.get_iuv(uv, rtm::vec2(dx, dy)))); };
	const rtm::vec2 f = texture.get_fract_uv(uv);
	return rtm::mix(rtm::mix(texel(0, 0), texel(1, 0), f[0]), rtm::mix(texel(0, 1), texel(1, 1), f[0]), f[1]);
}

static ClipVertex vertex_shader(const RasterKernelArgs& args, uint32_t vertex_index)
{
	const RasterVertex& v = args.vertices[vertex_index];
	ClipVertex out{};
	out.position = args.view_proj * rtm::vec4(v.position.x, v.position.y, v.position.z, 1.0f);
	out.attr[ATTR_U] = v.uv.x;
	out.attr[ATTR_V] = v.uv.y;
	out.attr[ATTR_NX] = v.normal.x;
	out.attr[ATTR_NY] = v.normal.y;
	out.attr[ATTR_NZ] = v.normal.z;
	return out;
}

static uint32_t fragment_shader(const RasterKernelArgs& args, uint32_t draw_id, const TrianglePlanes& planes, int32_t px, int32_t py)
{
	const rtm::Material& material = args.materials[args.draws[draw_id].material_id];
	if (!material.use_am) return encode_pixel(rtm::vec3(1.0f, 0.0f, 0.0f));
	const rtm::vec2 uv(interpolate_attribute(planes, ATTR_U, px, py), interpolate_attribute(planes, ATTR_V, px, py));
	const rtm::vec4 albedo = sample2d(material.albedo_texture, uv);
	return encode_pixel(rtm::vec3(albedo.x, albedo.y, albedo.z));
}

int main(int argc, char* argv[])
{
	setvbuf(stdout, nullptr, _IONBF, 0);
	Arches::SimulationConfig sim_config;
	sim_config.parse(argc, argv);

	const std::string scene_name = sim_config.get_string("scene-name");
	rtm::Mesh mesh(sim_config.get_string("dataset-dir") + "/" + scene_name + ".obj");
	const Scene scene = build_scene(mesh);

	RasterKernelArgs  args{};
	args.framebuffer_width = sim_config.get_int("framebuffer-width");
	args.framebuffer_height = sim_config.get_int("framebuffer-height");
	args.framebuffer_size = args.framebuffer_width * args.framebuffer_height;
	args.near_plane = T_MIN;
	args.view_proj = rtm::raster::view_projection(sim_config.camera, args.near_plane);
	args.vertices = scene.vertices.data();
	args.indices = scene.indices.data();
	args.commands = scene.commands.data();
	args.draws = scene.draws.data();
	args.materials = mesh.materials.data();

	const uint32_t width = args.framebuffer_width, height = args.framebuffer_height;
	std::vector<uint32_t> color(args.framebuffer_size, 0xff000000u);
	std::vector<float> depth(args.framebuffer_size, CLEAR_DEPTH);
	std::vector<uint32_t> depth_seq(args.framebuffer_size, CLEAR_SEQ);
	args.framebuffer = color.data();

	const auto start = std::chrono::steady_clock::now();
	for (const RasterCommand& command : scene.commands)
	{
		if (command.op != RASTER_CMD_DRAW_INDEXED) break;
		const RasterDraw& draw = args.draws[command.draw_id];

		for (uint32_t first = command.first_index; first < command.first_index + command.index_count; first += 3)
		{
			const uint32_t seq = first / 3;

			ClipVertex clip[3], polygon[MAX_CLIPPED_VERTICES];
			for (uint32_t k = 0; k < 3; ++k) clip[k] = vertex_shader(args, args.indices[first + k]);
			const uint32_t count = clip_triangle(clip, args.near_plane, polygon);

			ScreenVertex screen[MAX_CLIPPED_VERTICES];
			for (uint32_t k = 0; k < count; ++k) screen[k] = to_screen(polygon[k], width, height);

			for (uint32_t k = 1; k + 1 < count; ++k)
			{
				TriangleSetup setup;
				if (!setup_triangle(screen[0], screen[k], screen[k + 1], width, height, setup)) continue;
				const TrianglePlanes planes = setup_planes(setup, draw.num_attributes, args.near_plane);

				for (int32_t py = setup.py0; py <= setup.py1; ++py)
				{
					for (int32_t px = setup.px0; px <= setup.px1; ++px)
					{
						int64_t e[3];
						if (!coverage(setup, px, py, e)) continue;

						const float z = interpolate_depth(planes, px, py);
						const uint32_t i = (uint32_t)py * width + (uint32_t)px;
						if (!depth_test(z, seq, depth[i], depth_seq[i])) continue;

						depth[i] = z;
						depth_seq[i] = seq;
						color[i] = fragment_shader(args, command.draw_id, planes, px, py);
					}
				}
			}
		}
	}
	const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
	printf("Rasterized %s: %zu triangles, %ux%u, %.2f s\n", scene_name.c_str(), scene.indices.size() / 3, width, height, seconds);

	std::vector<uint32_t> depth_image(args.framebuffer_size);
	for (uint32_t i = 0; i < args.framebuffer_size; ++i)
		depth_image[i] = encode_pixel(rtm::vec3(depth[i] > 0.0f ? 1.0f + std::log2(depth[i]) / 16.0f : 0.0f));

	stbi_flip_vertically_on_write(true);
	stbi_write_png("color.png", width, height, 4, color.data(), 0);
	stbi_write_png("depth.png", width, height, 4, depth_image.data(), 0);
	printf("Raster Complete");
	return 0;
}
