#pragma once

#include "Basalt/Audio/AudioReverbSettings.h"

#include <miniaudio.h>

#include <cstdint>

namespace Basalt {

	struct ReverbState;

	// A miniaudio node that turns its input into reverb (Freeverb, through verblib from miniaudio's extras): wet only,
	// at unity gain, for the reverb send of a bus. Unlike miniaudio's ma_reverb_node it takes new room settings while
	// it plays without a data race: the main thread publishes them through atomics and the audio thread applies them
	// at its next block.
	//
	// Engine-internal (AudioSystem's buses): it includes miniaudio. Base comes first so miniaudio can use the struct as
	// an ma_node; the rest lives in State, which keeps the reverb's buffers and the audio thread's state out of here.
	struct ReverbNode
	{
		ma_node_base Base;
		ReverbState* State;

		// The graph keeps the node's address: never copied or moved.
		ReverbNode() = default;
		ReverbNode(const ReverbNode&) = delete;
		ReverbNode& operator=(const ReverbNode&) = delete;
	};

	// verblib handles 1 or 2 channels at 22.05 to 176.4 kHz: MA_INVALID_ARGS otherwise (and for settings outside
	// ValidateAudioReverb's rules). On failure node is left uninitialized (do not uninit it).
	ma_result ReverbNodeInit(ma_node_graph* graph, uint32_t channels, uint32_t sampleRate, const AudioReverbSettings& settings, ReverbNode* node);
	// Signature matches miniaudio's own node uninit functions.
	void ReverbNodeUninit(ReverbNode* node, const ma_allocation_callbacks* allocationCallbacks);

	// New room settings, from the main thread (valid by ValidateAudioReverb, asserted); the audio thread applies them
	// at its next block. The node must be initialized.
	void ReverbNodeSetSettings(ReverbNode* node, const AudioReverbSettings& settings);

}
