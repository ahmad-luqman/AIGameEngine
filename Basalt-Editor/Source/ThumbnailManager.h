#pragma once

#include "Basalt/Asset/MeshSource.h"
#include "Basalt/Asset/Thumbnails.h"
#include "Basalt/Core/Base.h"
#include "Basalt/Core/FileWatcher.h"
#include "Basalt/Renderer/PreviewRenderer.h"

#include <nvrhi/nvrhi.h>

#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace Basalt {

	class Project;

	// Content-browser and inspector previews. Panels ask for a preview every frame they show it; missing or
	// outdated ones are queued and made by Update (the editor calls it once per frame, before the ImGui pass)
	// within a frame budget: at most one GPU render, and new CPU work only until about 8 ms have passed (one large
	// image can take longer). File previews are cached on disk (ThumbnailCache); material previews only in GPU
	// textures. Previews not shown recently are dropped beyond MaxTextures / MaxSlots. Assets a preview loads only for
	// itself are released from AssetManager when it is made (AssetManager::TemporaryLoads).
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
			struct Material
			{
				std::string Name;
				// The whole imported material, previewed losslessly (as GetMaterial's MaterialData overload draws it).
				MaterialData Values;
			};
			// A model's materials, read when its preview was made this session (the model is not kept loaded for it);
			// cleared when the file changes and the new version's are not known.
			std::vector<Material> Materials;
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
		// without one, or without an open project. The pointer is valid until the next Update().
		const Preview* Get(const std::string& relativePath);
		// A material on a sphere. `id` names the preview slot (an entity, a model's material); it is made again once
		// the material's values have stopped changing for a frame (not on every frame of a drag). The pointer is
		// valid until the next Update().
		const Preview* GetMaterial(const std::string& id, const MaterialComponent& material);
		// A material by value: the material editor's unsaved draft, or a model's imported material. Made again like the
		// overload above once its values settle.
		const Preview* GetMaterial(const std::string& id, const MaterialData& material);
		// Makes queued previews and applies a requested clear. Renders and uploads on its own command lists, so
		// call it while no other immediate command list is open, and before the frame's ImGui pass (which then
		// draws the textures it leaves).
		void Update();

		std::optional<Generated> Generate(const std::string& relativePath, bool refresh, std::string& outError);
		std::optional<ThumbnailImage> RenderMaterial(const MaterialComponent& material, std::string& outError);
		std::optional<ThumbnailImage> RenderMaterial(const MaterialData& material, std::string& outError);

		// Files changed on disk (after AssetManager dropped them): every file preview checks its version on its next
		// request instead of up to RecheckSeconds (2 s) later, and material previews that read one of them (a map, a
		// material file or its textures, a model) are made again.
		void OnFilesChanged(const std::vector<FileChange>& changes);

		// A model's imported materials as previews list them ("Material N" for unnamed ones), without the default
		// material the importer adds for primitives that have none.
		static std::vector<Preview::Material> ListMaterials(const MeshSource& mesh);

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
			// Set for a by-value preview (a draft, an imported material), which renders this instead of Material.
			std::optional<MaterialData> Draft;
			bool IsMaterial = false;
			bool Queued = false;
			uint64_t LastUsed = 0;
			// When the material values last changed (previews wait until they settle).
			uint64_t ChangedFrame = 0;
			double LastChecked = -1.0;
			// The file could not be read at the last check: when it is back, even unchanged, it is previewed again.
			bool SourceMissing = false;
		};

		enum class ClearRequest : uint8_t
		{
			None,
			Previews,
			PreviewsAndCache
		};

		Slot& Touch(const std::string& id);
		// Queues a material slot when its key (the values it shows) changed.
		const Preview* UpdateMaterial(const std::string& id, std::string key, const std::function<void(Slot&)>& assign);
		void Enqueue(const std::string& id, Slot& slot);
		static void Fail(Slot& slot, std::string error);
		bool EnsureRenderer(std::string& outError);
		std::optional<ThumbnailCache> GetCache() const;
		// Makes one queued preview; true for a GPU-rendered kind, which ends the frame's budget even when the render
		// failed.
		bool Process(const std::string& id, Slot& slot);
		// With `reread`, the model or prefab and the files it uses are read again first (their file changed).
		// outMaterials (optional) receives a model's materials.
		std::optional<ThumbnailCache::Entry> Make(const ThumbnailCache& cache, const std::string& relativePath, ThumbnailKind kind, bool reread, std::vector<Preview::Material>* outMaterials, std::string& outError);
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
		// The project the previews belong to (weak, so a new project at the same address still counts as another);
		// another project starts them afresh.
		std::weak_ptr<Project> m_Project;
		ClearRequest m_ClearRequest = ClearRequest::None;
	};

}
