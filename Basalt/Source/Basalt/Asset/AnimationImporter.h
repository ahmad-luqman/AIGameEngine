#pragma once

#include "Basalt/Asset/MeshSource.h"

#include <string>

struct cgltf_data;

namespace Basalt {

	// Copies and validates skeletons and clips after cgltf has loaded and validated its buffers.
	bool ImportAnimations(const cgltf_data& data, MeshSource& source, std::string& outError);

}
