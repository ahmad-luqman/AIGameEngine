#include "Basalt/Renderer/GpuProfiler.h"

#include "Basalt/Core/Assert.h"
#include "Basalt/Core/Log.h"

#include <algorithm>

namespace Basalt {

	namespace {

		class NvrhiTimerBackend final : public GpuTimerBackend
		{
		public:
			explicit NvrhiTimerBackend(nvrhi::IDevice* device)
				: m_Device(device)
			{
			}

			bool CreateQueries(uint32_t count) override
			{
				m_Queries.clear();
				for (uint32_t i = 0; i < count; i++)
				{
					nvrhi::TimerQueryHandle query = m_Device->createTimerQuery();
					if (!query)
					{
						m_Queries.clear();
						return false;
					}
					m_Queries.push_back(std::move(query));
				}
				return true;
			}
			void BeginQuery(nvrhi::ICommandList* commandList, uint32_t query) override { commandList->beginTimerQuery(m_Queries[query]); }
			void EndQuery(nvrhi::ICommandList* commandList, uint32_t query) override { commandList->endTimerQuery(m_Queries[query]); }
			bool PollQuery(uint32_t query) override { return m_Device->pollTimerQuery(m_Queries[query]); }
			// getTimerQueryTime also clears nvrhi's "started" state, which lets the query be recorded again.
			float ReadMilliseconds(uint32_t query) override { return m_Device->getTimerQueryTime(m_Queries[query]) * 1000.0f; }
			void BeginMarker(nvrhi::ICommandList* commandList, const char* name) override { commandList->beginMarker(name); }
			void EndMarker(nvrhi::ICommandList* commandList) override { commandList->endMarker(); }

		private:
			nvrhi::IDevice* m_Device;
			std::vector<nvrhi::TimerQueryHandle> m_Queries;
		};

	}

	uint32_t GpuProfiler::s_QueriesInUse = 0;

	Scope<GpuTimerBackend> CreateNvrhiTimerBackend(nvrhi::IDevice* device)
	{
		if (!device)
			return nullptr;
		return CreateScope<NvrhiTimerBackend>(device);
	}

	float GpuFrameSample::MillisecondsWithout(std::initializer_list<uint32_t> passes) const
	{
		float milliseconds = FrameMilliseconds;
		for (const uint32_t pass : passes)
		{
			if (pass < PassMilliseconds.size())
				milliseconds -= PassMilliseconds[pass];
		}
		return milliseconds;
	}

	GpuTimingAverage::GpuTimingAverage(std::vector<std::string> passNames, uint32_t window)
		: m_Window(std::max(window, 1u))
	{
		for (std::string& name : passNames)
			m_Average.Passes.push_back({ std::move(name), 0.0f });
	}

	void GpuTimingAverage::Add(const GpuFrameSample& sample)
	{
		BS_CORE_ASSERT(sample.PassMilliseconds.size() == m_Average.Passes.size(), "GPU frame sample has the wrong number of passes");
		if (m_Samples.size() < m_Window)
			m_Samples.push_back(sample);
		else
			m_Samples[m_Next] = sample;
		m_Next = (m_Next + 1) % m_Window;
		Recompute();
	}

	void GpuTimingAverage::Recompute()
	{
		// Re-summed from the stored samples on every change instead of keeping a running total, which would
		// drift as samples enter and leave the window. The window is short, so this is cheap.
		double frame = 0.0;
		std::vector<double>& passes = m_PassSums;
		passes.assign(m_Average.Passes.size(), 0.0);
		for (const GpuFrameSample& sample : m_Samples)
		{
			frame += sample.FrameMilliseconds;
			for (size_t pass = 0; pass < passes.size() && pass < sample.PassMilliseconds.size(); pass++)
				passes[pass] += sample.PassMilliseconds[pass];
		}
		const double count = static_cast<double>(std::max<size_t>(m_Samples.size(), 1));
		m_Average.SampleCount = static_cast<uint32_t>(m_Samples.size());
		m_Average.FrameMilliseconds = static_cast<float>(frame / count);
		for (size_t pass = 0; pass < passes.size(); pass++)
			m_Average.Passes[pass].Milliseconds = static_cast<float>(passes[pass] / count);
	}

	GpuProfiler::GpuProfiler(nvrhi::IDevice* device, std::vector<std::string> passNames, bool enabled)
		: GpuProfiler(CreateNvrhiTimerBackend(device), std::move(passNames), enabled)
	{
	}

	GpuProfiler::GpuProfiler(Scope<GpuTimerBackend> backend, std::vector<std::string> passNames, bool enabled)
		: m_Backend(std::move(backend))
		, m_Enabled(enabled && m_Backend)
		, m_Average(std::move(passNames), AverageWindow)
	{
		if (!m_Enabled)
			return;

		const uint32_t queries = Latency * (GetPassCount() + 1);
		if (s_QueriesInUse + queries > DeviceTimerQueryCapacity)
		{
			BS_CORE_WARN("GpuProfiler: {} timer queries are in use and {} more would exceed the device's {}; GPU timings are disabled for this renderer", s_QueriesInUse, queries, DeviceTimerQueryCapacity);
			m_Enabled = false;
			return;
		}
		if (!m_Backend->CreateQueries(queries))
		{
			BS_CORE_WARN("GpuProfiler: timer query creation failed; GPU timings are disabled for this renderer");
			m_Enabled = false;
			return;
		}
		m_Slots.resize(Latency);
		for (FrameSlot& slot : m_Slots)
			slot.Recorded.assign(GetPassCount(), 0);
		m_QueryCount = queries;
		s_QueriesInUse += queries;
	}

	GpuProfiler::~GpuProfiler()
	{
		s_QueriesInUse -= m_QueryCount;
	}

	bool GpuProfiler::IsReady(uint32_t slot) const
	{
		// Every query of a frame is in one command list, so they finish together; poll them all anyway,
		// because reading one that is not ready would block until it is.
		if (!m_Backend->PollQuery(FrameQuery(slot)))
			return false;
		for (uint32_t pass = 0; pass < GetPassCount(); pass++)
		{
			if (m_Slots[slot].Recorded[pass] && !m_Backend->PollQuery(PassQuery(slot, pass)))
				return false;
		}
		return true;
	}

	GpuFrameSample GpuProfiler::Collect(uint32_t slot)
	{
		FrameSlot& frame = m_Slots[slot];
		frame.Pending = false;
		m_ConsecutiveDrops = 0;

		GpuFrameSample sample;
		sample.FrameMilliseconds = m_Backend->ReadMilliseconds(FrameQuery(slot));
		sample.PassMilliseconds.assign(GetPassCount(), 0.0f);
		for (uint32_t pass = 0; pass < GetPassCount(); pass++)
		{
			if (frame.Recorded[pass])
				sample.PassMilliseconds[pass] = m_Backend->ReadMilliseconds(PassQuery(slot, pass));
		}
		sample.RenderedPixels = frame.RenderedPixels;
		m_Average.Add(sample);
		return sample;
	}

	std::optional<GpuFrameSample> GpuProfiler::BeginFrame(nvrhi::ICommandList* commandList)
	{
		BS_CORE_ASSERT(!m_FrameOpen, "GpuProfiler::BeginFrame called twice without EndFrame");
		if (!m_Enabled)
			return std::nullopt;

		m_Current = (m_Current + 1) % Latency;
		FrameSlot& slot = m_Slots[m_Current];
		std::optional<GpuFrameSample> sample;
		if (slot.Pending)
		{
			if (!IsReady(m_Current))
			{
				// Normally impossible (see Latency). The slot stays pending, and this frame goes untimed: reusing
				// its queries now could let the old submission write them after the new one reset them. A run
				// of these means results stopped arriving (lost device, a driver that never completes
				// timestamps), which also stalls dynamic resolution.
				m_Average.AddDropped();
				if (++m_ConsecutiveDrops == DropWarningThreshold)
					BS_CORE_WARN("GpuProfiler: no GPU timer results for {} frames in a row; GPU timings and dynamic resolution are stalled", m_ConsecutiveDrops);
				return std::nullopt;
			}
			sample = Collect(m_Current);
		}
		std::fill(slot.Recorded.begin(), slot.Recorded.end(), uint8_t{ 0 });
		m_Backend->BeginQuery(commandList, FrameQuery(m_Current));
		m_FrameOpen = true;
		return sample;
	}

	void GpuProfiler::BeginPass(nvrhi::ICommandList* commandList, uint32_t pass)
	{
		BS_CORE_ASSERT(pass < GetPassCount(), "unknown GPU profiler pass");
		BS_CORE_ASSERT(m_OpenPass == NoPass, "GPU profiler passes do not nest");
		if (m_Backend)
			m_Backend->BeginMarker(commandList, m_Average.Get().Passes[pass].Name.c_str());
		m_OpenPass = pass;
		if (!m_FrameOpen)
			return;
		FrameSlot& slot = m_Slots[m_Current];
		BS_CORE_ASSERT(!slot.Recorded[pass], "a GPU profiler pass ran twice in one frame");
		m_Backend->BeginQuery(commandList, PassQuery(m_Current, pass));
		slot.Recorded[pass] = 1;
	}

	void GpuProfiler::EndPass(nvrhi::ICommandList* commandList, uint32_t pass)
	{
		BS_CORE_ASSERT(m_OpenPass == pass, "GpuProfiler::EndPass does not match the open pass");
		if (m_FrameOpen)
			m_Backend->EndQuery(commandList, PassQuery(m_Current, pass));
		if (m_Backend)
			m_Backend->EndMarker(commandList);
		m_OpenPass = NoPass;
	}

	void GpuProfiler::EndFrame(nvrhi::ICommandList* commandList, uint64_t renderedPixels)
	{
		BS_CORE_ASSERT(m_OpenPass == NoPass, "GpuProfiler::EndFrame inside a pass");
		if (!m_FrameOpen)
			return;
		FrameSlot& slot = m_Slots[m_Current];
		m_Backend->EndQuery(commandList, FrameQuery(m_Current));
		slot.RenderedPixels = renderedPixels;
		slot.Pending = true;
		m_FrameOpen = false;
	}

}
