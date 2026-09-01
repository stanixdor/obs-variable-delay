#include "delay-types.hpp"

namespace dynamic_delay {

const char *state_name(const DelayState state) noexcept
{
	switch (state) {
	case DelayState::Bypass:
		return "bypass";
	case DelayState::Preparing:
		return "preparing";
	case DelayState::Filling:
		return "filling";
	case DelayState::Delayed:
		return "delayed";
	case DelayState::ReturningLive:
		return "returning-live";
	case DelayState::Error:
		return "error";
	}
	return "unknown";
}

} // namespace dynamic_delay
