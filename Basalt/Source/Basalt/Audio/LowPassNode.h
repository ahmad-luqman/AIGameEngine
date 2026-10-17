#pragma once

#include <miniaudio.h>

#include <cstdint>

namespace Basalt {

	struct LowPassState;

	// A miniaudio node that low-passes its input at 12 dB per octave, built for a cutoff that changes while it plays
	// (AudioSource.LowPass follows occlusion every frame). It is a state-variable filter in its topology-preserving
	// form, which stays stable and quiet when its coefficients move, unlike the biquad behind miniaudio's
	// ma_lpf_node; and it glides to a new cutoff (in log frequency, with a 10 ms time constant: settled within about
	// 100 ms) instead of jumping. The main thread sets the
	// cutoff through an atomic, so it never writes what the audio thread reads mid-block.
	//
	// Engine-internal (AudioSystem's effect chain): it includes miniaudio. Base comes first so miniaudio can use the
	// struct as an ma_node; the rest lives in State, which keeps the audio thread's state and its headers out of here.
	struct LowPassNode
	{
		ma_node_base Base;
		LowPassState* State;

		// The graph keeps the node's address: never copied or moved.
		LowPassNode() = default;
		LowPassNode(const LowPassNode&) = delete;
		LowPassNode& operator=(const LowPassNode&) = delete;
	};

	// channels and sampleRate are the sound's output; cutoff (Hz, finite, above 0 and below the Nyquist frequency)
	// applies from the first frame. MA_INVALID_ARGS for anything else; on failure node is left uninitialized (do not
	// uninit it).
	ma_result LowPassNodeInit(ma_node_graph* graph, uint32_t channels, uint32_t sampleRate, float cutoff, LowPassNode* node);
	// Signature matches miniaudio's own node uninit functions.
	void LowPassNodeUninit(LowPassNode* node, const ma_allocation_callbacks* allocationCallbacks);

	// The cutoff to glide to, from the main thread; the audio thread picks it up at its next block. The caller keeps it
	// finite, above 0 and below the Nyquist frequency (asserted). The node must be initialized.
	void LowPassNodeSetCutoff(LowPassNode* node, float cutoff);
	// For a filter that goes back into a chain after being bypassed: the audio thread clears its memory and starts at
	// cutoff without gliding, so it neither replays the sound from before the bypass nor sweeps.
	void LowPassNodeRestart(LowPassNode* node, float cutoff);
	// The cutoff the filter had reached at the end of its last block (for tests and diagnostics).
	float LowPassNodeGetCurrentCutoff(const LowPassNode* node);

}
