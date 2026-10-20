#include "Basalt/Audio/SourceEffectsNode.h"

#include <atomic>
#include <cmath>
#include <memory>
#include <numbers>
#include <type_traits>
#include <vector>

namespace Basalt {
	struct SourceEffectsState
	{
		std::atomic<float> HighPass{ 0.0f }, Echo{ 0.0f }, Distortion{ 0.0f };
		uint32_t Channels = 0, SampleRate = 0;
		size_t Cursor = 0;
		std::vector<float> Delay, PreviousInput, PreviousOutput;
		float Cutoff = 0.0f, Wet = 0.0f, Drive = 0.0f;
	};
	namespace {
		static_assert(std::is_standard_layout_v<SourceEffectsNode>);
		static_assert(std::atomic<float>::is_always_lock_free);
		void Process(ma_node* node, const float** framesIn, ma_uint32*, float** framesOut, ma_uint32* frameCountOut)
		{
			auto& s = *static_cast<SourceEffectsNode*>(node)->State;
			const float cutoff = s.HighPass.load(std::memory_order_relaxed);
			const float wet = s.Echo.load(std::memory_order_relaxed);
			const float drive = s.Distortion.load(std::memory_order_relaxed);
			const float glide = 1.0f - std::exp(-1.0f / (0.01f * static_cast<float>(s.SampleRate)));
			for (ma_uint32 f = 0; f < *frameCountOut; ++f)
			{
				s.Cutoff += (cutoff - s.Cutoff) * glide;
				s.Wet += (wet - s.Wet) * glide;
				s.Drive += (drive - s.Drive) * glide;
				const float alpha = std::exp(-2.0f * std::numbers::pi_v<float> * s.Cutoff / static_cast<float>(s.SampleRate));
				for (uint32_t c = 0; c < s.Channels; ++c)
				{
					const size_t i = size_t(f) * s.Channels + c;
					const float input = framesIn[0][i];
					const float hp = alpha * (s.PreviousOutput[c] + input - s.PreviousInput[c]);
					s.PreviousInput[c] = input;
					s.PreviousOutput[c] = hp;
					float value = s.Cutoff > 0.01f ? hp : input;
					// Parallel soft clipping preserves unity gain as the control reaches zero.
					value = (1.0f - s.Drive) * value + s.Drive * std::tanh(value * (1.0f + 15.0f * s.Drive));
					const size_t index = s.Cursor * s.Channels + c;
					const float delayed = s.Delay[index];
					s.Delay[index] = s.Wet > 0.00001f ? value + delayed * 0.35f : 0.0f;
					framesOut[0][i] = value + delayed * s.Wet;
				}
				s.Cursor = (s.Cursor + 1) % (s.Delay.size() / s.Channels);
			}
		}
		const ma_node_vtable s_VTable = { Process, nullptr, 1, 1, MA_NODE_FLAG_CONTINUOUS_PROCESSING };
	}
	ma_result SourceEffectsNodeInit(ma_node_graph* graph, uint32_t channels, uint32_t sampleRate, SourceEffectsNode* node)
	{
		if (!graph || !node || channels == 0 || channels > 8 || sampleRate < 8000 || sampleRate > 192000)
			return MA_INVALID_ARGS;
		auto state = std::make_unique<SourceEffectsState>();
		state->Channels = channels;
		state->SampleRate = sampleRate;
		state->Delay.resize(size_t(sampleRate / 4) * channels);
		state->PreviousInput.resize(channels);
		state->PreviousOutput.resize(channels);
		ma_node_config config = ma_node_config_init();
		config.vtable = &s_VTable;
		config.pInputChannels = &channels;
		config.pOutputChannels = &channels;
		node->State = state.get();
		const ma_result result = ma_node_init(graph, &config, nullptr, &node->Base);
		if (result != MA_SUCCESS)
		{
			node->State = nullptr;
			return result;
		}
		node->State = state.release();
		return MA_SUCCESS;
	}
	void SourceEffectsNodeUninit(SourceEffectsNode* node, const ma_allocation_callbacks* callbacks)
	{
		ma_node_uninit(&node->Base, callbacks);
		delete node->State;
		node->State = nullptr;
	}
	void SourceEffectsNodeSet(SourceEffectsNode* node, float highPass, float echo, float distortion)
	{
		node->State->HighPass.store(highPass, std::memory_order_relaxed);
		node->State->Echo.store(echo, std::memory_order_relaxed);
		node->State->Distortion.store(distortion, std::memory_order_relaxed);
	}
}
