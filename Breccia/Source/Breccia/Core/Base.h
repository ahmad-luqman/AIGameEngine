#pragma once

#include <cstdint>
#include <memory>
#include <utility>

// ---------------------------------------------------------------------------------------------
// Platform detection
// ---------------------------------------------------------------------------------------------
#if defined(_WIN32)
	#define BR_PLATFORM_WINDOWS 1
#elif defined(__APPLE__)
	#define BR_PLATFORM_MACOS 1
#elif defined(__linux__)
	#define BR_PLATFORM_LINUX 1
#else
	#error "Breccia: unsupported platform"
#endif

#if defined(BR_PLATFORM_WINDOWS)
	#define BR_DEBUGBREAK() __debugbreak()
#elif defined(__clang__) || defined(__GNUC__)
	#define BR_DEBUGBREAK() __builtin_trap()
#endif

#if defined(_MSC_VER)
	#define BR_NOINLINE __declspec(noinline)
#else
	#define BR_NOINLINE __attribute__((noinline))
#endif

#define BR_EXPAND_MACRO(x) x
#define BR_STRINGIFY_MACRO(x) #x

#define BIT(x) (1u << (x))

// Binds a member function as an event callback: BR_BIND_EVENT_FN(Application::OnEvent)
#define BR_BIND_EVENT_FN(fn) [this](auto&&... args) -> decltype(auto) { return this->fn(std::forward<decltype(args)>(args)...); }

namespace Breccia {

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
