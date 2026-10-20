#pragma once

#include <miniaudio.h>

#include <cstdint>

namespace Basalt {
	struct SourceEffectsState;

	// Per-voice high-pass, quarter-second echo and soft distortion. Parameters cross to the audio thread
	// through lock-free atomics; all delay/filter memory belongs to the audio thread.
	struct SourceEffectsNode
	{
		ma_node_base Base;
		// Owned after successful Init; Uninit releases it after the graph stops using the node.
		SourceEffectsState* State = nullptr;
		SourceEffectsNode() = default;
		SourceEffectsNode(const SourceEffectsNode&) = delete;
		SourceEffectsNode& operator=(const SourceEffectsNode&) = delete;
	};
	ma_result SourceEffectsNodeInit(ma_node_graph* graph, uint32_t channels, uint32_t sampleRate, SourceEffectsNode* node);
	void SourceEffectsNodeUninit(SourceEffectsNode* node, const ma_allocation_callbacks* callbacks);
	void SourceEffectsNodeSet(SourceEffectsNode* node, float highPass, float echo, float distortion);
}
