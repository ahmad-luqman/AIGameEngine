#include "Basalt/Audio/ReverbNode.h"

#include "Basalt/Audio/AudioRules.h"
#include "Basalt/Core/Assert.h"

#include <extras/nodes/ma_reverb_node/verblib.h>

#include <array>
#include <atomic>
#include <memory>
#include <type_traits>

namespace Basalt {

	struct ReverbState
	{
		// Published by the main thread: the settings, then a new version (release); the audio thread applies them
		// when it sees a version it has not applied. A block may see settings from two publishes at once, which the
		// next block corrects; nothing reads half-written memory.
		std::array<std::atomic<float>, AudioReverbFields.size()> Settings;
		std::atomic<uint32_t> Version = 0;
		// Audio thread only (and the main thread before the node is in a graph).
		uint32_t AppliedVersion = 0;
		verblib Reverb;
	};

	namespace {

		static_assert(std::is_standard_layout_v<ReverbNode>, "miniaudio casts the node to its first member, Base");
		static_assert(std::atomic<float>::is_always_lock_free, "the audio thread must never block on the settings");

		void Apply(ReverbState& state)
		{
			AudioReverbSettings settings;
			for (size_t i = 0; i < AudioReverbFields.size(); i++)
				settings.*AudioReverbFields[i].Member = state.Settings[i].load(std::memory_order_relaxed);
			verblib_set_room_size(&state.Reverb, settings.RoomSize);
			verblib_set_damping(&state.Reverb, settings.Damping);
			verblib_set_width(&state.Reverb, settings.Width);
		}

		void Store(ReverbState& state, const AudioReverbSettings& settings)
		{
			for (size_t i = 0; i < AudioReverbFields.size(); i++)
				state.Settings[i].store(settings.*AudioReverbFields[i].Member, std::memory_order_relaxed);
		}

		void Process(ma_node* node, const float** framesIn, ma_uint32* /*frameCountIn*/, float** framesOut, ma_uint32* frameCountOut)
		{
			ReverbState& state = *static_cast<ReverbNode*>(node)->State;
			if (const uint32_t version = state.Version.load(std::memory_order_acquire); version != state.AppliedVersion)
			{
				Apply(state);
				state.AppliedVersion = version;
			}
			verblib_process(&state.Reverb, framesIn[0], framesOut[0], *frameCountOut);
		}

		const ma_node_vtable s_VTable = {
			Process,
			nullptr,
			1, // input buses
			1, // output buses
			// The tail rings on after the sends stop (the input is then silence), as with ma_reverb_node.
			MA_NODE_FLAG_CONTINUOUS_PROCESSING
		};

	}

	ma_result ReverbNodeInit(ma_node_graph* graph, uint32_t channels, uint32_t sampleRate, const AudioReverbSettings& settings, ReverbNode* node)
	{
		if (!graph || !node || !ValidateAudioReverb(settings).empty())
			return MA_INVALID_ARGS;
		// verblib's buffers are sized for its highest rate: a few hundred kilobytes, so on the heap.
		auto state = std::make_unique<ReverbState>();
		if (verblib_initialize(&state->Reverb, sampleRate, channels) == 0)
			return MA_INVALID_ARGS;
		// verblib starts wet only at unity gain (the dry sound reaches the bus directly); the room comes from settings.
		Store(*state, settings);
		Apply(*state);

		ma_node_config config = ma_node_config_init();
		config.vtable = &s_VTable;
		const ma_uint32 channelCount = channels;
		config.pInputChannels = &channelCount;
		config.pOutputChannels = &channelCount;
		// State is set before init so a failed init can clear it; the audio thread reaches the node only once it is
		// attached.
		node->State = state.get();
		if (const ma_result result = ma_node_init(graph, &config, nullptr, &node->Base); result != MA_SUCCESS)
		{
			node->State = nullptr;
			return result;
		}
		// The node owns it from here on (ReverbNodeUninit frees it).
		node->State = state.release();
		return MA_SUCCESS;
	}

	void ReverbNodeUninit(ReverbNode* node, const ma_allocation_callbacks* allocationCallbacks)
	{
		// Detaches the node from the graph first (miniaudio lets the audio thread finish with it), then frees State.
		ma_node_uninit(&node->Base, allocationCallbacks);
		delete node->State;
		node->State = nullptr;
	}

	void ReverbNodeSetSettings(ReverbNode* node, const AudioReverbSettings& settings)
	{
		BS_CORE_ASSERT(ValidateAudioReverb(settings).empty(), "the caller validates the settings");
		Store(*node->State, settings);
		node->State->Version.fetch_add(1, std::memory_order_release);
	}

}
