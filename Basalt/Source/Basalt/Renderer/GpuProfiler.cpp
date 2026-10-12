#include "Basalt/Renderer/GpuProfiler.h"

#include "Basalt/Core/Assert.h"
#include "Basalt/Core/Log.h"

#include <algorithm>

namespace Basalt {

	GpuTimingAverage::GpuTimingAverage(std::vector<std::string> passNames, uint32_t window)
		: m_PassNames(std::move(passNames))
		, m_Window(std::max(window, 1u))
	{
		Recompute();
	}

	void GpuTimingAverage::Add(const GpuFrameSample& sample)
	{
		BS_CORE_ASSERT(sample.PassMilliseconds.size() == m_PassNames.size(), "GPU frame sample has the wrong number of passes");
		if (m_Samples.size() < m_Window)
			m_Samples.push_back(sample);
		else
			m_Samples[m_Next] = sample;
		m_Next = (m_Next + 1) % m_Window;
		Recompute();
	}

	void GpuTimingAverage::Clear()
	{
		m_Samples.clear();
		m_Next = 0;
		Recompute();
	}

	void GpuTimingAverage::Recompute()
	{
		m_Average.SampleCount = static_cast<uint32_t>(m_Samples.size());
		m_Average.FrameMilliseconds = 0.0f;
		m_Average.Passes.resize(m_PassNames.size());
		for (size_t pass = 0; pass < m_PassNames.size(); pass++)
			m_Average.Passes[pass] = { m_PassNames[pass], 0.0f };
		if (m_Samples.empty())
			return;

		// Sums in double: the window is short, but a long-lived float accumulator would drift.
		double frame = 0.0;
		std::vector<double> passes(m_PassNames.size(), 0.0);
		for (const GpuFrameSample& sample : m_Samples)
		{
			frame += sample.FrameMilliseconds;
			for (size_t pass = 0; pass < passes.size() && pass < sample.PassMilliseconds.size(); pass++)
				passes[pass] += sample.PassMilliseconds[pass];
		}
		const double count = static_cast<double>(m_Samples.size());
		m_Average.FrameMilliseconds = static_cast<float>(frame / count);
		for (size_t pass = 0; pass < passes.size(); pass++)
			m_Average.Passes[pass].Milliseconds = static_cast<float>(passes[pass] / count);
	}

	GpuProfiler::GpuProfiler(nvrhi::IDevice* device, std::vector<std::string> passNames, bool enabled)
		: m_Device(device)
		, m_PassNames(passNames)
		, m_Enabled(enabled && device)
		, m_Average(std::move(passNames), AverageWindow)
	{
		if (!m_Enabled)
			return;
		for (FrameSlot& slot : m_Slots)
		{
			slot.Frame = m_Device->createTimerQuery();
			slot.Passes.resize(m_PassNames.size());
			slot.Recorded.assign(m_PassNames.size(), 0);
			for (nvrhi::TimerQueryHandle& query : slot.Passes)
			{
				query = m_Device->createTimerQuery();
				if (!query)
					break;
			}
			const bool created = slot.Frame && std::all_of(slot.Passes.begin(), slot.Passes.end(), [](const nvrhi::TimerQueryHandle& query) { return query != nullptr; });
			if (!created)
			{
				BS_CORE_WARN("GpuProfiler: the device has no timer queries left; GPU timings are disabled for this renderer");
				m_Enabled = false;
				m_Slots = {};
				return;
			}
		}
	}

	std::optional<GpuFrameSample> GpuProfiler::Collect(FrameSlot& slot)
	{
		slot.Pending = false;
		// Every query of a frame is in one command list, so they finish together; poll them all anyway,
		// because reading one that is not ready would block until it is.
		bool ready = m_Device->pollTimerQuery(slot.Frame);
		for (size_t pass = 0; ready && pass < slot.Passes.size(); pass++)
		{
			if (slot.Recorded[pass])
				ready = m_Device->pollTimerQuery(slot.Passes[pass]);
		}
		if (!ready)
		{
			// Dropped. Resetting lets the queries be recorded again (nvrhi refuses to restart a query whose
			// result has not been read or reset).
			m_Device->resetTimerQuery(slot.Frame);
			for (size_t pass = 0; pass < slot.Passes.size(); pass++)
			{
				if (slot.Recorded[pass])
					m_Device->resetTimerQuery(slot.Passes[pass]);
			}
			return std::nullopt;
		}

		GpuFrameSample sample;
		sample.FrameMilliseconds = m_Device->getTimerQueryTime(slot.Frame) * 1000.0f;
		sample.PassMilliseconds.assign(slot.Passes.size(), 0.0f);
		for (size_t pass = 0; pass < slot.Passes.size(); pass++)
		{
			if (slot.Recorded[pass])
				sample.PassMilliseconds[pass] = m_Device->getTimerQueryTime(slot.Passes[pass]) * 1000.0f;
		}
		sample.RenderScale = slot.RenderScale;
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
			sample = Collect(slot);
		std::fill(slot.Recorded.begin(), slot.Recorded.end(), uint8_t{ 0 });
		commandList->beginTimerQuery(slot.Frame);
		m_FrameOpen = true;
		return sample;
	}

	void GpuProfiler::BeginPass(nvrhi::ICommandList* commandList, uint32_t pass)
	{
		BS_CORE_ASSERT(pass < m_PassNames.size(), "unknown GPU profiler pass");
		BS_CORE_ASSERT(m_OpenPass == UINT32_MAX, "GPU profiler passes do not nest");
		commandList->beginMarker(m_PassNames[pass].c_str());
		m_OpenPass = pass;
		if (!m_FrameOpen)
			return;
		FrameSlot& slot = m_Slots[m_Current];
		BS_CORE_ASSERT(!slot.Recorded[pass], "a GPU profiler pass ran twice in one frame");
		commandList->beginTimerQuery(slot.Passes[pass]);
		slot.Recorded[pass] = 1;
	}

	void GpuProfiler::EndPass(nvrhi::ICommandList* commandList, uint32_t pass)
	{
		BS_CORE_ASSERT(m_OpenPass == pass, "GpuProfiler::EndPass does not match the open pass");
		if (m_FrameOpen)
			commandList->endTimerQuery(m_Slots[m_Current].Passes[pass]);
		commandList->endMarker();
		m_OpenPass = UINT32_MAX;
	}

	void GpuProfiler::EndFrame(nvrhi::ICommandList* commandList, float renderScale)
	{
		BS_CORE_ASSERT(m_OpenPass == UINT32_MAX, "GpuProfiler::EndFrame inside a pass");
		if (!m_FrameOpen)
			return;
		FrameSlot& slot = m_Slots[m_Current];
		commandList->endTimerQuery(slot.Frame);
		slot.RenderScale = renderScale;
		slot.Pending = true;
		m_FrameOpen = false;
	}

}
