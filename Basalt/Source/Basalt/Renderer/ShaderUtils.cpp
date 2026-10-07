#include "Basalt/Renderer/ShaderUtils.h"

#include "Basalt/Core/Log.h"
#include "Basalt/Renderer/EmbeddedShaders.h"

#include <string>

namespace Basalt {

	nvrhi::ShaderHandle CreateEmbeddedShader(nvrhi::IDevice* device, std::string_view name, nvrhi::ShaderType type)
	{
		const std::span<const uint32_t> code = EmbeddedShaders::Get(name);
		if (code.empty())
		{
			BS_CORE_ERROR("Embedded shader '{}' not found", name);
			return nullptr;
		}

		const std::string debugName(name);
		nvrhi::ShaderDesc desc;
		desc.shaderType = type;
		desc.debugName = debugName;
		desc.entryName = "main";
		return device->createShader(desc, code.data(), code.size_bytes());
	}

}
