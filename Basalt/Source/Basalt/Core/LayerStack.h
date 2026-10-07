#pragma once

#include "Basalt/Core/Layer.h"

#include <vector>

namespace Basalt {

	// Owns layers. Regular layers sit below overlays; both keep insertion order.
	class LayerStack
	{
	public:
		LayerStack() = default;
		~LayerStack();

		LayerStack(const LayerStack&) = delete;
		LayerStack& operator=(const LayerStack&) = delete;

		void PushLayer(Layer* layer);
		void PushOverlay(Layer* overlay);
		// Detaches and returns ownership to the caller. Returns false if the layer is not in the stack.
		bool PopLayer(Layer* layer);
		bool PopOverlay(Layer* overlay);
		// Detaches and deletes every layer, overlays first.
		void Clear();

		size_t GetSize() const { return m_Layers.size(); }
		Layer* operator[](size_t index) const { return m_Layers[index]; }

		std::vector<Layer*>::iterator begin() { return m_Layers.begin(); }
		std::vector<Layer*>::iterator end() { return m_Layers.end(); }
		std::vector<Layer*>::reverse_iterator rbegin() { return m_Layers.rbegin(); }
		std::vector<Layer*>::reverse_iterator rend() { return m_Layers.rend(); }

	private:
		std::vector<Layer*> m_Layers;
		size_t m_LayerInsertIndex = 0;
	};

}
