#include "ThumbnailManager.h"

#include "Basalt/Asset/AssetManager.h"
#include "Basalt/Asset/MeshSource.h"
#include "Basalt/Asset/TextureSource.h"
#include "Basalt/Core/FileSystem.h"
#include "Basalt/Core/JsonUtils.h"
#include "Basalt/Core/Log.h"
#include "Basalt/Project/Project.h"

#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <chrono>
#include <unordered_set>

namespace Basalt {

	namespace {

		// How often a shown preview checks whether its file changed.
		constexpr double RecheckSeconds = 2.0;
		// CPU work per frame (image decoding, cached previews); a GPU render always ends the frame's work.
		constexpr std::chrono::milliseconds FrameBudget{ 8 };
		// Content-browser paths start with "Assets/", so material slots (and their prefix) cannot collide with them.
		const std::string s_MaterialPrefix = "material:";

		double Now()
		{
			return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
		}

		// Every value a material preview shows, exactly (fmt prints floats so they read back the same). A string
		// rather than operator==: NaN never compares equal, which would re-render it every frame.
		std::string MaterialKey(const MaterialComponent& m)
		{
			return fmt::format("{} {} {} {} {} {} {} {} {} {}|{}|{}|{}|{}|{}", m.AlbedoColor.r, m.AlbedoColor.g, m.AlbedoColor.b, m.AlbedoColor.a, m.Metallic, m.Roughness, m.EmissiveColor.r, m.EmissiveColor.g, m.EmissiveColor.b, m.EmissiveIntensity, m.AlbedoMap, m.NormalMap, m.MetallicRoughnessMap, m.OcclusionMap, m.EmissiveMap);
		}

		std::vector<std::string> ModelDetails(const MeshSource& mesh)
		{
			const size_t triangles = mesh.Indices.size() / 3;
			std::vector<std::string> details = { fmt::format("{} mesh(es), {} material(s)", mesh.Meshes.size(), mesh.Materials.size()), fmt::format("{} vertices, {} triangles", mesh.Vertices.size(), triangles) };
			if (!mesh.Skins.empty() || !mesh.Animations.empty())
				details.push_back(fmt::format("{} skin(s), {} animation(s)", mesh.Skins.size(), mesh.Animations.size()));
			return details;
		}

	}

	ThumbnailManager::ThumbnailManager(nvrhi::IDevice* device)
		: m_Device(device)
	{
	}

	ThumbnailManager::~ThumbnailManager() = default;

	std::optional<ThumbnailCache> ThumbnailManager::GetCache() const
	{
		const Ref<Project>& project = Project::GetActive();
		if (!project)
			return std::nullopt;
		return ThumbnailCache(project->GetDirectory());
	}

	ThumbnailManager::Slot& ThumbnailManager::Touch(const std::string& id)
	{
		Slot& slot = m_Slots[id];
		slot.LastUsed = m_Frame;
		return slot;
	}

	void ThumbnailManager::Enqueue(const std::string& id, Slot& slot)
	{
		if (slot.Queued)
			return;
		slot.Queued = true;
		m_Queue.push_back(id);
	}

	std::vector<ThumbnailManager::Preview::Material> ThumbnailManager::ListMaterials(const MeshSource& mesh)
	{
		std::vector<Preview::Material> materials;
		materials.reserve(mesh.Materials.size());
		for (size_t i = 0; i < mesh.Materials.size(); i++)
			materials.push_back({ mesh.Materials[i].Name.empty() ? "Material " + std::to_string(i) : mesh.Materials[i].Name, ToMaterialComponent(mesh.Materials[i]) });
		return materials;
	}

	void ThumbnailManager::Fail(Slot& slot, std::string error)
	{
		// What the old version's model held no longer describes the file.
		slot.Data.Materials.clear();
		slot.Data.Status = State::Failed;
		slot.Data.Error = std::move(error);
	}

	const ThumbnailManager::Preview* ThumbnailManager::Get(const std::string& relativePath)
	{
		BS_CORE_ASSERT(relativePath.starts_with("Assets/"), "thumbnail paths are project-relative asset paths");
		const ThumbnailKind kind = ClassifyThumbnail(relativePath);
		const auto cache = GetCache();
		if (kind == ThumbnailKind::None || !cache)
			return nullptr;
		Slot& slot = Touch(relativePath);
		slot.Data.Kind = kind;
		if (const double now = Now(); slot.LastChecked < 0.0 || now - slot.LastChecked >= RecheckSeconds)
		{
			slot.LastChecked = now;
			std::string error;
			const auto key = cache->MakeKey(relativePath, error);
			if (!key)
			{
				slot.SourceMissing = true;
				Fail(slot, error);
			}
			else if (*key != slot.Key || slot.SourceMissing)
			{
				// The old image stays on screen until the new one is ready.
				slot.SourceMissing = false;
				slot.Data.Status = State::Pending;
				Enqueue(relativePath, slot);
			}
		}
		return &slot.Data;
	}

	const ThumbnailManager::Preview* ThumbnailManager::GetMaterial(const std::string& id, const MaterialComponent& material)
	{
		const std::string slotId = s_MaterialPrefix + id;
		Slot& slot = Touch(slotId);
		slot.IsMaterial = true;
		if (std::string key = MaterialKey(material); key != slot.Key)
		{
			slot.Key = std::move(key);
			slot.Material = material;
			slot.ChangedFrame = m_Frame;
			slot.Data.Status = State::Pending;
			Enqueue(slotId, slot);
		}
		return &slot.Data;
	}

	void ThumbnailManager::OnFilesChanged(const std::vector<FileChange>& changes)
	{
		std::unordered_set<std::string> changed;
		for (const FileChange& change : changes)
			changed.insert(change.Path);
		// An embedded image key ("Model.glb#image/0") changes with its model file.
		const auto uses = [&](const std::string& map) { return !map.empty() && changed.contains(AssetManager::GetSourceFile(map)); };
		for (auto& [id, slot] : m_Slots)
		{
			if (!slot.IsMaterial)
			{
				slot.LastChecked = -1.0;
				continue;
			}
			const MaterialComponent& material = slot.Material;
			// A cleared key no longer matches the material's, so its next GetMaterial queues it.
			if (uses(material.AlbedoMap) || uses(material.NormalMap) || uses(material.MetallicRoughnessMap) || uses(material.OcclusionMap) || uses(material.EmissiveMap))
				slot.Key.clear();
		}
	}

	void ThumbnailManager::RequestClear(bool deleteCache)
	{
		if (deleteCache || m_ClearRequest == ClearRequest::None)
			m_ClearRequest = deleteCache ? ClearRequest::PreviewsAndCache : ClearRequest::Previews;
	}

	void ThumbnailManager::Update()
	{
		// Clears are applied here rather than when requested: the ImGui pass that requested one may already have drawn
		// textures it frees, and those must survive until that frame renders.
		if (Project::GetActive() != m_Project.lock())
		{
			Clear();
			// A clear requested for the previous project must not delete the new one's cache.
			m_ClearRequest = ClearRequest::None;
			m_Project = Project::GetActive();
		}
		if (m_ClearRequest != ClearRequest::None)
		{
			const bool deleteCache = m_ClearRequest == ClearRequest::PreviewsAndCache;
			m_ClearRequest = ClearRequest::None;
			Clear();
			if (const auto cache = GetCache(); deleteCache && cache)
				BS_CORE_INFO("Thumbnails: removed {} cached file(s); previews are made again", cache->Clear());
		}

		const auto start = std::chrono::steady_clock::now();
		std::vector<std::string> waiting;
		while (!m_Queue.empty() && std::chrono::steady_clock::now() - start < FrameBudget)
		{
			const std::string id = m_Queue.front();
			m_Queue.pop_front();
			const auto it = m_Slots.find(id);
			// Evicted or cleared while waiting.
			if (it == m_Slots.end() || !it->second.Queued)
				continue;
			// A material still being edited (it changed during the last ImGui pass) waits for its values to settle.
			if (it->second.IsMaterial && it->second.ChangedFrame >= m_Frame)
			{
				waiting.push_back(id);
				continue;
			}
			it->second.Queued = false;
			if (Process(id, it->second))
				break;
		}
		m_Queue.insert(m_Queue.end(), waiting.begin(), waiting.end());
		EvictLeastRecentlyUsed();
		m_Frame++;
	}

	bool ThumbnailManager::Process(const std::string& id, Slot& slot)
	{
		std::string error;
		if (slot.IsMaterial)
		{
			if (const auto image = RenderMaterial(slot.Material, error))
				Upload(slot, *image);
			else
				Fail(slot, error);
			return true;
		}
		const auto cache = GetCache();
		if (!cache)
			return false;
		// Re-read the key just before reading the file: a write during the preview then leaves a key that no longer
		// matches, and the preview is made again.
		const auto key = cache->MakeKey(id, error);
		if (!key)
		{
			Fail(slot, error);
			return false;
		}
		const bool sourceChanged = !slot.Key.empty() && slot.Key != *key;
		slot.Key = *key;
		if (const auto entry = cache->Load(id, slot.Key))
		{
			// Cached on disk without its materials: those of the version shown before may no longer apply.
			if (sourceChanged)
				slot.Data.Materials.clear();
			slot.Data.Details = entry->Details;
			Upload(slot, entry->Image);
			return false;
		}
		std::vector<Preview::Material> materials;
		const auto entry = Make(*cache, id, slot.Data.Kind, sourceChanged, &materials, error);
		if (!entry)
		{
			BS_CORE_WARN("Thumbnails: no preview of '{}': {}", id, error);
			Fail(slot, error);
			return IsRenderedThumbnail(slot.Data.Kind);
		}
		// The disk cache only saves time later; a preview that cannot be stored is still shown.
		if (!cache->Store(id, slot.Key, *entry, error))
			BS_CORE_WARN("Thumbnails: cannot cache the preview of '{}': {}", id, error);
		slot.Data.Details = entry->Details;
		slot.Data.Materials = std::move(materials);
		Upload(slot, entry->Image);
		return IsRenderedThumbnail(slot.Data.Kind);
	}

	bool ThumbnailManager::EnsureRenderer(std::string& outError)
	{
		if (m_Renderer)
			return true;
		if (m_RendererFailed || !m_Device)
		{
			outError = "the preview renderer is not available";
			return false;
		}
		auto renderer = CreateScope<PreviewRenderer>(m_Device);
		if (!renderer->Initialize())
		{
			// Logged once: every later preview reports the same error instead of retrying.
			BS_CORE_ERROR("Thumbnails: the preview renderer failed to initialize (see the renderer error above)");
			m_RendererFailed = true;
			outError = "the preview renderer failed to initialize";
			return false;
		}
		m_Renderer = std::move(renderer);
		return true;
	}

	std::optional<ThumbnailCache::Entry> ThumbnailManager::Make(const ThumbnailCache& cache, const std::string& relativePath, ThumbnailKind kind, bool reread, std::vector<Preview::Material>* outMaterials, std::string& outError)
	{
		// What only this preview loads is released once it is made; what was loaded before (the open scene's) stays,
		// including when it is read again below.
		AssetManager::TemporaryLoads previewOnly;
		// AssetManager keeps what it loaded earlier: read the changed files again. (The open scene then shows the new
		// version too, which is what a changed file means there.)
		if (reread && IsRenderedThumbnail(kind))
		{
			AssetManager::Reload(relativePath);
			for (const std::string& dependency : cache.GetDependencies(relativePath))
				AssetManager::Reload(dependency);
		}
		switch (kind)
		{
			case ThumbnailKind::Image:
			case ThumbnailKind::Environment:
			{
				// Loaded directly, not through AssetManager: browsing must not keep every image (or a 100 MB HDRI) in memory.
				const Ref<TextureSource> source = TextureSource::LoadFromFile(Project::ResolvePath(FileSystem::PathFromUtf8(relativePath)), outError);
				if (!source)
					return std::nullopt;
				ThumbnailImage image = MakeImageThumbnail(*source, PreviewRenderer::OutputSize);
				if (!image.IsValid())
				{
					outError = "the image data is incomplete";
					return std::nullopt;
				}
				return ThumbnailCache::Entry{ std::move(image), { fmt::format("{} x {}{}", source->Width, source->Height, source->Format == TextureFormat::RGBA32F ? " HDR" : "") } };
			}
			case ThumbnailKind::Model:
			{
				if (!EnsureRenderer(outError))
					return std::nullopt;
				auto image = m_Renderer->RenderModel(relativePath, outError);
				if (!image)
					return std::nullopt;
				// RenderModel just loaded it; should AssetManager have dropped it since, only the details line is lost.
				const Ref<MeshSource> mesh = AssetManager::FindLoadedMesh(relativePath);
				if (!mesh)
					BS_CORE_WARN("Thumbnails: '{}' rendered but is no longer loaded; its preview has no details", relativePath);
				// Kept with the preview: the model itself is released when the preview is made.
				if (mesh && outMaterials)
					*outMaterials = ListMaterials(*mesh);
				return ThumbnailCache::Entry{ std::move(*image), mesh ? ModelDetails(*mesh) : std::vector<std::string>{} };
			}
			case ThumbnailKind::Prefab:
			{
				if (!EnsureRenderer(outError))
					return std::nullopt;
				auto image = m_Renderer->RenderPrefab(relativePath, outError);
				if (!image)
					return std::nullopt;
				const auto text = FileSystem::ReadTextFile(Project::ResolvePath(FileSystem::PathFromUtf8(relativePath)));
				const nlohmann::json data = text ? ParseJson(*text) : nlohmann::json();
				const auto entities = data.is_object() ? data.find("Entities") : data.end();
				std::vector<std::string> details;
				if (entities != data.end() && entities->is_array())
					details.push_back(fmt::format("{} entit{}", entities->size(), entities->size() == 1 ? "y" : "ies"));
				else
					BS_CORE_WARN("Thumbnails: '{}' rendered but could not be read again for its details (changed meanwhile?)", relativePath);
				return ThumbnailCache::Entry{ std::move(*image), std::move(details) };
			}
			case ThumbnailKind::None:
				break;
		}
		outError = "no preview for this file type";
		return std::nullopt;
	}

	std::optional<ThumbnailImage> ThumbnailManager::RenderMaterial(const MaterialComponent& material, std::string& outError)
	{
		if (!EnsureRenderer(outError))
			return std::nullopt;
		AssetManager::TemporaryLoads previewOnly;
		return m_Renderer->RenderMaterial(material, outError);
	}

	std::optional<ThumbnailManager::Generated> ThumbnailManager::Generate(const std::string& relativePath, bool refresh, std::string& outError)
	{
		const ThumbnailKind kind = ClassifyThumbnail(relativePath);
		const auto cache = GetCache();
		if (!cache)
		{
			outError = "no project is open";
			return std::nullopt;
		}
		if (kind == ThumbnailKind::None)
		{
			outError = "'" + relativePath + "' has no preview (images, .hdr environments, models and prefabs do)";
			return std::nullopt;
		}
		// Previews are of project assets: never read (or cache) a file elsewhere on disk.
		const std::filesystem::path normal = FileSystem::PathFromUtf8(relativePath).lexically_normal();
		if (!relativePath.starts_with("Assets/") || normal.empty() || *normal.begin() != "Assets")
		{
			outError = "'" + relativePath + "' is not inside Assets/ (paths are project-relative, like Assets/Models/Ship.glb)";
			return std::nullopt;
		}
		std::string keyError;
		const auto key = cache->MakeKey(relativePath, keyError);
		if (!key)
		{
			outError = "no preview of '" + relativePath + "': " + keyError;
			return std::nullopt;
		}
		Generated result;
		result.Kind = kind;
		result.CachePath = cache->GetImagePath(relativePath);
		// A refresh makes it again, reading the model (and what it uses) anew, whatever AssetManager holds.
		if (auto entry = refresh ? std::nullopt : cache->Load(relativePath, *key))
		{
			result.Image = std::move(entry->Image);
			result.Details = std::move(entry->Details);
			result.Cached = true;
			return result;
		}
		auto entry = Make(*cache, relativePath, kind, refresh, nullptr, outError);
		if (!entry)
			return std::nullopt;
		if (!cache->Store(relativePath, *key, *entry, result.CacheWarning))
			BS_CORE_WARN("Thumbnails: cannot cache the preview of '{}': {}", relativePath, result.CacheWarning);
		// The browser picks up the new image from the cache.
		if (const auto it = m_Slots.find(relativePath); it != m_Slots.end())
		{
			it->second.Key.clear();
			it->second.LastChecked = -1.0;
		}
		result.Image = std::move(entry->Image);
		result.Details = std::move(entry->Details);
		return result;
	}

	void ThumbnailManager::Upload(Slot& slot, const ThumbnailImage& image)
	{
		if (!image.IsValid())
		{
			Fail(slot, "the preview is empty");
			return;
		}
		nvrhi::TextureDesc desc;
		desc.width = image.Width;
		desc.height = image.Height;
		desc.format = nvrhi::Format::RGBA8_UNORM;
		desc.initialState = nvrhi::ResourceStates::ShaderResource;
		desc.keepInitialState = true;
		desc.debugName = "Thumbnail";
		nvrhi::TextureHandle texture = m_Device->createTexture(desc);
		if (!m_UploadList)
			m_UploadList = m_Device->createCommandList();
		if (!texture || !m_UploadList)
		{
			BS_CORE_WARN("Thumbnails: cannot create a {}x{} preview texture", image.Width, image.Height);
			Fail(slot, "cannot create the preview texture");
			return;
		}
		m_UploadList->open();
		m_UploadList->writeTexture(texture, 0, 0, image.Pixels.data(), static_cast<size_t>(image.Width) * 4);
		m_UploadList->close();
		m_Device->executeCommandList(m_UploadList);
		// Update runs before this frame's ImGui pass, so no draw list holds the old texture's raw pointer yet; nvrhi
		// keeps it alive for last frame's draws still in flight.
		slot.Data.Texture = texture;
		slot.Data.Width = image.Width;
		slot.Data.Height = image.Height;
		slot.Data.Status = State::Ready;
		slot.Data.Error.clear();
	}

	void ThumbnailManager::EvictLeastRecentlyUsed()
	{
		size_t textures = 0;
		for (const auto& [id, slot] : m_Slots)
			textures += slot.Data.Texture ? 1 : 0;
		while (textures > MaxTextures || m_Slots.size() > MaxSlots)
		{
			// Over the texture budget only slots with textures count; over the slot budget any slot does.
			const bool texturesOnly = textures > MaxTextures;
			auto oldest = m_Slots.end();
			for (auto it = m_Slots.begin(); it != m_Slots.end(); ++it)
				if ((!texturesOnly || it->second.Data.Texture) && (oldest == m_Slots.end() || it->second.LastUsed < oldest->second.LastUsed))
					oldest = it;
			// Never drop a preview requested during the last ImGui pass (it is still on screen).
			if (oldest == m_Slots.end() || oldest->second.LastUsed >= m_Frame)
				break;
			textures -= oldest->second.Data.Texture ? 1 : 0;
			m_Slots.erase(oldest);
		}
	}

	void ThumbnailManager::Clear()
	{
		m_Slots.clear();
		m_Queue.clear();
	}

}
