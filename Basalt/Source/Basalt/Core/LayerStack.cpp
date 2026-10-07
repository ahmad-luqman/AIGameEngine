#include "Basalt/Core/LayerStack.h"

#include <algorithm>
#include <ranges>

namespace Basalt {

	LayerStack::~LayerStack()
	{
		Clear();
	}

	void LayerStack::PushLayer(Layer* layer)
	{
		m_Layers.emplace(m_Layers.begin() + static_cast<std::ptrdiff_t>(m_LayerInsertIndex), layer);
		m_LayerInsertIndex++;
		layer->OnAttach();
	}

	void LayerStack::PushOverlay(Layer* overlay)
	{
		m_Layers.emplace_back(overlay);
		overlay->OnAttach();
	}

	bool LayerStack::PopLayer(Layer* layer)
	{
		const auto layersEnd = m_Layers.begin() + static_cast<std::ptrdiff_t>(m_LayerInsertIndex);
		auto it = std::find(m_Layers.begin(), layersEnd, layer);
		if (it == layersEnd)
			return false;

		layer->OnDetach();
		m_Layers.erase(it);
		m_LayerInsertIndex--;
		return true;
	}

	bool LayerStack::PopOverlay(Layer* overlay)
	{
		const auto overlaysBegin = m_Layers.begin() + static_cast<std::ptrdiff_t>(m_LayerInsertIndex);
		auto it = std::find(overlaysBegin, m_Layers.end(), overlay);
		if (it == m_Layers.end())
			return false;

		overlay->OnDetach();
		m_Layers.erase(it);
		return true;
	}

	void LayerStack::Clear()
	{
		for (Layer* layer : std::views::reverse(m_Layers))
		{
			layer->OnDetach();
			delete layer;
		}
		m_Layers.clear();
		m_LayerInsertIndex = 0;
	}

}
