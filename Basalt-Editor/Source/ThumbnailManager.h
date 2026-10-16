#pragma once

#include "Basalt/Asset/Thumbnails.h"
#include "Basalt/Core/Base.h"
#include "Basalt/Renderer/PreviewRenderer.h"

#include <nvrhi/nvrhi.h>

#include <deque>
#include <filesystem>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace Basalt {

	class Project;

	// Content-browser and inspector previews. Panels ask for a preview every frame they show it; missing or
	// outdated ones are queued and made by Update (the editor calls it once per frame, before the ImGui pass)
	// within a frame budget: at most one GPU render, CPU work for up to a few milliseconds. File previews are
	// cached on disk (ThumbnailCache); material previews only in GPU textures. Previews not shown recently are
	// dropped beyond MaxTextures / MaxSlots.
	class ThumbnailManager
	{
	public:
		static constexpr size_t MaxTextures = 512;
		// Every remembered preview, including failed and pending ones without a texture.
		static constexpr size_t MaxSlots = MaxTextures * 4;

		enum class State : uint8_t
		{
			Pending,
			Ready,
			Failed
		};

		// One preview as panels see it. Texture may still hold the previous image while a newer one is Pending, and
		// after a later attempt Failed.
		struct Preview
		{
			ThumbnailKind Kind = ThumbnailKind::None;
			State Status = State::Pending;
			nvrhi::TextureHandle Texture;
			uint32_t Width = 0;
			uint32_t Height = 0;
			std::string Error;
			std::vector<std::string> Details;
		};

		// The result of a synchronous request (editor.thumbnail).
		struct Generated
		{
			ThumbnailKind Kind = ThumbnailKind::None;
			ThumbnailImage Image;
			std::vector<std::string> Details;
			bool Cached = false;
			std::filesystem::path CachePath;
			// Set when the preview was made but could not be stored in the disk cache.
			std::string CacheWarning;
		};

		explicit ThumbnailManager(nvrhi::IDevice* device);
		~ThumbnailManager();
		ThumbnailManager(const ThumbnailManager&) = delete;
		ThumbnailManager& operator=(const ThumbnailManager&) = delete;

		// The preview of a project-relative asset ("Assets/..."), queued when missing or outdated; nullptr for files
		// without one. The pointer is valid until the next Update().
		const Preview* Get(const std::string& relativePath);
		// A material on a sphere. `id` names the preview slot (an entity, a model's material); it is made again once
		// the material's values have stopped changing for a frame (not on every frame of a drag). The pointer is
		// valid until the next Update().
		const Preview* GetMaterial(const std::string& id, const MaterialComponent& material);
		// Makes queued previews and applies a requested clear. Renders and uploads on its own command lists, so
		// call it while no other immediate command list is open, and before the frame's ImGui pass (which then
		// draws the textures it leaves).
		void Update();

		std::optional<Generated> Generate(const std::string& relativePath, bool refresh, std::string& outError);
		std::optional<ThumbnailImage> RenderMaterial(const MaterialComponent& material, std::string& outError);

		// Forgets every preview at the next Update (textures drawn this frame stay alive until then); with
		// `deleteCache`, the disk cache too (the count it removed is logged).
		void RequestClear(bool deleteCache);

	private:
		struct Slot
		{
			Preview Data;
			// The source version (ThumbnailCache key) or material values the preview shows or is queued for.
			std::string Key;
			MaterialComponent Material;
			bool IsMaterial = false;
			bool Queued = false;
			uint64_t LastUsed = 0;
			// When the material values last changed (previews wait until they settle).
			uint64_t ChangedFrame = 0;
			double LastChecked = -1.0;
		};

		Slot& Touch(const std::string& id);
		void Enqueue(const std::string& id, Slot& slot);
		static void Fail(Slot& slot, std::string error);
		bool EnsureRenderer(std::string& outError);
		std::optional<ThumbnailCache> GetCache() const;
		// Makes one queued preview; true when it rendered on the GPU (which ends the frame's budget).
		bool Process(const std::string& id, Slot& slot);
		std::optional<ThumbnailCache::Entry> Make(const std::string& relativePath, ThumbnailKind kind, std::string& outError);
		void Upload(Slot& slot, const ThumbnailImage& image);
		void Clear();
		void EvictLeastRecentlyUsed();

		nvrhi::IDevice* m_Device = nullptr;
		Scope<PreviewRenderer> m_Renderer;
		nvrhi::CommandListHandle m_UploadList;
		bool m_RendererFailed = false;
		std::unordered_map<std::string, Slot> m_Slots;
		std::deque<std::string> m_Queue;
		// Incremented at the end of Update, so previews requested during this frame's ImGui pass carry the number
		// the next Update compares against.
		uint64_t m_Frame = 1;
		// The project the previews belong to; another project starts them afresh.
		const Project* m_Project = nullptr;
		std::optional<bool> m_ClearRequest;
	};

}
