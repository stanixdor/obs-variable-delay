#include "multistream-transport.hpp"

#include "core/multistream-queue.hpp"

extern "C" {
#include <libavcodec/packet.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/mem.h>
#include <libavutil/opt.h>
#include <librtmp/log.h>
#include <librtmp/rtmp.h>
}

#include <QCoreApplication>
#include <QEventLoop>
#include <QHostAddress>
#include <QHostInfo>
#include <QTimer>
#include <QUrl>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <climits>
#include <condition_variable>
#include <cstring>
#include <functional>
#include <limits>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <utility>

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
#endif

namespace dynamic_delay {
namespace {

using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;
constexpr uint64_t ConnectTimeoutUsec = 5'000'000;
constexpr uint64_t WriteTimeoutUsec = 1'000'000;
constexpr std::size_t MaxFlvTagBytes = 16U * 1024U * 1024U;
#ifdef _WIN32
constexpr SOCKET InvalidSocket = INVALID_SOCKET;
using SocketLength = int;
#else
constexpr SOCKET InvalidSocket = -1;
using SocketLength = socklen_t;
#endif

uint64_t now_usec() noexcept
{
	return static_cast<uint64_t>(
		std::chrono::duration_cast<std::chrono::microseconds>(Clock::now().time_since_epoch()).count());
}

void close_socket(const SOCKET socket) noexcept
{
	if (socket == InvalidSocket)
		return;
#ifdef _WIN32
	closesocket(socket);
#else
	close(socket);
#endif
}

void shutdown_socket(const SOCKET socket) noexcept
{
	if (socket == InvalidSocket)
		return;
#ifdef _WIN32
	shutdown(socket, SD_BOTH);
#else
	shutdown(socket, SHUT_RDWR);
#endif
}

SOCKET duplicate_socket(const SOCKET socket) noexcept
{
#ifdef _WIN32
	WSAPROTOCOL_INFOW info{};
	if (WSADuplicateSocketW(socket, GetCurrentProcessId(), &info) != 0)
		return InvalidSocket;
	return WSASocketW(FROM_PROTOCOL_INFO, FROM_PROTOCOL_INFO, FROM_PROTOCOL_INFO, &info, 0, WSA_FLAG_OVERLAPPED);
#else
	return dup(socket);
#endif
}

bool nonblocking(const SOCKET socket, const bool enabled) noexcept
{
#ifdef _WIN32
	u_long mode = enabled ? 1UL : 0UL;
	return ioctlsocket(socket, FIONBIO, &mode) == 0;
#else
	const int flags = fcntl(socket, F_GETFL, 0);
	return flags >= 0 && fcntl(socket, F_SETFL, enabled ? flags | O_NONBLOCK : flags & ~O_NONBLOCK) == 0;
#endif
}

class SocketOwner final {
public:
	explicit SocketOwner(const SOCKET value = InvalidSocket) : value_(value) {}
	~SocketOwner() { close_socket(value_); }
	SocketOwner(const SocketOwner &) = delete;
	SocketOwner &operator=(const SocketOwner &) = delete;
	SOCKET get() const noexcept { return value_; }
	SOCKET release() noexcept { return std::exchange(value_, InvalidSocket); }

private:
	SOCKET value_;
};

QList<QHostAddress> resolve_host(const QString &hostname, const std::function<bool()> &interrupted)
{
	QHostAddress literal;
	if (literal.setAddress(hostname))
		return {literal};
	QList<QHostAddress> addresses;
	QEventLoop loop;
	QTimer timer;
	timer.setInterval(25);
	QObject::connect(&timer, &QTimer::timeout, &loop, [&] {
		if (interrupted())
			loop.quit();
	});
	const int lookup = QHostInfo::lookupHost(hostname, &loop, [&](const QHostInfo &result) {
		if (result.error() == QHostInfo::NoError)
			addresses = result.addresses();
		loop.quit();
	});
	timer.start();
	if (!interrupted())
		loop.exec();
	QHostInfo::abortHostLookup(lookup);
	// The receiver context is the local loop: destroying it removes queued
	// completions, so an aborted DNS lookup never retains stack references.
	return interrupted() ? QList<QHostAddress>{} : addresses;
}

SOCKET connect_address(const QHostAddress &address, const uint16_t port, const std::function<bool()> &interrupted)
{
	sockaddr_storage storage{};
	int length = 0;
	int family = AF_INET;
	if (address.protocol() == QAbstractSocket::IPv6Protocol) {
		family = AF_INET6;
		auto &endpoint = reinterpret_cast<sockaddr_in6 &>(storage);
		endpoint.sin6_family = AF_INET6;
		endpoint.sin6_port = htons(port);
		const Q_IPV6ADDR bytes = address.toIPv6Address();
		std::memcpy(&endpoint.sin6_addr, bytes.c, sizeof(endpoint.sin6_addr));
		endpoint.sin6_scope_id = address.scopeId().toUInt();
		length = sizeof(endpoint);
	} else if (address.protocol() == QAbstractSocket::IPv4Protocol) {
		auto &endpoint = reinterpret_cast<sockaddr_in &>(storage);
		endpoint.sin_family = AF_INET;
		endpoint.sin_port = htons(port);
		endpoint.sin_addr.s_addr = htonl(address.toIPv4Address());
		length = sizeof(endpoint);
	} else {
		return InvalidSocket;
	}
	SocketOwner socket(::socket(family, SOCK_STREAM, IPPROTO_TCP));
	if (socket.get() == InvalidSocket || !nonblocking(socket.get(), true))
		return InvalidSocket;
#ifdef SO_NOSIGPIPE
	const int noSignal = 1;
	setsockopt(socket.get(), SOL_SOCKET, SO_NOSIGPIPE, &noSignal, sizeof(noSignal));
#endif
	const int result = ::connect(socket.get(), reinterpret_cast<const sockaddr *>(&storage),
				     static_cast<SocketLength>(length));
	if (result != 0) {
#ifdef _WIN32
		const int error = WSAGetLastError();
		if (error != WSAEWOULDBLOCK && error != WSAEINPROGRESS)
			return InvalidSocket;
#else
		if (errno != EINPROGRESS)
			return InvalidSocket;
#endif
		const auto endpointDeadline = Clock::now() + 1500ms;
		for (;;) {
			if (interrupted() || Clock::now() >= endpointDeadline)
				return InvalidSocket;
#ifdef _WIN32
			fd_set writable;
			FD_ZERO(&writable);
			FD_SET(socket.get(), &writable);
			timeval timeout{0, 50'000};
			const int selected = select(0, nullptr, &writable, nullptr, &timeout);
#else
			// OBS can own descriptors above FD_SETSIZE (many media sources).
			// poll has no fixed descriptor-index limit, unlike FD_SET.
			pollfd descriptor{socket.get(), POLLOUT, 0};
			const int selected = poll(&descriptor, 1, 50);
#endif
			if (selected < 0)
				return InvalidSocket;
			if (selected == 0)
				continue;
			int error = 0;
			SocketLength size = sizeof(error);
			if (getsockopt(socket.get(), SOL_SOCKET, SO_ERROR, reinterpret_cast<char *>(&error), &size) !=
				    0 ||
			    error != 0)
				return InvalidSocket;
			break;
		}
	}
	return !interrupted() && nonblocking(socket.get(), false) ? socket.release() : InvalidSocket;
}

bool description_valid(const StreamDescription &description) noexcept
{
	return !description.videoExtraData.empty() && description.videoExtraData.size() <= 1024U * 1024U &&
	       !description.audioExtraData.empty() && description.audioExtraData.size() <= 65536U &&
	       description.width > 0 && description.width <= 16384 && description.height > 0 &&
	       description.height <= 16384 && description.sampleRate >= 8000 && description.sampleRate <= 192000 &&
	       description.channels > 0 && description.channels <= 8;
}

bool target_valid(const MultistreamTarget &target)
{
	if (target.id.empty() || target.id.size() > 128 || target.name.size() > 256 || target.server.empty() ||
	    target.server.size() > 4096 || target.key.empty() || target.key.size() > 4096)
		return false;
	if (std::any_of(target.server.begin(), target.server.end(),
			[](const unsigned char c) { return c <= 32 || c == 127; }) ||
	    std::any_of(target.key.begin(), target.key.end(), [](const unsigned char c) { return c < 32 || c == 127; }))
		return false;
	const QUrl url(QString::fromStdString(target.server), QUrl::StrictMode);
	return url.isValid() && (url.scheme() == QStringLiteral("rtmp") || url.scheme() == QStringLiteral("rtmps")) &&
	       !url.host().isEmpty() && url.userInfo().isEmpty() && !url.hasFragment() && url.port(1935) > 0 &&
	       url.port(1935) <= 65535;
}

class Destination final {
public:
	Destination(MultistreamTarget target, StreamDescription description)
		: target_(std::move(target)),
		  description_(std::move(description))
	{
		status_.id = target_.id;
		status_.name = target_.name;
		status_.state = MultistreamState::Connecting;
		status_.detail = "Connecting";
	}
	~Destination()
	{
		request_stop();
		if (thread_.joinable())
			thread_.join();
	}
	void start()
	{
		thread_ = std::thread([this] { run(); });
	}
	void request_stop() noexcept
	{
		stop_.store(true, std::memory_order_release);
		interrupt_socket();
		condition_.notify_all();
	}
	bool finished() const noexcept { return finished_.load(std::memory_order_acquire); }
	void check_deadline() noexcept
	{
		const uint64_t deadline = deadlineUsec_.load(std::memory_order_acquire);
		if (deadline != 0 && now_usec() >= deadline) {
			restart_.store(true, std::memory_order_release);
			interrupt_socket();
		}
	}
	void submit(const SharedEncodedPacket &packet) noexcept
	{
		std::scoped_lock lock(mutex_);
		if (stop_.load(std::memory_order_acquire) || restart_.load(std::memory_order_acquire) ||
		    (status_.state != MultistreamState::WaitingForKeyframe &&
		     status_.state != MultistreamState::Streaming))
			return;
		try {
			const auto result = queue_.push(packet, now_usec());
			if (result == core::MultistreamQueue::PushResult::Overflow) {
				restart_.store(true, std::memory_order_release);
				status_.state = MultistreamState::Reconnecting;
				status_.detail = "Destination too slow; reconnecting at the next delayed keyframe";
			}
		} catch (...) {
			// A destination queue allocation must never unwind through OBS's
			// encoded-packet callback or affect the other destinations.
			queue_.reset();
			restart_.store(true, std::memory_order_release);
			status_.state = MultistreamState::Reconnecting;
		}
		condition_.notify_all();
	}
	MultistreamStatus snapshot() const
	{
		std::scoped_lock lock(mutex_);
		MultistreamStatus result = status_;
		result.queuedBytes = queue_.bytes();
		result.queuedPackets = queue_.size();
		return result;
	}

private:
	bool interrupted() const noexcept
	{
		const uint64_t deadline = deadlineUsec_.load(std::memory_order_acquire);
		return stop_.load(std::memory_order_acquire) || restart_.load(std::memory_order_acquire) ||
		       (deadline != 0 && now_usec() >= deadline);
	}
	void interrupt_socket() noexcept
	{
		std::scoped_lock lock(socketMutex_);
		// This is our own duplicate handle, not librtmp's fd. Internal RTMP
		// error cleanup may close/reuse its fd without racing this shutdown.
		shutdown_socket(interruptSocket_);
	}
	void discard_interrupt_socket() noexcept
	{
		std::scoped_lock lock(socketMutex_);
		close_socket(std::exchange(interruptSocket_, InvalidSocket));
	}
	void set_status(const MultistreamState state, const char *detail)
	{
		std::scoped_lock lock(mutex_);
		status_.state = state;
		status_.detail = detail;
	}
	static int interrupt_callback(void *opaque)
	{
		return static_cast<Destination *>(opaque)->interrupted() ? 1 : 0;
	}
	static int write_callback(void *opaque, const uint8_t *bytes, const int size)
	{
		try {
			return static_cast<Destination *>(opaque)->write_flv(bytes, size);
		} catch (...) {
			return AVERROR(ENOMEM);
		}
	}
	int write_flv(const uint8_t *bytes, const int size)
	{
		if (size < 0 || interrupted() || !rtmp_)
			return AVERROR_EXIT;
		if (static_cast<std::size_t>(size) > MaxFlvTagBytes - std::min(flvPending_.size(), MaxFlvTagBytes))
			return AVERROR(ENOBUFS);
		flvPending_.insert(flvPending_.end(), bytes, bytes + size);
		std::size_t consumed = 0;
		if (!haveFlvHeader_) {
			if (flvPending_.size() < 13)
				return size;
			if (flvPending_[0] != 'F' || flvPending_[1] != 'L' || flvPending_[2] != 'V')
				return AVERROR_INVALIDDATA;
			consumed = 13;
			haveFlvHeader_ = true;
		}
		while (flvPending_.size() - consumed >= 11) {
			const uint8_t *tag = flvPending_.data() + consumed;
			const std::size_t payload = (static_cast<std::size_t>(tag[1]) << 16U) |
						    (static_cast<std::size_t>(tag[2]) << 8U) | tag[3];
			const std::size_t total = payload + 15U;
			if (total > MaxFlvTagBytes)
				return AVERROR_INVALIDDATA;
			if (flvPending_.size() - consumed < total)
				break;
			if (interrupted() || RTMP_Write(rtmp_, reinterpret_cast<const char *>(tag),
							static_cast<int>(total), 0) != static_cast<int>(total))
				return AVERROR(EIO);
			consumed += total;
		}
		if (consumed != 0)
			flvPending_.erase(flvPending_.begin(),
					  flvPending_.begin() + static_cast<std::ptrdiff_t>(consumed));
		return size;
	}
	bool connect_network()
	{
		deadlineUsec_.store(now_usec() + ConnectTimeoutUsec, std::memory_order_release);
		rtmp_ = RTMP_Alloc();
		if (!rtmp_)
			return false;
		RTMP_Init(rtmp_);
		serverBuffer_.assign(target_.server.begin(), target_.server.end());
		serverBuffer_.push_back('\0');
		if (!RTMP_SetupURL(rtmp_, serverBuffer_.data()))
			return false;
		RTMP_EnableWrite(rtmp_);
		RTMP_AddStream(rtmp_, target_.key.c_str());
		rtmp_->Link.receiveTimeout = 1;
		rtmp_->Link.sendTimeout = 1;
		const QUrl url(QString::fromStdString(target_.server), QUrl::StrictMode);
		const auto addresses = resolve_host(url.host(), [this] { return interrupted(); });
		SOCKET connected = InvalidSocket;
		for (const QHostAddress &address : addresses) {
			connected = connect_address(address, rtmp_->Link.port, [this] { return interrupted(); });
			if (connected != InvalidSocket || interrupted())
				break;
		}
		SocketOwner socket(connected);
		if (connected == InvalidSocket || interrupted())
			return false;
		{
			std::scoped_lock lock(socketMutex_);
			interruptSocket_ = duplicate_socket(connected);
			if (interruptSocket_ == InvalidSocket)
				return false;
		}
		// RTMP_Connect0 takes ownership. Certificate chain and hostname
		// verification remain required by our private mbedTLS build.
		if (!RTMP_Connect0(rtmp_, socket.release()))
			return false;
		rtmp_->m_bSendCounter = 1;
		if (interrupted() || !RTMP_Connect1(rtmp_, nullptr) || interrupted() || !RTMP_ConnectStream(rtmp_, 0))
			return false;
		return !interrupted();
	}
	bool add_stream(const bool video)
	{
		AVStream *stream = avformat_new_stream(format_, nullptr);
		if (!stream)
			return false;
		stream->time_base = {1, 1'000'000};
		AVCodecParameters *parameters = stream->codecpar;
		parameters->codec_type = video ? AVMEDIA_TYPE_VIDEO : AVMEDIA_TYPE_AUDIO;
		parameters->codec_id = video ? AV_CODEC_ID_H264 : AV_CODEC_ID_AAC;
		const auto &extra = video ? description_.videoExtraData : description_.audioExtraData;
		parameters->extradata = static_cast<uint8_t *>(av_mallocz(extra.size() + AV_INPUT_BUFFER_PADDING_SIZE));
		if (!parameters->extradata)
			return false;
		std::memcpy(parameters->extradata, extra.data(), extra.size());
		parameters->extradata_size = static_cast<int>(extra.size());
		if (video) {
			parameters->width = static_cast<int>(description_.width);
			parameters->height = static_cast<int>(description_.height);
		} else {
			parameters->sample_rate = static_cast<int>(description_.sampleRate);
			av_channel_layout_default(&parameters->ch_layout, static_cast<int>(description_.channels));
		}
		return true;
	}
	bool open_muxer()
	{
		// Credentials never enter FFmpeg, whose logger is process-global in
		// OBS. Only the private librtmp copy sees server/key, and it is quiet.
		if (avformat_alloc_output_context2(&format_, nullptr, "flv", "dynamic-delay.flv") < 0 || !format_)
			return false;
		format_->interrupt_callback = {interrupt_callback, this};
		format_->flags |= AVFMT_FLAG_CUSTOM_IO | AVFMT_FLAG_FLUSH_PACKETS;
		format_->avoid_negative_ts = AVFMT_AVOID_NEG_TS_DISABLED;
		if (!add_stream(true) || !add_stream(false))
			return false;
		packet_ = av_packet_alloc();
		if (!packet_)
			return false;
		constexpr int IoBufferBytes = 32768;
		auto *buffer = static_cast<uint8_t *>(av_malloc(IoBufferBytes));
		if (!buffer)
			return false;
		io_ = avio_alloc_context(buffer, IoBufferBytes, 1, this, nullptr, write_callback, nullptr);
		if (!io_) {
			av_free(buffer);
			return false;
		}
		io_->seekable = 0;
		format_->pb = io_;
		AVDictionary *options = nullptr;
		av_dict_set(&options, "flvflags", "no_duration_filesize", 0);
		const int result = avformat_write_header(format_, &options);
		av_dict_free(&options);
		avio_flush(io_);
		return result >= 0 && io_->error >= 0 && !interrupted();
	}
	bool send_packet(const SharedEncodedPacket &source, const int64_t epoch)
	{
		if (!source.data || source.data->size() > static_cast<std::size_t>(INT_MAX) || source.dtsUsec < epoch ||
		    source.ptsUsec < epoch)
			return false;
		const uint64_t dtsDelta = static_cast<uint64_t>(source.dtsUsec) - static_cast<uint64_t>(epoch);
		const uint64_t ptsDelta = static_cast<uint64_t>(source.ptsUsec) - static_cast<uint64_t>(epoch);
		if (dtsDelta > static_cast<uint64_t>(INT64_MAX) || ptsDelta > static_cast<uint64_t>(INT64_MAX))
			return false;
		AVPacket *packet = packet_;
		packet->data = const_cast<uint8_t *>(source.data->data());
		packet->size = static_cast<int>(source.data->size());
		packet->stream_index = source.video ? 0 : 1;
		packet->dts = static_cast<int64_t>(dtsDelta);
		packet->pts = static_cast<int64_t>(ptsDelta);
		packet->flags = source.keyframe ? AV_PKT_FLAG_KEY : 0;
		av_packet_rescale_ts(packet, AVRational{1, 1'000'000},
				     format_->streams[packet->stream_index]->time_base);
		deadlineUsec_.store(now_usec() + WriteTimeoutUsec, std::memory_order_release);
		// av_write_frame is synchronous and does not retain borrowed payloads.
		// Avoid the interleaver's private packet copies/queues for each target.
		const int result = av_write_frame(format_, packet);
		avio_flush(io_);
		packet->data = nullptr;
		packet->size = 0;
		av_packet_unref(packet);
		const bool success = result >= 0 && io_->error >= 0 && !interrupted();
		deadlineUsec_.store(0, std::memory_order_release);
		if (success) {
			std::scoped_lock lock(mutex_);
			status_.bytesSent += source.data->size();
			status_.state = MultistreamState::Streaming;
			status_.detail = "Streaming delayed output";
		}
		return success;
	}
	void close_connection() noexcept
	{
		interrupt_socket();
		av_packet_free(&packet_);
		if (format_)
			format_->pb = nullptr;
		if (io_) {
			av_freep(&io_->buffer);
			avio_context_free(&io_);
		}
		if (format_)
			avformat_free_context(std::exchange(format_, nullptr));
		if (rtmp_) {
			RTMP_Close(rtmp_);
			RTMP_Free(std::exchange(rtmp_, nullptr));
		}
		discard_interrupt_socket();
		deadlineUsec_.store(0, std::memory_order_release);
		flvPending_.clear();
		haveFlvHeader_ = false;
	}
	void run() noexcept
	{
		try {
			uint32_t backoffSeconds = 1;
			while (!stop_.load(std::memory_order_acquire)) {
				restart_.store(false, std::memory_order_release);
				set_status(MultistreamState::Connecting, "Connecting");
				if (connect_network() && open_muxer()) {
					deadlineUsec_.store(0, std::memory_order_release);
					{
						std::scoped_lock lock(mutex_);
						// Drop anything from before connection readiness. Only
						// a newly submitted delayed IDR may start this epoch.
						queue_.reset();
						status_.state = MultistreamState::WaitingForKeyframe;
						status_.detail = "Waiting for the next delayed keyframe";
					}
					const auto connectedAt = Clock::now();
					while (!interrupted()) {
						SharedEncodedPacket packet;
						int64_t epoch = 0;
						{
							std::unique_lock lock(mutex_);
							condition_.wait_for(lock, 100ms, [&] {
								return interrupted() || !queue_.empty();
							});
							if (interrupted())
								break;
							if (queue_.empty())
								continue;
							epoch = queue_.epoch_dts_usec();
							if (!queue_.pop(packet, now_usec())) {
								restart_.store(true, std::memory_order_release);
								break;
							}
						}
						if (!send_packet(packet, epoch))
							break;
						if (Clock::now() - connectedAt > 10s)
							backoffSeconds = 1;
					}
				}
				close_connection();
				{
					std::unique_lock lock(mutex_);
					queue_.reset();
					if (stop_.load(std::memory_order_acquire))
						break;
					status_.state = MultistreamState::Reconnecting;
					status_.detail =
						"Connection interrupted; retrying safely. Check server, key, and certificate if this continues";
					if (status_.reconnectAttempts < UINT32_MAX)
						++status_.reconnectAttempts;
					condition_.wait_for(lock, std::chrono::seconds(backoffSeconds),
							    [&] { return stop_.load(std::memory_order_acquire); });
				}
				backoffSeconds = std::min(backoffSeconds * 2U, 30U);
			}
			close_connection();
			set_status(MultistreamState::Stopped, "Stopped");
		} catch (...) {
			close_connection();
			try {
				set_status(MultistreamState::Error,
					   "Destination stopped after an internal transport error");
			} catch (...) {
			}
		}
		finished_.store(true, std::memory_order_release);
	}

	const MultistreamTarget target_;
	const StreamDescription description_;
	mutable std::mutex mutex_;
	std::condition_variable condition_;
	core::MultistreamQueue queue_;
	MultistreamStatus status_;
	std::thread thread_;
	std::atomic_bool stop_{false};
	std::atomic_bool restart_{false};
	std::atomic_bool finished_{false};
	std::atomic<uint64_t> deadlineUsec_{0};
	std::mutex socketMutex_;
	SOCKET interruptSocket_ = InvalidSocket;
	RTMP *rtmp_ = nullptr;
	AVFormatContext *format_ = nullptr;
	AVIOContext *io_ = nullptr;
	AVPacket *packet_ = nullptr;
	std::vector<char> serverBuffer_;
	std::vector<uint8_t> flvPending_;
	bool haveFlvHeader_ = false;
};

} // namespace

class MultistreamTransport::Impl final {
public:
	Impl()
	{
#ifdef _WIN32
		WSADATA data{};
		winsockReady_ = WSAStartup(MAKEWORD(2, 2), &data) == 0;
#endif
		static std::once_flag quietLogging;
		std::call_once(quietLogging, [] { RTMP_LogSetCallback([](int, const char *, va_list) {}); });
		watchdog_ = std::thread([this] {
			std::unique_lock lock(mutex_);
			while (!shuttingDown_) {
				if (destinations_.empty() && retired_.empty())
					wake_.wait(lock, [this] {
						return shuttingDown_ || !destinations_.empty() || !retired_.empty();
					});
				else
					wake_.wait_for(lock, 25ms);
				for (const auto &[id, destination] : destinations_) {
					(void)id;
					destination->check_deadline();
				}
				for (const auto &destination : retired_)
					destination->check_deadline();
				// finished is published after all connection cleanup; joining
				// here cannot wait for network/DNS and releases keys promptly.
				std::erase_if(retired_,
					      [](const auto &destination) { return destination->finished(); });
			}
		});
	}
	~Impl()
	{
		stop_all();
		{
			std::scoped_lock lock(mutex_);
			shuttingDown_ = true;
			wake_.notify_all();
		}
		watchdog_.join();
		retired_.clear();
#ifdef _WIN32
		if (winsockReady_)
			WSACleanup();
#endif
	}
	bool start(const MultistreamTarget &target, const StreamDescription &description, std::string &error)
	{
		if (!QCoreApplication::instance()) {
			error = "The application event system is not ready";
			return false;
		}
		if (!target_valid(target)) {
			error = "Enter a valid RTMP/RTMPS server and a separate non-empty stream key";
			return false;
		}
		if (!description_valid(description)) {
			error = "Multistream requires valid H.264 and AAC headers with audio/video dimensions";
			return false;
		}
#ifdef _WIN32
		if (!winsockReady_) {
			error = "Windows networking is unavailable";
			return false;
		}
#endif
		std::scoped_lock lock(mutex_);
		std::erase_if(retired_, [](const auto &destination) { return destination->finished(); });
		if (destinations_.contains(target.id) || destinations_.size() >= 8 ||
		    destinations_.size() + retired_.size() >= 16) {
			error = "This destination is already active, or the eight-destination limit was reached";
			return false;
		}
		try {
			auto destination = std::make_shared<Destination>(target, description);
			destinations_.emplace(target.id, destination);
			destination->start();
		} catch (...) {
			destinations_.erase(target.id);
			error = "Could not create the destination worker";
			return false;
		}
		error.clear();
		wake_.notify_all();
		return true;
	}
	void stop(const std::string &id)
	{
		std::scoped_lock lock(mutex_);
		const auto found = destinations_.find(id);
		if (found == destinations_.end())
			return;
		found->second->request_stop();
		retired_.push_back(std::move(found->second));
		destinations_.erase(found);
		std::erase_if(retired_, [](const auto &destination) { return destination->finished(); });
	}
	void stop_all()
	{
		std::scoped_lock lock(mutex_);
		for (auto &[id, destination] : destinations_) {
			(void)id;
			destination->request_stop();
			retired_.push_back(std::move(destination));
		}
		destinations_.clear();
		std::erase_if(retired_, [](const auto &destination) { return destination->finished(); });
	}
	void submit(const SharedEncodedPacket &packet)
	{
		std::scoped_lock lock(mutex_);
		for (const auto &[id, destination] : destinations_) {
			(void)id;
			destination->submit(packet);
		}
	}
	std::vector<MultistreamStatus> snapshot() const
	{
		std::scoped_lock lock(mutex_);
		std::vector<MultistreamStatus> result;
		result.reserve(destinations_.size());
		for (const auto &[id, destination] : destinations_) {
			(void)id;
			result.push_back(destination->snapshot());
		}
		return result;
	}
	bool active() const
	{
		std::scoped_lock lock(mutex_);
		return !destinations_.empty();
	}

private:
	mutable std::mutex mutex_;
	std::condition_variable wake_;
	std::unordered_map<std::string, std::shared_ptr<Destination>> destinations_;
	std::vector<std::shared_ptr<Destination>> retired_;
	std::thread watchdog_;
	bool shuttingDown_ = false;
#ifdef _WIN32
	bool winsockReady_ = false;
#endif
};

MultistreamTransport::MultistreamTransport() : impl_(std::make_unique<Impl>()) {}
MultistreamTransport::~MultistreamTransport() = default;
bool MultistreamTransport::start(const MultistreamTarget &target, const StreamDescription &description,
				 std::string &error)
{
	return impl_->start(target, description, error);
}
void MultistreamTransport::stop(const std::string &id)
{
	impl_->stop(id);
}
void MultistreamTransport::stop_all()
{
	impl_->stop_all();
}
void MultistreamTransport::submit(const SharedEncodedPacket &packet)
{
	impl_->submit(packet);
}
std::vector<MultistreamStatus> MultistreamTransport::snapshot() const
{
	return impl_->snapshot();
}
bool MultistreamTransport::active() const
{
	return impl_->active();
}
bool MultistreamTransport::has_targets() const
{
	return impl_->active();
}

} // namespace dynamic_delay
