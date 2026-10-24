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
	// A forged size may legitimately ask for up to MaxEntrySize bytes; reading such entries tests nothing more and would
	// exceed libFuzzer's memory limit, so the fuzzer reads only small ones.
	constexpr uint64_t MaxFuzzEntrySize = uint64_t{ 16 } << 20;
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
