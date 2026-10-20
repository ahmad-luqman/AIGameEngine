#include "Basalt/Audio/SourceEffectsNode.h"

#include "Basalt/Core/Assert.h"

#include <algorithm>
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
		float Cutoff = 0.0f, HighPassMix = 0.0f, Wet = 0.0f, Drive = 0.0f;
		bool Dry = true;
	};
	namespace {
		static_assert(std::is_standard_layout_v<SourceEffectsNode>);
		static_assert(std::atomic<float>::is_always_lock_free);
		constexpr float GlideSeconds = 0.01f;
		constexpr float EchoFeedback = 0.35f;
		float Smooth(float value, float target, float glide)
		{
			const float next = value + (target - value) * glide;
			return std::abs(next - target) < std::max(1e-6f, std::abs(target) * 1e-5f) ? target : next;
		}
		void Process(ma_node* node, const float** framesIn, ma_uint32*, float** framesOut, ma_uint32* frameCountOut)
		{
			auto& s = *static_cast<SourceEffectsNode*>(node)->State;
			const float cutoff = s.HighPass.load(std::memory_order_relaxed);
			const float wet = s.Echo.load(std::memory_order_relaxed);
			const float drive = s.Distortion.load(std::memory_order_relaxed);
			if (cutoff == 0 && wet == 0 && drive == 0 && s.HighPassMix == 0 && s.Wet == 0 && s.Drive == 0)
			{
				if (!s.Dry)
				{
					std::fill(s.Delay.begin(), s.Delay.end(), 0.0f);
					std::fill(s.PreviousInput.begin(), s.PreviousInput.end(), 0.0f);
					std::fill(s.PreviousOutput.begin(), s.PreviousOutput.end(), 0.0f);
					s.Cutoff = 0;
					s.Dry = true;
				}
				std::copy_n(framesIn[0], static_cast<size_t>(*frameCountOut) * s.Channels, framesOut[0]);
				return;
			}
			s.Dry = false;
			const float glide = 1.0f - std::exp(-1.0f / (GlideSeconds * static_cast<float>(s.SampleRate)));
			for (ma_uint32 f = 0; f < *frameCountOut; ++f)
			{
				s.HighPassMix = Smooth(s.HighPassMix, cutoff > 0 ? 1.0f : 0.0f, glide);
				if (cutoff > 0)
					s.Cutoff = s.Cutoff == 0 ? cutoff : Smooth(s.Cutoff, cutoff, glide);
				s.Wet = Smooth(s.Wet, wet, glide);
				s.Drive = Smooth(s.Drive, drive, glide);
				// Bilinear one-pole high-pass, normalized at Nyquist, stable for every supported cutoff.
				const float warp = std::tan(std::numbers::pi_v<float> * s.Cutoff / static_cast<float>(s.SampleRate));
				const float feed = 1.0f / (1.0f + warp), feedback = (1.0f - warp) * feed;
				for (uint32_t c = 0; c < s.Channels; ++c)
				{
					const size_t i = static_cast<size_t>(f) * s.Channels + c;
					const float input = framesIn[0][i];
					const float hp = feedback * s.PreviousOutput[c] + feed * (input - s.PreviousInput[c]);
					s.PreviousInput[c] = input;
					s.PreviousOutput[c] = std::abs(hp) < 1e-20f ? 0.0f : hp;
					float value = input + (hp - input) * s.HighPassMix;
					// Parallel soft clipping preserves unity gain as the control reaches zero.
					if (s.Drive > 0)
						value = (1.0f - s.Drive) * value + s.Drive * std::tanh(value * (1.0f + 15.0f * s.Drive));
					const size_t index = s.Cursor * s.Channels + c;
					const float delayed = s.Delay[index];
					const float echoInput = s.Wet > 0.0f ? value + delayed * EchoFeedback : 0.0f;
					s.Delay[index] = std::abs(echoInput) < 1e-20f ? 0.0f : echoInput;
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
		state->Delay.resize(static_cast<size_t>(sampleRate / 4) * channels);
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
		BS_CORE_ASSERT(std::isfinite(highPass) && highPass >= 0.0f && highPass < 0.5f * static_cast<float>(node->State->SampleRate) && std::isfinite(echo) && echo >= 0.0f && echo <= 1.0f && std::isfinite(distortion) && distortion >= 0.0f && distortion <= 1.0f, "caller sanitizes effect parameters");
		node->State->HighPass.store(highPass, std::memory_order_relaxed);
		node->State->Echo.store(echo, std::memory_order_relaxed);
		node->State->Distortion.store(distortion, std::memory_order_relaxed);
	}
}
