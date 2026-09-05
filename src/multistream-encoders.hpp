#pragma once

#include <obs.h>

#include <string>

namespace dynamic_delay {

// Configure an empty, inactive internal capture output. Shares compatible
// active native-stream encoders; otherwise creates a private H.264/AAC pair
// from the current profile without starting or changing the native stream.
[[nodiscard]] bool configure_multistream_encoders(obs_output_t *master, std::string &error);

} // namespace dynamic_delay
