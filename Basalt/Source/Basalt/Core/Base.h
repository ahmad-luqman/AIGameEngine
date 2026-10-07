#pragma once

#include <cstdint>
#include <memory>
#include <utility>

// ---------------------------------------------------------------------------------------------
// Platform detection
// ---------------------------------------------------------------------------------------------
#if defined(_WIN32)
	#define BS_PLATFORM_WINDOWS 1
#elif defined(__APPLE__)
	#define BS_PLATFORM_MACOS 1
#elif defined(__linux__)
	#define BS_PLATFORM_LINUX 1
#else
	#error "Basalt: unsupported platform"
#endif

#if defined(BS_PLATFORM_WINDOWS)
	#define BS_DEBUGBREAK() __debugbreak()
#elif defined(__clang__) || defined(__GNUC__)
	#define BS_DEBUGBREAK() __builtin_trap()
#endif

#define BS_EXPAND_MACRO(x) x
#define BS_STRINGIFY_MACRO(x) #x

#define BIT(x) (1u << (x))

// Binds a member function as an event callback: BS_BIND_EVENT_FN(Application::OnEvent)
#define BS_BIND_EVENT_FN(fn) [this](auto&&... args) -> decltype(auto) { return this->fn(std::forward<decltype(args)>(args)...); }

namespace Basalt {

	// Owning pointer with a single owner.
	template<typename T>
	using Scope = std::unique_ptr<T>;

	template<typename T, typename... Args>
	constexpr Scope<T> CreateScope(Args&&... args)
	{
		return std::make_unique<T>(std::forward<Args>(args)...);
	}

	// Reference-counted shared ownership.
	template<typename T>
	using Ref = std::shared_ptr<T>;

	template<typename T, typename... Args>
	constexpr Ref<T> CreateRef(Args&&... args)
	{
		return std::make_shared<T>(std::forward<Args>(args)...);
	}

}
