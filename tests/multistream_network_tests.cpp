// Real network-boundary tests. Every socket is bound to loopback; no service,
// OBS profile, credentials, or public DNS server is contacted.
#include "multistream-transport.hpp"

extern "C" {
#include <libavutil/log.h>
}

#include <QCoreApplication>
#include <QHostAddress>
#include <QSslCertificate>
#include <QSslKey>
#include <QSslSocket>
#include <QTcpServer>
#include <QTcpSocket>
#include <QThread>
#include <QTimer>

#include <array>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

namespace dynamic_delay {
void multistream_test_fail_next_worker() noexcept;
}

namespace {
using namespace std::chrono_literals;
using namespace dynamic_delay;
using Clock = std::chrono::steady_clock;
constexpr const char *Secret = "transport-test-secret-never-log-5da49";
constexpr const char *ServerMarker = "transport-test-private-app-874e1";
std::mutex logMutex;
std::string logs;

void require(const bool value, const char *message)
{
	if (!value)
		throw std::runtime_error(message);
}

void append_log(const std::string &message)
{
	std::scoped_lock lock(logMutex);
	logs.append(message);
}

void ffmpeg_log(void *, int, const char *format, va_list arguments)
{
	std::array<char, 4096> text{};
	vsnprintf(text.data(), text.size(), format, arguments);
	append_log(text.data());
}

void qt_log(QtMsgType, const QMessageLogContext &, const QString &message)
{
	append_log(message.toStdString());
}

bool wait_until(const std::function<bool()> &predicate, const std::chrono::milliseconds timeout)
{
	const auto deadline = Clock::now() + timeout;
	while (!predicate()) {
		if (Clock::now() >= deadline)
			return false;
		QCoreApplication::processEvents();
		QThread::msleep(2);
	}
	return true;
}

StreamDescription description()
{
	// Valid AVCDecoderConfigurationRecord and AAC AudioSpecificConfig. These
	// failures happen before publish, but still exercise production validation.
	StreamDescription result;
	result.videoExtraData = {1,   66, 0,   30,  255, 225, 0, 9, 103, 66,  0,  30,
				 218, 2,  128, 183, 254, 1,   0, 4, 104, 206, 60, 128};
	result.audioExtraData = {0x11, 0x90};
	result.width = 640;
	result.height = 360;
	result.sampleRate = 48000;
	result.channels = 2;
	return result;
}

MultistreamTarget target(const uint16_t port, const std::string &scheme = "rtmp")
{
	return {"test", "Local test", scheme + "://127.0.0.1:" + std::to_string(port) + "/" + ServerMarker, Secret};
}

class TcpFixture final : public QTcpServer {
public:
	explicit TcpFixture(const uint16_t port, const bool trickle = false) : trickle_(trickle)
	{
		require(listen(QHostAddress::LocalHost, port), "Cannot bind local TCP fixture");
	}
	int accepted = 0;
	int trickles = 0;

protected:
	void incomingConnection(const qintptr descriptor) override
	{
		auto *socket = new QTcpSocket(this);
		require(socket->setSocketDescriptor(descriptor), "Cannot adopt local TCP socket");
		++accepted;
		QObject::connect(socket, &QTcpSocket::readyRead, socket, [socket] { socket->readAll(); });
		if (trickle_) {
			socket->write(QByteArray(1, '\x03'));
			auto *timer = new QTimer(socket);
			timer->setInterval(100);
			QObject::connect(timer, &QTimer::timeout, socket, [this, socket] {
				if (socket->state() == QAbstractSocket::ConnectedState) {
					socket->write(QByteArray(1, '\0'));
					++trickles;
				}
			});
			timer->start();
		}
	}

private:
	bool trickle_;
};

// Deliberately public TEST-ONLY self-signed identity. It is never installed as
// a trust anchor. SAN matches loopback so the negative test checks trust.
constexpr char TestCertificate[] = R"PEM(-----BEGIN CERTIFICATE-----
MIIDGjCCAgKgAwIBAgIUco67vDid8LbAXeMUgqQtS82H63IwDQYJKoZIhvcNAQEL
BQAwFDESMBAGA1UEAwwJMTI3LjAuMC4xMB4XDTI2MDkwNTExNDIyMFoXDTM2MDkw
MjExNDIyMFowFDESMBAGA1UEAwwJMTI3LjAuMC4xMIIBIjANBgkqhkiG9w0BAQEF
AAOCAQ8AMIIBCgKCAQEA0ghqmkZoYKkMGMubuo6pN2JFGjJ+6IlIElkVsPnpdzK/
N9YIoHhzXAO/rY0nFgngqw53JK5hv5ah5vCDPuvM95wCo3LkUS4T5pu0zsINrVBX
g66Pn0Vp6ygD040alpZJXkpldK8YlVMInglIzwU+LGHn//tYTeJcTXCPY1yfIARv
qdEE6knbUWxIs9Mx/hRjjiJdFSu0Uh7aJSa0d2LuGiBSf/+bd75hRlegpNFlAcK6
JJuLucEzyIfiLiaGka8Mx8BZ9b+yYe7agl1zho5M+JqKsswVlaPgz2k1Jk3N6DZR
EYKnJQQFbq46Pf/w4Wvm601aN7NpfxonD5PXfQVUiQIDAQABo2QwYjAdBgNVHQ4E
FgQU70o826LnuPUvOVy/XkFJzvy7XgcwHwYDVR0jBBgwFoAU70o826LnuPUvOVy/
XkFJzvy7XgcwDwYDVR0TAQH/BAUwAwEB/zAPBgNVHREECDAGhwR/AAABMA0GCSqG
SIb3DQEBCwUAA4IBAQCdkM68pc+XQCHeTAb4rJeB3kOHJUZrXu7rnLpexhGCRWsW
X/w+mxdLAnRzND7KpQmTOWdSTEabCzTd05EF5NEdvf99J+eQ6X1j3nVGSYl+Fpv+
ZtH7MLaRVb1MyCK9dlJTzpYTQFBo1sieQV9xJfi8DtL2Gc5Aha373yuAkFXKQ8qO
shE7Hmf8PzileXg7XrnOjGM5/N0rPC1m2qmt8HxF1t2Lq8TI1N1Vl1GJg+Gipo4K
ZhpIwdHqyDq6jBbpIIAEhu7xDZ53b2G7yCslhWhTnJ0JQc9rJizKMuveTwC3CH/J
hFYtOZlJtc9Va41M0CwDIP6JFeu/0kqycNFkivAg
-----END CERTIFICATE-----
)PEM";
constexpr char TestPrivateKey[] = R"PEM(-----BEGIN PRIVATE KEY-----
MIIEvgIBADANBgkqhkiG9w0BAQEFAASCBKgwggSkAgEAAoIBAQDSCGqaRmhgqQwY
y5u6jqk3YkUaMn7oiUgSWRWw+el3Mr831gigeHNcA7+tjScWCeCrDnckrmG/lqHm
8IM+68z3nAKjcuRRLhPmm7TOwg2tUFeDro+fRWnrKAPTjRqWlkleSmV0rxiVUwie
CUjPBT4sYef/+1hN4lxNcI9jXJ8gBG+p0QTqSdtRbEiz0zH+FGOOIl0VK7RSHtol
JrR3Yu4aIFJ//5t3vmFGV6Ck0WUBwrokm4u5wTPIh+IuJoaRrwzHwFn1v7Jh7tqC
XXOGjkz4moqyzBWVo+DPaTUmTc3oNlERgqclBAVurjo9//Dha+brTVo3s2l/GicP
k9d9BVSJAgMBAAECggEAHbbOnBpjaG9qFHTzyHIn6vu2YoUr0qqfXWcojDPjDXfK
uTPI3ykDSwyFuOAsHDBUDmc6wGACWLhPGezDaKq0AgDal7Soq/p8msbDDvBBvpgL
9YeD7qjDmh78YZjIHu9OnG4iid4+uFt5r+AI4q7vc5h7Wcn9nXEtlVAcHRbMtVrh
nrScpVNJs96dFMNVVGbJjXE/OlwIAEm1761rP6aUQFw19ArsZXv/uHaydbEFHHL0
T2S/aytBpT2DFKGhHIzUoEDFfi3nSphHIXqYlc+TJ0qeCvdVDrMVaKyH/p5mFMNs
SQA7MGZDr2vG/uJxjCAuFSSNZNysyTcyqPL6zqoVPQKBgQDz1uTVdvwY8o3C9jQ7
ddjLJTcpFlDo87e/FNYnzBFJXgkCcg4cstUMluXSPVzYjvtCQnXV/OtBVFA5Gyh9
8DeoofaYfjC3WPNBm97q3lxCZ1mqSXEDzh2jpQBx7BdphM2WTlyXcmQ1MGzUwBQD
YeEgtNqC0jJyDt1uD/2ndrrJjQKBgQDcgenEQN4dSdeA9eajUhd+3RGleQgwudY5
1QSEtyQdtoIBfOvX9Fe70AG+RMpgDRZDIV4PzmQuni5xgq+SMH+ikr4zY+4dQHNt
uU9Wnzs60GDAOpmyZ3EkLX8TCDnkqBAo6RzuQ7FEnEjvvcl1JBpRG0hlT6zyRzqQ
x33XdYfx7QKBgAqJbroKujoZwSZ7nSY0oXml0gxkenqWjYokTFzL5LNW7Oy+IP38
fFhe7O9411pEU3c5h/4HP+NC0XSmR8mpZ75RwWY1jcVegqSDJ87ebP4xmR5Srh+L
+JvptWs22IwmwPuNx1KEDvB9dzZ6VmMPB7tyFT8x9hwXqFnpNvzVlJzBAoGBAJsS
fdy3pbzBNl4KEMXgsIdWXteZE2p49rA7H29aAHso46q6OH3p5108fk1ZwVzlNzfE
morRIeEq+wx21JQhqVEik8I+T7GgpsyOWr5XQucsri3hyD8PwiCoIkq3KUel7Z9n
uSHS1zKiGiUHukq4Ng29+x7Mdpr3/rbcpKJGlHFNAoGBANz/kZAHKvtA+zyqj8iy
i6V3Q8LRUEh7Kq2+2YhYTiMtTZ5geiyOsN0BBDjsLLnGXUuIHps2589oYSKsj/XU
xl5smCuTZQ1rQkRedGs3ckn90tBNW7M1VlzOkXl5aSQ24OgvuVtekyLvO7XYi78d
JFuof2UVfXPg+pkVLlPsOpgB
-----END PRIVATE KEY-----
)PEM";

class TlsFixture final : public QTcpServer {
public:
	explicit TlsFixture(const uint16_t port)
	{
		require(QSslSocket::supportsSsl(), "Qt TLS backend is required for the untrusted-certificate test");
		require(listen(QHostAddress::LocalHost, port), "Cannot bind local TLS fixture");
	}
	int accepted = 0;
	int encrypted = 0;
	int handshakeErrors = 0;
	qint64 plaintextBytes = 0;

protected:
	void incomingConnection(const qintptr descriptor) override
	{
		auto *socket = new QSslSocket(this);
		socket->setLocalCertificate(QSslCertificate(QByteArray(TestCertificate)));
		socket->setPrivateKey(QSslKey(QByteArray(TestPrivateKey), QSsl::Rsa));
		socket->setPeerVerifyMode(QSslSocket::VerifyNone);
		require(!socket->localCertificate().isNull() && !socket->privateKey().isNull(),
			"Invalid test-only TLS identity");
		require(socket->setSocketDescriptor(descriptor), "Cannot adopt local TLS socket");
		++accepted;
		QObject::connect(socket, &QSslSocket::encrypted, socket, [this] { ++encrypted; });
		QObject::connect(socket, &QSslSocket::errorOccurred, socket,
				 [this](QAbstractSocket::SocketError) { ++handshakeErrors; });
		QObject::connect(socket, &QSslSocket::readyRead, socket,
				 [this, socket] { plaintextBytes += socket->readAll().size(); });
		socket->startServerEncryption();
	}
};

void invalid_inputs()
{
	MultistreamTransport transport;
	std::string error;
	for (const auto &server :
	     {"https://127.0.0.1/live", "rtmp://user:secret@127.0.0.1/live", "rtmp://127.0.0.1/live injected=option",
	      "rtmp://127.0.0.1:99999/live", "rtmp://127.0.0.1/live#fragment"}) {
		auto invalid = target(19401);
		invalid.server = server;
		require(!transport.start(invalid, description(), error), "Invalid server was accepted");
		require(!error.empty(), "Missing safe input error");
		append_log(error);
	}
	auto invalid = target(19401);
	invalid.key = std::string(Secret) + '\n';
	require(!transport.start(invalid, description(), error), "Control character accepted in stream key");
	auto headers = description();
	headers.videoExtraData.clear();
	require(!transport.start(target(19401), headers, error), "Missing video headers accepted");
	require(!transport.active() && !transport.has_targets() && transport.snapshot().empty(),
		"Validation created an active destination");
}

void silent_peer_stop()
{
	TcpFixture server(19401);
	auto transport = std::make_unique<MultistreamTransport>();
	std::string error;
	require(transport->start(target(19401), description(), error), "Silent-peer destination start failed");
	require(wait_until([&] { return server.accepted > 0; }, 3000ms), "Silent peer was never connected");
	require(!transport->start(target(19401), description(), error), "Duplicate destination ID accepted");
	const auto stopAt = Clock::now();
	transport->stop("test");
	require(Clock::now() - stopAt < 100ms, "stop blocked the caller during RTMP handshake");
	require(!transport->active(), "Stopped target remained active");
	const auto destroyAt = Clock::now();
	transport.reset();
	require(Clock::now() - destroyAt < 1500ms, "Destructor did not cancel a blocked handshake promptly");
}

void trickle_deadline()
{
	TcpFixture server(19402, true);
	MultistreamTransport transport;
	std::string error;
	require(transport.start(target(19402), description(), error), "Trickle destination start failed");
	const auto started = Clock::now();
	require(wait_until(
			[&] {
				const auto states = transport.snapshot();
				return !states.empty() && states.front().reconnectAttempts >= 1;
			},
			6500ms),
		"Slow-trickle handshake escaped the absolute connect deadline");
	require(server.trickles >= 10, "Peer did not sustain enough traffic to exercise the absolute watchdog");
	require(Clock::now() - started < 6500ms, "Trickle timeout exceeded the bounded deadline");
	const auto state = transport.snapshot().front();
	require(state.bytesSent == 0 && state.queuedBytes == 0, "Failed handshake retained or published media");
	append_log(state.detail);
	transport.stop_all();
}

void untrusted_tls()
{
	TlsFixture server(19403);
	MultistreamTransport transport;
	std::string error;
	require(transport.start(target(19403, "rtmps"), description(), error), "TLS destination start failed");
	require(wait_until(
			[&] {
				const auto states = transport.snapshot();
				return !states.empty() && states.front().reconnectAttempts >= 1;
			},
			6500ms),
		"Untrusted TLS handshake was not rejected within its deadline");
	require(server.accepted > 0, "TLS test never connected to the local certificate fixture");
	require(server.encrypted == 0 && server.plaintextBytes == 0,
		"Untrusted certificate reached an encrypted RTMP application session");
	require(wait_until([&] { return server.handshakeErrors > 0; }, 1000ms),
		"Local TLS fixture did not observe handshake rejection");
	const auto state = transport.snapshot().front();
	require(state.bytesSent == 0 && state.state != MultistreamState::Streaming,
		"Untrusted certificate published media");
	append_log(state.detail);
	transport.stop_all();
}

void terminal_error_can_restart()
{
	TcpFixture server(19404);
	MultistreamTransport transport;
	std::string error;
	multistream_test_fail_next_worker();
	require(transport.start(target(19404), description(), error), "Fault-injected worker did not start");
	require(wait_until(
			[&] {
				const auto states = transport.snapshot();
				return !states.empty() && states.front().state == MultistreamState::Error &&
				       !transport.active() && !transport.has_targets();
			},
			1000ms),
		"Terminal worker retained active capture demand");
	require(server.accepted == 0, "Fault injection unexpectedly opened a network connection");
	require(transport.start(target(19404), description(), error),
		"Terminal error prevented restarting the same ID");
	require(wait_until([&] { return server.accepted > 0; }, 3000ms), "Restarted destination never connected");
	require(transport.snapshot().size() == 1, "Restart retained a duplicate terminal diagnostic");
	transport.stop_all();
	require(transport.snapshot().empty(), "stop_all retained terminal diagnostics");
}

void logs_are_redacted()
{
	std::scoped_lock lock(logMutex);
	require(logs.find(Secret) == std::string::npos, "Stream key leaked into a diagnostic");
	require(logs.find(ServerMarker) == std::string::npos, "Private server path leaked into a diagnostic");
}
} // namespace

int main(int argc, char **argv)
{
	QCoreApplication application(argc, argv);
	if (argc > 1)
		QCoreApplication::addLibraryPath(QString::fromLocal8Bit(argv[1]));
	const auto previousQtHandler = qInstallMessageHandler(qt_log);
	av_log_set_callback(ffmpeg_log); // Isolated test process; production never changes this global callback.
	int failures = 0;
	const std::array<std::pair<const char *, void (*)()>, 6> tests = {{
		{"invalid inputs", invalid_inputs},
		{"silent peer / bounded stop", silent_peer_stop},
		{"slow-trickle absolute deadline", trickle_deadline},
		{"untrusted RTMPS certificate", untrusted_tls},
		{"terminal error / restart same ID", terminal_error_can_restart},
		{"secret-safe diagnostics", logs_are_redacted},
	}};
	for (const auto &[name, test] : tests) {
		try {
			test();
			std::cout << "PASS " << name << '\n';
		} catch (const std::exception &error) {
			++failures;
			std::cerr << "FAIL " << name << ": " << error.what() << '\n';
		}
	}
	av_log_set_callback(av_log_default_callback);
	qInstallMessageHandler(previousQtHandler);
	return failures == 0 ? 0 : 1;
}
