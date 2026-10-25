// .bpak archives (Core/PackArchive): index parsing and entry reads, as the runtime mounts an exported game's Assets.bpak.
#include "FuzzCommon.h"

#include <Basalt/Core/PackArchive.h>

#include <vector>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
	BasaltFuzz::Init();
	std::string error;
	const Basalt::Ref<Basalt::PackArchive> archive = Basalt::PackArchive::OpenMemory(std::vector<uint8_t>(data, data + size), error);
	if (!archive)
		return 0;
	// Parse bounds a compressed entry's size by its stored bytes (128 KiB per 3 bytes), so a fuzz input of a few KiB can
	// still claim hundreds of megabytes, all legitimately allocated; reading those tests nothing more and only slows the
	// run, so the fuzzer skips entries past 256 MiB (well within the 3-4 GiB RSS limit scripts/fuzz.sh sets).
	constexpr uint64_t MaxFuzzEntrySize = uint64_t{ 256 } << 20;
	for (const Basalt::PackArchive::Entry& entry : archive->GetEntries())
	{
		(void)archive->Find(entry.Path);
		(void)archive->FindAsset(entry.AssetID);
		(void)archive->HasFolder(entry.Path);
		if (entry.Size <= MaxFuzzEntrySize)
			(void)archive->Read(entry, error);
	}
	return 0;
}
