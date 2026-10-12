#include "Basalt/Renderer/GpuProfiler.h"

#include "Basalt/Core/Assert.h"
#include "Basalt/Core/Log.h"

#include <algorithm>

namespace Basalt {

	uint32_t GpuProfiler::s_QueriesInUse = 0;

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
		: m_Device(device)
		, m_Enabled(enabled && device)
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

		bool created = true;
		for (FrameSlot& slot : m_Slots)
		{
			slot.Frame = m_Device->createTimerQuery();
			slot.Passes.resize(GetPassCount());
			slot.Recorded.assign(GetPassCount(), 0);
			created = created && slot.Frame;
			for (size_t pass = 0; created && pass < slot.Passes.size(); pass++)
			{
				slot.Passes[pass] = m_Device->createTimerQuery();
				created = slot.Passes[pass] != nullptr;
			}
		}
		if (!created)
		{
			BS_CORE_WARN("GpuProfiler: timer query creation failed; GPU timings are disabled for this renderer");
			m_Enabled = false;
			m_Slots = {};
			return;
		}
		m_QueryCount = queries;
		s_QueriesInUse += queries;
	}

	GpuProfiler::~GpuProfiler()
	{
		s_QueriesInUse -= m_QueryCount;
	}

	bool GpuProfiler::IsReady(const FrameSlot& slot) const
	{
		// Every query of a frame is in one command list, so they finish together; poll them all anyway,
		// because reading one that is not ready would block until it is.
		if (!m_Device->pollTimerQuery(slot.Frame))
			return false;
		for (size_t pass = 0; pass < slot.Passes.size(); pass++)
		{
			if (slot.Recorded[pass] && !m_Device->pollTimerQuery(slot.Passes[pass]))
				return false;
		}
		return true;
	}

	std::optional<GpuFrameSample> GpuProfiler::Collect(FrameSlot& slot)
	{
		slot.Pending = false;
		m_ConsecutiveDrops = 0;

		GpuFrameSample sample;
		sample.FrameMilliseconds = m_Device->getTimerQueryTime(slot.Frame) * 1000.0f;
		sample.PassMilliseconds.assign(slot.Passes.size(), 0.0f);
		for (size_t pass = 0; pass < slot.Passes.size(); pass++)
		{
			if (slot.Recorded[pass])
				sample.PassMilliseconds[pass] = m_Device->getTimerQueryTime(slot.Passes[pass]) * 1000.0f;
		}
		sample.RenderedPixels = slot.RenderedPixels;
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
			if (!IsReady(slot))
			{
				// Normally impossible (see Latency). The slot stays pending, and this frame goes untimed: reusing
				// its queries now could let the old submission write them after the new one reset them. A run
				// of these means results stopped arriving (lost device, a driver that never completes
				// timestamps), which also stalls dynamic resolution.
				m_Average.AddDropped();
				if (++m_ConsecutiveDrops == 2 * Latency)
					BS_CORE_WARN("GpuProfiler: no GPU timer results for {} frames in a row; GPU timings and dynamic resolution are stalled", m_ConsecutiveDrops);
				return std::nullopt;
			}
			sample = Collect(slot);
		}
		std::fill(slot.Recorded.begin(), slot.Recorded.end(), uint8_t{ 0 });
		commandList->beginTimerQuery(slot.Frame);
		m_FrameOpen = true;
		return sample;
	}

	void GpuProfiler::BeginPass(nvrhi::ICommandList* commandList, uint32_t pass)
	{
		BS_CORE_ASSERT(pass < GetPassCount(), "unknown GPU profiler pass");
		BS_CORE_ASSERT(m_OpenPass == NoPass, "GPU profiler passes do not nest");
		commandList->beginMarker(m_Average.Get().Passes[pass].Name.c_str());
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
		m_OpenPass = NoPass;
	}

	void GpuProfiler::EndFrame(nvrhi::ICommandList* commandList, uint64_t renderedPixels)
	{
		BS_CORE_ASSERT(m_OpenPass == NoPass, "GpuProfiler::EndFrame inside a pass");
		if (!m_FrameOpen)
			return;
		FrameSlot& slot = m_Slots[m_Current];
		commandList->endTimerQuery(slot.Frame);
		slot.RenderedPixels = renderedPixels;
		slot.Pending = true;
		m_FrameOpen = false;
	}

}
