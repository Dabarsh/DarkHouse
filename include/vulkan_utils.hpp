// DarkHouse — small Vulkan helpers shared by the engine translation units.
#pragma once

#include <vulkan/vulkan.h>

#include <string>
#include <utility>

namespace darkhouse {

// "VK_ERROR_DEVICE_LOST", ..., or "VkResult(<n>)" for codes without a name here.
[[nodiscard]] std::string vkResultName(VkResult result);

// Throws std::runtime_error("<what> failed: <result name>") unless result is VK_SUCCESS.
void checkVk(VkResult result, const char* what);

// Runs a callable when the scope ends, including during stack unwinding.
template <class F>
class ScopeExit {
public:
    explicit ScopeExit(F fn) : fn_(std::move(fn)) {}
    ~ScopeExit() { fn_(); }
    ScopeExit(const ScopeExit&) = delete;
    ScopeExit& operator=(const ScopeExit&) = delete;

private:
    F fn_;
};

}  // namespace darkhouse
