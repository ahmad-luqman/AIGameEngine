#include "Basalt/Audio/LowPassNode.h"

#include <glm/glm.hpp>

#include <atomic>
#include <cmath>
#include <memory>
#include <type_traits>
#include <vector>

namespace Basalt {

	namespace {

		static_assert(std::is_standard_layout_v<LowPassNode>, "miniaudio casts the node to its first member, Base");
		static_assert(std::atomic<float>::is_always_lock_free, "the audio thread must never block on the cutoff");

		// About 10 ms to cover most (63%) of the way to a new cutoff, in log frequency (an octave takes as long at
		// 100 Hz as at 10 kHz): fast enough to follow occlusion, slow enough to never click.
		constexpr float GlideSeconds = 0.01f;
		// A Butterworth response (Q = 1/sqrt(2)): flat up to the cutoff, no resonant peak. The filter's damping is 1/Q.
		constexpr float Damping = 1.41421356f;
		constexpr float Pi = 3.14159265f;

	}

	struct LowPassState
	{
		std::atomic<float> TargetCutoff = 0.0f;
		std::atomic<bool> RestartPending = false;
		// Audio thread only (and the main thread before the node is in a graph).
		float Cutoff = 0.0f;
		float SampleRate = 48000.0f;
		uint32_t Channels = 0;
		// The two integrators of each channel.
		std::vector<glm::vec2> Integrators;
	};

	namespace {

		// The filter's frequency warp for a cutoff: tan(pi * cutoff / sampleRate).
		float Warp(float cutoff, float sampleRate)
		{
			return std::tan(Pi * cutoff / sampleRate);
		}

		void Process(ma_node* node, const float** framesIn, ma_uint32* frameCountIn, float** framesOut, ma_uint32* frameCountOut)
		{
			(void)frameCountIn;
			LowPassState& state = *static_cast<LowPassNode*>(node)->State;
			const ma_uint32 frames = *frameCountOut;
			const float* in = framesIn[0];
			float* out = framesOut[0];

			const float target = state.TargetCutoff.load(std::memory_order_relaxed);
			if (state.RestartPending.exchange(false, std::memory_order_acquire))
			{
				for (glm::vec2& integrators : state.Integrators)
					integrators = glm::vec2(0.0f);
				state.Cutoff = target;
			}
			// Glide in log frequency over this block; the warp is interpolated per frame, which keeps the sweep smooth.
			const float start = state.Cutoff;
			float end = target;
			if (start != target)
			{
				const float keep = std::exp(-static_cast<float>(frames) / (GlideSeconds * state.SampleRate));
				end = std::exp(std::log(target) + (std::log(start) - std::log(target)) * keep);
				// Close enough to stop gliding (and to compare equal from now on).
				if (std::abs(end - target) <= target * 1e-4f)
					end = target;
			}
			state.Cutoff = end;
			const float startWarp = Warp(start, state.SampleRate);
			const float endWarp = Warp(end, state.SampleRate);

			const uint32_t channels = state.Channels;
			for (ma_uint32 frame = 0; frame < frames; frame++)
			{
				const float t = frames > 1 ? static_cast<float>(frame) / static_cast<float>(frames - 1) : 1.0f;
				const float g = startWarp + (endWarp - startWarp) * t;
				// Zavalishin's topology-preserving transform of the state-variable filter (as in Cytomic's notes).
				const float a1 = 1.0f / (1.0f + g * (g + Damping));
				const float a2 = g * a1;
				const float a3 = g * a2;
				for (uint32_t channel = 0; channel < channels; channel++)
				{
					glm::vec2& ic = state.Integrators[channel];
					const float v3 = in[frame * channels + channel] - ic.y;
					const float v1 = a1 * ic.x + a2 * v3;
					const float v2 = ic.y + a2 * ic.x + a3 * v3;
					ic = glm::vec2(2.0f * v1 - ic.x, 2.0f * v2 - ic.y);
					out[frame * channels + channel] = v2;
				}
			}
		}

		ma_node_vtable s_VTable = {
			Process,
			nullptr,
			1, // input buses
			1, // output buses
			0  // flags: no input, no output (no tail worth keeping)
		};

	}

	ma_result LowPassNodeInit(ma_node_graph* graph, uint32_t channels, uint32_t sampleRate, float cutoff, LowPassNode* node)
	{
		if (!graph || !node || channels == 0 || sampleRate == 0 || !(cutoff > 0.0f))
			return MA_INVALID_ARGS;
		auto state = std::make_unique<LowPassState>();
		state->TargetCutoff.store(cutoff);
		state->Cutoff = cutoff;
		state->SampleRate = static_cast<float>(sampleRate);
		state->Channels = channels;
		state->Integrators.assign(channels, glm::vec2(0.0f));

		ma_node_config config = ma_node_config_init();
		config.vtable = &s_VTable;
		const ma_uint32 channelCount = channels;
		config.pInputChannels = &channelCount;
		config.pOutputChannels = &channelCount;
		// State is set before the node joins the graph: the audio thread may process it as soon as it is attached.
		node->State = state.get();
		if (const ma_result result = ma_node_init(graph, &config, nullptr, &node->Base); result != MA_SUCCESS)
		{
			node->State = nullptr;
			return result;
		}
		// The node owns it from here on (LowPassNodeUninit frees it).
		node->State = state.release();
		return MA_SUCCESS;
	}

	void LowPassNodeUninit(LowPassNode* node, const ma_allocation_callbacks* allocationCallbacks)
	{
		// Detaches the node from the graph first (miniaudio lets the audio thread finish with it), then frees State.
		ma_node_uninit(&node->Base, allocationCallbacks);
		delete node->State;
		node->State = nullptr;
	}

	void LowPassNodeSetCutoff(LowPassNode* node, float cutoff)
	{
		node->State->TargetCutoff.store(cutoff, std::memory_order_relaxed);
	}

	void LowPassNodeRestart(LowPassNode* node, float cutoff)
	{
		node->State->TargetCutoff.store(cutoff, std::memory_order_relaxed);
		node->State->RestartPending.store(true, std::memory_order_release);
	}

	float LowPassNodeGetCurrentCutoff(const LowPassNode* node)
	{
		return node->State->Cutoff;
	}

}
