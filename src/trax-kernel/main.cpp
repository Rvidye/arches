#include "stdafx.hpp"

#include "include.hpp"
#include "intersect.hpp"
#include "custom-instr.hpp"
#include "rtm/anim.hpp"

inline static uint32_t encode_pixel(rtm::vec3 in)
{
	in = rtm::clamp(in, 0.0f, 1.0f);
	uint32_t out = 0u;
	out |= static_cast<uint32_t>(in.r * 255.0f + 0.5f) << 0;
	out |= static_cast<uint32_t>(in.g * 255.0f + 0.5f) << 8;
	out |= static_cast<uint32_t>(in.b * 255.0f + 0.5f) << 16;
	out |= 0xff << 24;
	return out;
}

inline rtm::vec3 palette(float t)
{
	t = rtm::clamp(t, 0.0, 1.0f);

	const rtm::vec3 k[] = {
		rtm::vec3( 000.13572138f, 000.09140261f, 000.10667330f),
		rtm::vec3( 004.61539260f, 002.19418839f, 012.64194608f),
		rtm::vec3(-042.66032258f, 004.84296658f,-060.58204836f),
		rtm::vec3( 132.13108234f,-014.18503333f, 110.36276771f),
		rtm::vec3(-152.94239396f, 004.27729857f,-089.90310912f),
		rtm::vec3( 059.28637943f, 002.82956604f, 027.34824973f)
	};
	
	float t_pow = t;
	rtm::vec3 color(k[1] * t + k[0]);
	for(uint i = 2; i < 6; ++i)
	{
		t_pow *= t;
		color += k[i] * t_pow;
	}

	return rtm::clamp(color);
}

#ifndef  __riscv
static std::atomic_uint node_steps = 0;
static std::atomic_uint prim_steps = 0;
#endif

inline static void kernel(const TRaXKernelArgs& args)
{
	constexpr uint32_t SPP = 1;
	constexpr uint TILE_X = 4;
	constexpr uint TILE_Y = 8;
	constexpr uint TILE_SIZE = TILE_X * TILE_Y;
	
	for (uint index = fchthrd(); index < args.framebuffer_size; index = fchthrd())
	{
		uint tile_id = index / TILE_SIZE;
		//tile_id = rtm::RNG::fast_hash(tile_id) % (args.framebuffer_size / TILE_SIZE);
		uint32_t tile_x = tile_id % (args.framebuffer_width / TILE_X);
		uint32_t tile_y = tile_id / (args.framebuffer_width / TILE_X);
		uint thread_id = index % TILE_SIZE;
		uint32_t x = tile_x * TILE_X + thread_id % TILE_X;
		uint32_t y = tile_y * TILE_Y + thread_id / TILE_X;
		uint fb_index = y * args.framebuffer_width + x;
		
		rtm::RNG rng(fb_index);
		IntersectStats stats;

	#if 0
		float radiance = 0.0f;
		for(uint32_t i = 0; i < SPP; ++i)
		{
			float throughput = 1.0f;
			rtm::Ray ray = args.pregen_rays ? args.rays[fb_index] : args.camera.generate_ray_through_pixel(x, y);
			for(uint32_t j = 0; j < 3; ++j)
			{
				rtm::Hit hit(ray.t_max, rtm::vec2(0.0f), ~0u);
			#if defined(__riscv) && (TRAX_USE_RT_CORE)
				_traceray<0x0u>(index, ray, hit);
			#else
				intersect(args.nodes, args.ftbs, ray, hit, stats);
			#endif

				if(hit.t >= ray.t_max)
				{
					radiance += throughput * 2.0f;
					break;
				}

				rtm::vec3 n = args.tris[hit.id].normal();
				uint hash = rtm::RNG::hash(hit.id) | 0xff'00'00'00;

				ray.o += ray.d * hit.t;
				ray.d = cosine_sample_hemisphere(n, rng); // generate secondray rays
				throughput *= 0.8f;
			}
		}

		args.framebuffer[fb_index] = (stats.node_steps * sizeof(rtm::CWBVH::Node) + stats.prim_steps * sizeof(rtm::FTB)) / SPP;
		//args.framebuffer[fb_index] = encode_pixel(radiance / SPP);
	#else 
		rtm::Ray ray = args.pregen_rays ? args.rays[fb_index] : args.camera.generate_ray_through_pixel(x, y);
		rtm::Hit hit(ray.t_max, rtm::vec2(0.0f), ~0u);
			
	#if defined(__riscv) && (TRAX_USE_RT_CORE)
		_traceray<0x0u>(index, ray, hit);
	#else
		intersect(args.nodes, args.ft_blocks, ray, hit, stats);
	#endif

		if(hit.t < ray.t_max)
		{
			uint32_t mat_id = args.material_indices[hit.id];
			rtm::Material& mat = args.materials[mat_id];
			rtm::uvec3 tci = args.tex_coord_indices[hit.id];
			rtm::vec2 tc = args.tex_coords[tci[0]] * hit.bc[0] + args.tex_coords[tci[1]] * hit.bc[1] + args.tex_coords[tci[2]] * (1.0f - hit.bc[0] - hit.bc[1]);

			rtm::vec4 albedo;
			if(mat.use_am)
			{
				albedo = sample2d(&mat.albedo_texture, tc);
			}
			else
			{
				albedo = rtm::vec4(1.0f, 0.0f, 1.0f, 1.0f);
			}

			//args.framebuffer[fb_index] = rtm::RNG::hash(mat.use_am) | 0xff000000;
			args.framebuffer[fb_index] = encode_pixel(rtm::vec3(albedo.x, albedo.y, albedo.z));
		}
		else
		{
			args.framebuffer[fb_index] = 0xff000000;
		}
	#endif
	#ifndef __riscv
		node_steps += stats.node_steps;
		prim_steps += stats.prim_steps;
	#endif
	}
}

inline static void mandelbrot(const TRaXKernelArgs& args)
{
	constexpr uint MAX_ITERS = 100;
	for(uint index = fchthrd(); index < args.framebuffer_size; index = fchthrd())
	{
		rtm::Ray ray = args.rays[index];

		// convert 1-d loop in to 2-d loop indices
		const int i = index / args.framebuffer_width;
		const int j = index % args.framebuffer_width;

		float zreal = 0.0f;
		float zimag = 0.0f;
		float creal = (j - args.framebuffer_width / 1.4f) / (args.framebuffer_width / 2.0f);
		float cimag = (i - args.framebuffer_height / 2.0f) / (args.framebuffer_height / 2.0f);

		float lengthsq;
		uint k = 0;
		do
		{
			float temp = (zreal * zreal) - (zimag * zimag) + creal;
			zimag = (2.0f * zreal * zimag) + cimag;
			zreal = temp;
			lengthsq = (zreal * zreal) + (zimag * zimag);
			++k;
		}
		while(lengthsq < 4.f && k < MAX_ITERS);

		if(k == MAX_ITERS)
			k = 0;

		args.framebuffer[index] = encode_pixel(k / (float)MAX_ITERS);
	}
}

#ifdef __riscv 
int main()
{
	kernel(*(const TRaXKernelArgs*)TRAX_KERNEL_ARGS_ADDRESS);
	return 0;
}
#else

// #include <Windows.h>
int main(int argc, char* argv[])
{
	uint pregen_bounce = 0;
	std::string scene_name = argv[1];

	TRaXKernelArgs args;
	args.framebuffer_width = (argc > 2) ? atoi(argv[2]) : 1920;
	args.framebuffer_height = (argc > 3) ? atoi(argv[3]) : 1080;
	args.framebuffer_size = args.framebuffer_width * args.framebuffer_height;
	std::vector<uint32_t> fb_vec(args.framebuffer_size);
	args.framebuffer = fb_vec.data();

	args.pregen_rays = false;
	//SetPriorityClass(GetCurrentProcess(), REALTIME_PRIORITY_CLASS);

	args.light_dir = rtm::normalize(rtm::vec3(4.5f, 42.5f, 5.0f));
	if(scene_name.compare("sibenik") == 0)
		args.camera = rtm::Camera(args.framebuffer_width, args.framebuffer_height, 12.0f, rtm::vec3(3.0, -13.0, 0.0), rtm::vec3(0, -12.0, 0));
	if(scene_name.compare("crytek-sponza") == 0)
		args.camera = rtm::Camera(args.framebuffer_width, args.framebuffer_height, 12.0f, rtm::vec3(-900.6f, 150.8f, 120.74f), rtm::vec3(79.7f, 14.0f, -17.4f));
	if(scene_name.compare("intel-sponza") == 0)
		args.camera = rtm::Camera(args.framebuffer_width, args.framebuffer_height, 12.0f, rtm::vec3(-900.6f, 150.8f, 120.74f), rtm::vec3(79.7f, 14.0f, -17.4f));
	if (scene_name.compare("intel-sponza1") == 0)
		args.camera = rtm::Camera(args.framebuffer_width, args.framebuffer_height, 12.0f, rtm::vec3(1.0f, 1.0f, 0.0f), rtm::vec3(-1.0f, 1.0f, 0.0f));
	if(scene_name.compare("sponza") == 0)
		args.camera = rtm::Camera(args.framebuffer_width, args.framebuffer_height, 12.0f, rtm::vec3(0.0f, 2.0f, 0.0f), rtm::vec3(90.0f, 0.0f, -1.0f));
	if(scene_name.compare("san-miguel") == 0)
		args.camera = rtm::Camera(args.framebuffer_width, args.framebuffer_height, 12.0f, rtm::vec3(7.448, 1.014, 12.357), rtm::vec3(7.448 + 0.608, 1.014 + 0.026, 12.357 - 0.794));
	if(scene_name.compare("bistro") == 0)
		args.camera = rtm::Camera(args.framebuffer_width, args.framebuffer_height, 12.0f, rtm::vec3(-8.0, 2.0, 2.0), rtm::vec3(0.0f, 1.0f, -1.0f));
	if(scene_name.compare("hairball") == 0)
		args.camera = rtm::Camera(args.framebuffer_width, args.framebuffer_height, 24.0f, rtm::vec3(0.0, 0.0, 10.0), rtm::vec3(0.0f, 0.0f, 0.0f));
	if (scene_name.compare("wooddoll") == 0)
		args.camera = rtm::Camera(args.framebuffer_width, args.framebuffer_height, 12.0f, rtm::vec3(0.0f, 0.0f, 0.5f), rtm::vec3(0.0f, 0.5f, -1.0f));
	if (scene_name.compare("ben") == 0)
		args.camera = rtm::Camera(args.framebuffer_width, args.framebuffer_height, 50.0f, rtm::vec3(0.0f, 0.348f, 1.114f), rtm::vec3(0.0f, 0.348f, -0.002f));
	if (scene_name.compare("hand") == 0)
		args.camera = rtm::Camera(args.framebuffer_width, args.framebuffer_height, 50.0f, rtm::vec3(-0.005f, 0.314f, 1.087f), rtm::vec3(-0.005f, 0.314f, -0.220f));
	if (scene_name.compare("marbles") == 0)
		args.camera = rtm::Camera(args.framebuffer_width, args.framebuffer_height, 50.0f, rtm::vec3(0.0f, 297.74f, 813.3f), rtm::vec3(0.0f, 297.74f, -0.002f));

	std::string mesh_path = "datasets/" + scene_name + ".obj";
	std::string bvh_cache_path = "datasets/cache/" + scene_name + ".bvh";

	//Animation Flags scanned from argv:
	//--anim-frames=N --anim-frame=K --anim-strategy=rebuild|refit --deform-mag=.. --deform-mode=twist|explode|sine
	auto argstr = [&](const char* key, std::string def) { std::string p = std::string("--") + key + "="; for (int i = 1; i < argc; ++i) { std::string a = argv[i]; if (a.rfind(p, 0) == 0) return a.substr(p.size()); } return def; };
	auto argint = [&](const char* key, int def) { std::string s = argstr(key, ""); return s.empty() ? def : atoi(s.c_str()); };
	auto argflt = [&](const char* key, float def) { std::string s = argstr(key, ""); return s.empty() ? def : (float)atof(s.c_str()); };

	int         anim_frames = argint("anim-frames", 0);
	int         anim_frame = argint("anim-frame", 0);
	std::string anim_strat = argstr("anim-strategy", "rebuild");
	float       deform_mag = argflt("deform-mag", 0.0f);
	std::string deform_mode = argstr("deform-mode", "twist");
	bool        keyframe_anim = anim_frames > 0;
	bool        animate = keyframe_anim || deform_mag != 0.0f;

	rtm::KeyFrameAnim anim;
	float anim_t = 0.0f;
	if (keyframe_anim)
	{
		anim.init("datasets/" + scene_name + "/", scene_name + "_");
		anim_t = (anim_frames > 1) ? (float)anim_frame / (anim_frames - 1) * (anim.count - 1) : 0.0f;
		mesh_path = anim.path(0);
	}
	rtm::Mesh mesh(mesh_path);
	auto apply_pose = [&](rtm::Mesh& m) { if (keyframe_anim) anim.sample(anim_t, m); else rtm::deform_mesh(m, deform_mode, deform_mag); };

#if 0
	for(uint32_t i = 4; i < 5; ++i)
	{
		rtm::BVH::BuildArgs ba;
		ba.cache_path = bvh_cache_path.c_str();
		ba.width = 7;
		ba.build_method = rtm::BVH::SAH;
		ba.silent = false;

		ba.max_prims_merge = rtm::BVH::MAX_FTB;
		ba.merge_nodes = true;
		ba.merge_leafs = true;

		if(i == 0)
		{
			printf("\nGreedy Collapse-----------------------------------------------------------------\n");
			ba.leaf_cost = rtm::BVH::LINEAR;
			ba.max_prims_collapse = 3;
			ba.collapse_method = rtm::BVH::GREEDY;
		}

		if(i == 1)
		{
			printf("\nDynamic Collapse----------------------------------------------------------------\n");
			ba.leaf_cost = rtm::BVH::LINEAR;
			ba.max_prims_collapse = 3;
			ba.collapse_method = rtm::BVH::DYNAMIC;
		}

		if(i == 2)
		{
			printf("\nCompression-aware Greedy Collapse-----------------------------------------------\n");
			ba.leaf_cost = 2;
			ba.max_prims_collapse = rtm::BVH::MAX_FTB;
			ba.collapse_method = rtm::BVH::GREEDY;
		}

		if(i == 3)
		{
			printf("\nCompression-aware Dynamic Collapse----------------------------------------------\n");
			ba.leaf_cost = 2;
			ba.max_prims_collapse = rtm::BVH::MAX_FTB;
			ba.collapse_method = rtm::BVH::DYNAMIC;
		}

		if(i == 4)
		{
			printf("\nCompression-aware Dynamic Collapse----------------------------------------------\n");
			ba.leaf_cost = rtm::BVH::LINEAR;
			ba.max_prims_collapse = 1;
			ba.collapse_method = rtm::BVH::DYNAMIC;
			ba.merge_nodes = false;
			ba.merge_leafs = false;
		}


		float node_collapse_time = 0.0f, leaf_collapse_time = 0.0f, node_merge_time = 0.0f, leaf_merge_time = 0.0f;
		for(uint j = 0; j < 16; ++j)
		{
			rtm::Mesh mesh_cpy(mesh);
			rtm::BVH b(mesh_cpy, ba);
			node_collapse_time += b.node_collapse_time;
			leaf_collapse_time += b.leaf_collapse_time;
			node_merge_time += b.node_merge_time;
			leaf_merge_time += b.leaf_merge_time;
			ba.silent = true;
		}
		printf("BVHN: Leaf Collapse Time: %.1fms\n", leaf_collapse_time / 16);
		printf("BVHN: Node Collapse Time: %.1fms\n", node_collapse_time / 16);
		printf("BVHN: Node Merge Time: %.1fms\n", node_merge_time / 16);
		printf("BVHN: Leaf Merge Time: %.1fms\n", leaf_merge_time / 16);
		float total_time = leaf_collapse_time + node_collapse_time + node_merge_time + leaf_merge_time;
		printf("BVHN: Total Time: %.1fms\n", total_time / 16);
	}
	printf("\n\n");
#endif
	//6909.8 //5259.9
	const char* bvh_cache = animate ? "" : bvh_cache_path.c_str();
	bool refit_mode = animate && anim_strat == "refit";

	//  keyframe anim + NO --anim-frame  -> the WHOLE sequence in one run (loop 0..anim_frames-1)
	//  --anim-frame=K given (or static) -> just that one frame (keeps benchmark_anim.py working)
	bool render_all = keyframe_anim && argstr("anim-frame", "").empty();
	int  frame_lo = render_all ? 0 : anim_frame;
	int  frame_hi = render_all ? anim_frames : anim_frame + 1;

	//Refit builds the tree topology ONCE from the rest pose (keyframe 0, as just loaded) and only
	//updates box extents each frame; static builds once too. Rebuild rebuilds inside the loop.
	rtm::CWBVH bvh;
	if (refit_mode || !animate)
		bvh = rtm::CWBVH(mesh, bvh_cache, 2, false);

	std::vector<rtm::Ray> rays(args.framebuffer_size);

	//These mesh buffers are stable across frames (a pose overwrites positions in place; only the
	//index arrays get reordered by a rebuild), so wire them once.
	args.vertices = mesh.vertices.data();
	args.normals = mesh.normals.data();
	args.tex_coords = mesh.tex_coords.data();
	args.materials = mesh.materials.data();

	uint thread_count = max(std::thread::hardware_concurrency() - 2u, 1u);

	for (int frame = frame_lo; frame < frame_hi; ++frame)
	{
		if (animate)
		{
			if (keyframe_anim)
				anim_t = (anim_frames > 1) ? (float)frame / (anim_frames - 1) * (anim.count - 1) : 0.0f;
			apply_pose(mesh);                                             //pose for THIS frame (absolute, not cumulative)
			if (refit_mode) bvh.refit(mesh);                             //keep topology, update boxes
			else            bvh = rtm::CWBVH(mesh, bvh_cache, 2, false);  //rebuild from scratch
		}

		args.nodes = bvh.nodes.data();
	#if USE_HECWBVH_V1
		args.ftbs = (rtm::FTB*)args.nodes;
	#else
		args.ft_blocks = bvh.ftbs.data();
	#endif
		//a rebuild reorders these into leaf order (in place), so re-point them every frame
		args.vertex_indices = mesh.vertex_indices.data();
		args.normal_indices = mesh.normal_indices.data();
		args.tex_coord_indices = mesh.tex_coord_indices.data();
		args.material_indices = mesh.material_indices.data();

		if (args.pregen_rays)
		{
			pregen_rays(args.nodes, args.ft_blocks, mesh, args.framebuffer_width, args.framebuffer_height, args.camera, pregen_bounce, rays);
			args.rays = rays.data();
		}

		node_steps = 0; prim_steps = 0; reset_fchthrd();

		printf("\nStarting Traversal (frame %d)\n", frame);
		auto start = std::chrono::high_resolution_clock::now();
		std::vector<std::thread> threads;
		for (uint i = 1; i < thread_count; ++i) threads.emplace_back(kernel, args);
		kernel(args);
		for (uint i = 1; i < thread_count; ++i) threads[i - 1].join();
		auto stop = std::chrono::high_resolution_clock::now();
		auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(stop - start);

		printf("Runtime: %dms\n", (uint)duration.count());
		printf("Node steps: %5.2f\n", (float)node_steps.load() / args.framebuffer_size);
		printf("Prim steps: %5.2f\n", (float)prim_steps.load() / args.framebuffer_size);
		printf("Memory Traffic: %.1f B/ray\n", (float)(node_steps.load() + prim_steps.load()) * sizeof(rtm::CWBVH::Node) / args.framebuffer_size);

		stbi_flip_vertically_on_write(true);
		char out_name[64] = "trax-out.png";
		if (animate) snprintf(out_name, sizeof(out_name), "frame_%03d.png", frame);
		stbi_write_png(out_name, args.framebuffer_width, args.framebuffer_height, 4, args.framebuffer, 0);
		printf("wrote %s\n", out_name);
	}

	return 0;
}
#endif
