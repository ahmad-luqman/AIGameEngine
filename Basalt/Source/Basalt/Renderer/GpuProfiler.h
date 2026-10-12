#pragma once

#include <nvrhi/nvrhi.h>

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace Basalt {

	// GPU time of one profiled frame.
	struct GpuFrameSample
	{
		float FrameMilliseconds = 0.0f;
		// One entry per profiler pass, in pass order; 0 for passes that did not run that frame.
		std::vector<float> PassMilliseconds;
		// Render scale the frame was drawn at (dynamic resolution compares frames drawn at different scales).
		float RenderScale = 1.0f;
	};

	struct GpuPassTiming
	{
		std::string Name;
		float Milliseconds = 0.0f;
	};

	// Average GPU times over the last profiled frames.
	struct GpuTimings
	{
		// Frames in the average; 0 until the first results arrive (always 0 without timer queries).
		uint32_t SampleCount = 0;
		float FrameMilliseconds = 0.0f;
		// Every pass in pass order. A pass that ran in only some frames counts as 0 in the others, so the
		// passes add up to the frame time minus the work outside them (uploads, one-time setup).
		std::vector<GpuPassTiming> Passes;
	};

	// Rolling average of the last `window` frame samples. Headless, so it is unit-tested without a GPU.
	class GpuTimingAverage
	{
	public:
		GpuTimingAverage(std::vector<std::string> passNames, uint32_t window);

		// Samples whose pass count differs from the pass names are a programmer error (asserted).
		void Add(const GpuFrameSample& sample);
		void Clear();
		const GpuTimings& Get() const { return m_Average; }

	private:
		void Recompute();

		std::vector<std::string> m_PassNames;
		uint32_t m_Window = 1;
		std::vector<GpuFrameSample> m_Samples;
		size_t m_Next = 0;
		GpuTimings m_Average;
	};

	// Times a renderer's passes on the GPU with timer queries. Results arrive Latency frames later, when the
	// slot holding the queries comes round again, and reading them never waits for the GPU: a frame that has
	// not finished by then is dropped. Pass scopes are also debug markers, so GPU captures show the same
	// names. Without timer-query support (or when disabled) only the markers are recorded.
	class GpuProfiler
	{
	public:
		// Frames between recording a slot and reading it back. The device keeps at most two frames in flight,
		// so results are normally ready well before the slot is reused.
		static constexpr uint32_t Latency = 4;
		// Frames in the average returned by GetTimings (half a second at 60 frames per second).
		static constexpr uint32_t AverageWindow = 30;

		// Creates every timer query up front (nvrhi's pool holds 256). Disables itself, with a warning, when the
		// device runs out of queries.
		GpuProfiler(nvrhi::IDevice* device, std::vector<std::string> passNames, bool enabled);

		GpuProfiler(const GpuProfiler&) = delete;
		GpuProfiler& operator=(const GpuProfiler&) = delete;

		bool IsEnabled() const { return m_Enabled; }

		// Starts a frame on an open command list, before any pass. Returns the frame recorded Latency frames
		// earlier when its results are available (they are also added to the average).
		std::optional<GpuFrameSample> BeginFrame(nvrhi::ICommandList* commandList);
		// A pass may run at most once per frame; passes do not nest.
		void BeginPass(nvrhi::ICommandList* commandList, uint32_t pass);
		void EndPass(nvrhi::ICommandList* commandList, uint32_t pass);
		// Ends the frame before the command list is closed; renderScale is returned with the frame's sample.
		void EndFrame(nvrhi::ICommandList* commandList, float renderScale);

		const GpuTimings& GetTimings() const { return m_Average.Get(); }

	private:
		struct FrameSlot
		{
			nvrhi::TimerQueryHandle Frame;
			std::vector<nvrhi::TimerQueryHandle> Passes;
			std::vector<uint8_t> Recorded;
			float RenderScale = 1.0f;
			// Recorded and submitted, not read back yet.
			bool Pending = false;
		};

		std::optional<GpuFrameSample> Collect(FrameSlot& slot);

		nvrhi::IDevice* m_Device = nullptr;
		std::vector<std::string> m_PassNames;
		std::array<FrameSlot, Latency> m_Slots;
		uint32_t m_Current = 0;
		bool m_Enabled = false;
		bool m_FrameOpen = false;
		// Pass being recorded, or UINT32_MAX.
		uint32_t m_OpenPass = UINT32_MAX;
		GpuTimingAverage m_Average;
	};

}
