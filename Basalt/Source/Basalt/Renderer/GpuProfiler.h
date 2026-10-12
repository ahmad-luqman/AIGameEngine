#pragma once

#include "Basalt/Core/Base.h"

#include <nvrhi/nvrhi.h>

#include <cstdint>
#include <initializer_list>
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
		// Pixels the frame rendered (EndFrame). Dynamic resolution normalizes the time by them against the
		// viewport current when the result arrives, which stays right even across a resize.
		uint64_t RenderedPixels = 0;

		// The frame time without the given passes (work that does not depend on the render scale). Indices
		// outside the pass list are ignored.
		float MillisecondsWithout(std::initializer_list<uint32_t> passes) const;
	};

	// Averaged GPU time of one named profiler pass (an entry of GpuTimings::Passes).
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
		// Frames left untimed because an earlier frame's results were still not ready when its slot came round,
		// since the profiler was created.
		uint64_t DroppedFrames = 0;
		float FrameMilliseconds = 0.0f;
		// Every pass in pass order, also before any sample arrives. A pass that ran in only some frames counts
		// as 0 in the others, so the passes add up to the frame time minus the work between them (gaps
		// between timestamps).
		std::vector<GpuPassTiming> Passes;
	};

	// Rolling average of the last `window` frame samples, plus the count of dropped frames. Headless, so it is
	// unit-tested without a GPU; it is the profiler's only copy of the pass names.
	class GpuTimingAverage
	{
	public:
		GpuTimingAverage(std::vector<std::string> passNames, uint32_t window);

		// Samples whose pass count differs from the pass names are a programmer error (asserted).
		void Add(const GpuFrameSample& sample);
		void AddDropped() { m_Average.DroppedFrames++; }
		const GpuTimings& Get() const { return m_Average; }

	private:
		void Recompute();

		uint32_t m_Window = 1;
		std::vector<GpuFrameSample> m_Samples;
		size_t m_Next = 0;
		GpuTimings m_Average;
		// Scratch for Recompute, kept to avoid an allocation per frame.
		std::vector<double> m_PassSums;
	};

	// The GPU side of GpuProfiler: timer queries addressed by index, and debug markers. The nvrhi backend is
	// the real one; tests substitute a fake to exercise the profiler's frame bookkeeping without a GPU.
	class GpuTimerBackend
	{
	public:
		virtual ~GpuTimerBackend() = default;

		// Creates queries 0..count-1; false when the device cannot.
		virtual bool CreateQueries(uint32_t count) = 0;
		virtual void BeginQuery(nvrhi::ICommandList* commandList, uint32_t query) = 0;
		virtual void EndQuery(nvrhi::ICommandList* commandList, uint32_t query) = 0;
		// True once the query's result is available; never waits.
		virtual bool PollQuery(uint32_t query) = 0;
		// The query's time; only after PollQuery returned true (reading earlier would wait for the GPU).
		virtual float ReadMilliseconds(uint32_t query) = 0;
		virtual void BeginMarker(nvrhi::ICommandList* commandList, const char* name) = 0;
		virtual void EndMarker(nvrhi::ICommandList* commandList) = 0;
	};

	// The nvrhi timer-query backend for `device` (null without a device).
	Scope<GpuTimerBackend> CreateNvrhiTimerBackend(nvrhi::IDevice* device);

	// Times a renderer's passes on the GPU with timer queries. Results arrive at least Latency frames later,
	// when the slot holding the queries comes round again, and reading them never waits for the GPU: when
	// they have not arrived yet, the slot is kept for later (its frame then arrives a multiple of Latency
	// frames late) and the new frame is not timed (a dropped frame, counted). Pass scopes are also debug
	// markers, so GPU captures show the same names. Without timer queries (or when disabled) only the markers
	// are recorded.
	class GpuProfiler
	{
	public:
		// Render calls between recording a slot and reading it back (the ring size). GraphicsDevice keeps at
		// most MaxFramesInFlight (2 by default) frames in flight, so results are normally ready well before the
		// slot is reused; raise this if that limit grows.
		static constexpr uint32_t Latency = 4;
		// Frames in the average returned by GetTimings (half a second at 60 frames per second).
		static constexpr uint32_t AverageWindow = 30;
		// Timer queries a device provides in total. GraphicsDevice creates the nvrhi device with this many
		// (DeviceDesc::maxTimerQueries), and profilers stay within it, because nvrhi reports running out as an
		// error (a GPU validation error). Counted per process, which is conservative with several devices.
		static constexpr uint32_t DeviceTimerQueryCapacity = 256;
		// Consecutive dropped frames after which a stall is warned about.
		static constexpr uint32_t DropWarningThreshold = 2 * Latency;

		// Times with nvrhi timer queries on `device` (markers only when `enabled` is false or the device is null).
		GpuProfiler(nvrhi::IDevice* device, std::vector<std::string> passNames, bool enabled);
		// Times through `backend` (markers only when `enabled` is false or the backend is null).
		GpuProfiler(Scope<GpuTimerBackend> backend, std::vector<std::string> passNames, bool enabled);
		~GpuProfiler();

		GpuProfiler(const GpuProfiler&) = delete;
		GpuProfiler& operator=(const GpuProfiler&) = delete;

		bool IsEnabled() const { return m_Enabled; }
		// Queries every profiler alive holds (against DeviceTimerQueryCapacity).
		static uint32_t GetQueriesInUse() { return s_QueriesInUse; }

		// Starts a frame on an open command list, before any pass. Returns the frame last recorded in this slot
		// (Latency or more frames earlier) when its results are available; they are also added to the average.
		std::optional<GpuFrameSample> BeginFrame(nvrhi::ICommandList* commandList);
		// A pass may run at most once per frame; passes do not nest. Prefer GpuPassScope.
		void BeginPass(nvrhi::ICommandList* commandList, uint32_t pass);
		void EndPass(nvrhi::ICommandList* commandList, uint32_t pass);
		// Ends the frame before the command list is closed; renderedPixels is returned with the frame's sample.
		void EndFrame(nvrhi::ICommandList* commandList, uint64_t renderedPixels);

		const GpuTimings& GetTimings() const { return m_Average.Get(); }

	private:
		static constexpr uint32_t NoPass = UINT32_MAX;

		struct FrameSlot
		{
			std::vector<uint8_t> Recorded;
			uint64_t RenderedPixels = 0;
			// Recorded and submitted, not read back yet.
			bool Pending = false;
		};

		// Query indices: each slot has one per pass, then one for the whole frame.
		uint32_t PassQuery(uint32_t slot, uint32_t pass) const { return slot * (GetPassCount() + 1) + pass; }
		uint32_t FrameQuery(uint32_t slot) const { return PassQuery(slot, GetPassCount()); }
		bool IsReady(uint32_t slot) const;
		GpuFrameSample Collect(uint32_t slot);
		uint32_t GetPassCount() const { return static_cast<uint32_t>(m_Average.Get().Passes.size()); }

		static uint32_t s_QueriesInUse;

		Scope<GpuTimerBackend> m_Backend;
		std::vector<FrameSlot> m_Slots;
		uint32_t m_Current = 0;
		uint32_t m_QueryCount = 0;
		bool m_Enabled = false;
		bool m_FrameOpen = false;
		uint32_t m_OpenPass = NoPass;
		uint32_t m_ConsecutiveDrops = 0;
		GpuTimingAverage m_Average;
	};

	// Brackets one profiler pass (timer query and debug marker) for the lifetime of the scope, so every
	// BeginPass has its EndPass on all paths.
	class GpuPassScope
	{
	public:
		GpuPassScope(GpuProfiler& profiler, nvrhi::ICommandList* commandList, uint32_t pass)
			: m_Profiler(profiler)
			, m_CommandList(commandList)
			, m_Pass(pass)
		{
			m_Profiler.BeginPass(m_CommandList, m_Pass);
		}
		~GpuPassScope() { m_Profiler.EndPass(m_CommandList, m_Pass); }

		GpuPassScope(const GpuPassScope&) = delete;
		GpuPassScope& operator=(const GpuPassScope&) = delete;

	private:
		GpuProfiler& m_Profiler;
		nvrhi::ICommandList* m_CommandList;
		uint32_t m_Pass;
	};

}
