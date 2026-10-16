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

	// Content-browser and inspector previews. Panels ask for a preview every frame they show it; missing or
	// outdated ones are queued and made in OnRender within a frame budget (at most one GPU render, CPU work
	// for up to a few milliseconds), cached on disk (ThumbnailCache) and in GPU textures (least recently used
	// ones are dropped beyond MaxTextures).
	class ThumbnailManager
	{
	public:
		static constexpr size_t MaxTextures = 512;

		enum class State
		{
			Pending,
			Ready,
			Failed
		};

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
		};

		explicit ThumbnailManager(nvrhi::IDevice* device);
		~ThumbnailManager();
		ThumbnailManager(const ThumbnailManager&) = delete;
		ThumbnailManager& operator=(const ThumbnailManager&) = delete;

		// The preview of a project-relative asset, queued when missing or outdated; nullptr for files without one.
		const Preview* Get(const std::string& relativePath);
		// A material on a sphere. `id` names the preview slot (an entity, a model's material); it is re-rendered
		// when the material's values change.
		const Preview* GetMaterial(const std::string& id, const MaterialComponent& material);
		// Makes queued previews. Renders and uploads on its own command lists, so call it while no other immediate
		// command list is open.
		void Update();

		std::optional<Generated> Generate(const std::string& relativePath, bool refresh, std::string& outError);
		std::optional<ThumbnailImage> RenderMaterial(const MaterialComponent& material, std::string& outError);

		// Forgets every preview (a project switch or an explicit clear); with `deleteCache`, the disk cache too.
		// Returns the number of disk entries removed.
		size_t Clear(bool deleteCache);

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
			double LastChecked = -1.0;
		};

		Slot* Touch(const std::string& id);
		bool EnsureRenderer(std::string& outError);
		std::optional<ThumbnailCache> GetCache() const;
		// Makes one queued preview; true when it rendered on the GPU (which takes the frame's budget).
		bool Process(const std::string& id, Slot& slot);
		std::optional<ThumbnailCache::Entry> Make(const std::string& relativePath, ThumbnailKind kind, std::string& outError);
		void Upload(Slot& slot, const ThumbnailImage& image);
		void EvictLeastRecentlyUsed();

		nvrhi::IDevice* m_Device = nullptr;
		Scope<PreviewRenderer> m_Renderer;
		nvrhi::CommandListHandle m_UploadList;
		bool m_RendererFailed = false;
		std::unordered_map<std::string, Slot> m_Slots;
		std::deque<std::string> m_Queue;
		uint64_t m_Frame = 0;
	};

}
