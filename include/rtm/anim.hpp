#pragma once

#include "mesh.hpp"

#ifndef __riscv

#include<string>
#include<fstream>
#include<cmath>
#include<cstdio>

namespace rtm {

	struct KeyFrameAnim
	{
		std::string folder, prefix;
		int count{ 0 };

		std::string path(int i) const
		{
			char b3[16], b2[16];
			snprintf(b3, sizeof(b3), "%03d", i);
			snprintf(b2, sizeof(b2), "%02d", i);
			std::string p3 = folder + prefix + b3 + ".obj";
			if (std::ifstream(p3).good()) return p3;
			return folder + prefix + b2 + ".obj";
		}

		bool init(const std::string& folder_, const std::string& prefix_)
		{
			folder = folder_; prefix = prefix_; count = 0;
			while (std::ifstream(path(count)).good()) count++;
			printf("KeyframeAnim: %d keyframes at %s%s\n", count, folder.c_str(), prefix.c_str());
			return count > 0;
		}

		void sample(float t, rtm::Mesh& mesh) const
		{
			if (count == 0) return;
			t = rtm::max(0.0f, rtm::min((float)(count - 1), t));
			int k0 = (int)t, k1 = rtm::min(k0 + 1, count - 1);
			float a = t - k0;
			rtm::Mesh m0(path(k0));
			if (k1 == k0)
			{
				for (size_t v = 0; v < mesh.vertices.size() && v < m0.vertices.size(); ++v) mesh.vertices[v] = m0.vertices[v];
				for (size_t v = 0; v < mesh.normals.size() && v < m0.normals.size(); ++v) mesh.normals[v] = m0.normals[v];
				return;
			}
			rtm::Mesh m1(path(k1));
			for (size_t v = 0; v < mesh.vertices.size() && v < m0.vertices.size() && v < m1.vertices.size(); ++v)
				mesh.vertices[v] = m0.vertices[v] * (1.0f - a) + m1.vertices[v] * a;
			for (size_t v = 0; v < mesh.normals.size() && v < m0.normals.size() && v < m1.normals.size(); ++v)
				mesh.normals[v] = rtm::normalize(m0.normals[v] * (1.0f - a) + m1.normals[v] * a);
		}

	};

	inline void deform_mesh(rtm::Mesh& mesh, const std::string& mode, float mag)
	{
		if (mag == 0.0f || mesh.vertices.empty()) return;
		rtm::AABB box;
		for (auto& v : mesh.vertices) box.add(v);
		rtm::vec3 c = box.centroid();
		rtm::vec3 ext = box.max - box.min;
		float ey = rtm::max(ext.y, 1e-6f);
		for (auto& v : mesh.vertices)
		{
			if (mode == "explode") { v = v + (v - c) * (mag * 0.5f); }
			else if (mode == "sine") { float t = (v.y - box.min.y) / ey; v.x += mag * ext.x * 0.25f * sinf(t * 6.2831853f * 2.0f); }
			else //"twist" about the vertical (Y) axis; angle grows with height
			{
				float t = (v.y - c.y) / ey;
				float ang = mag * 1.5707963f * t;
				float s = sinf(ang), co = cosf(ang);
				float x = v.x - c.x, z = v.z - c.z;
				v.x = c.x + x * co - z * s;
				v.z = c.z + x * s + z * co;
			}
		}
	}
}

#endif // !__resicv


