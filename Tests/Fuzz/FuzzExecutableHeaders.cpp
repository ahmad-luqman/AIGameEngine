// Executable headers the exporter reads (Project/Packaging): the runtime's Authenticode signature, which Windows exports
// remove, and the Mach-O architectures of the runtime and the Vulkan libraries a macOS export bundles.
#include "FuzzCommon.h"

#include <Basalt/Project/Packaging.h>

#include <string>
#include <vector>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
	BasaltFuzz::Init();
	std::vector<uint8_t> image(data, data + size);
	std::string error;
	const std::vector<uint8_t> original = image;
	const auto removal = Basalt::RemoveAuthenticodeSignature(image, error);
	// Anything but a removal leaves the bytes alone; a removal only shortens them, leaving an unsigned executable.
	if (!removal || *removal != Basalt::SignatureRemoval::Removed)
	{
		if (image != original)
			__builtin_trap();
	}
	else if (image.size() >= original.size() || Basalt::RemoveAuthenticodeSignature(image, error) != Basalt::SignatureRemoval::Unsigned)
	{
		__builtin_trap();
	}
	(void)Basalt::MachOArchitectures(original);
	return 0;
}
