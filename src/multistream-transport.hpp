#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace dynamic_delay {

struct MultistreamTarget {
	std::string id;
	std::string name;
	std::string server;
	std::string key;
};

// H.264 video and AAC audio only. Header bytes must match the shared encoded
// payloads supplied to submit(); this transport never encodes or decodes.
struct StreamDescription {
	std::vector<uint8_t> videoExtraData;
	std::vector<uint8_t> audioExtraData;
	uint32_t width = 0;
	uint32_t height = 0;
	uint32_t sampleRate = 0;
	uint32_t channels = 0;
};

struct SharedEncodedPacket {
	std::shared_ptr<const std::vector<uint8_t>> data;
	bool video = false;
	bool keyframe = false;
	int64_t ptsUsec = 0;
	int64_t dtsUsec = 0;
};

enum class MultistreamState { Connecting, WaitingForKeyframe, Streaming, Reconnecting, Stopped, Error };

struct MultistreamStatus {
	std::string id;
	std::string name;
	MultistreamState state = MultistreamState::Stopped;
	// Static, non-sensitive diagnostic. Never contains a URL, key, or a raw
	// protocol/FFmpeg error string.
	std::string detail;
	std::size_t queuedBytes = 0;
	std::size_t queuedPackets = 0;
	uint64_t bytesSent = 0;
	uint32_t reconnectAttempts = 0;
};

class MultistreamTransport final {
public:
	MultistreamTransport();
	~MultistreamTransport();
	MultistreamTransport(const MultistreamTransport &) = delete;
	MultistreamTransport &operator=(const MultistreamTransport &) = delete;
	bool start(const MultistreamTarget &target, const StreamDescription &description, std::string &error);
	void stop(const std::string &id);
	void stop_all();
	void submit(const SharedEncodedPacket &packet);
	[[nodiscard]] std::vector<MultistreamStatus> snapshot() const;
	[[nodiscard]] bool active() const;
	[[nodiscard]] bool has_targets() const;

private:
	class Impl;
	std::unique_ptr<Impl> impl_;
};

} // namespace dynamic_delay
