#include "protocol.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {
int failures = 0;

#define CHECK(cond) do { \
    if (!(cond)) { \
        std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        ++failures; \
    } \
} while (0)

void TestHelloRoundTrip() {
    wcs::Hello hello;
    hello.major = 1;
    hello.minor = 0;
    hello.features = wcs::kDefaultFeatures;
    hello.cols = 120;
    hello.rows = 40;
    hello.name = "test-client";
    hello.shell = "bash";
    hello.token = "secret";
    hello.client_id = "abc";
    hello.env.push_back("TERM=xterm-256color");

    std::vector<uint8_t> bytes = wcs::EncodeHello(hello);
    wcs::Hello out;
    std::string error;
    CHECK(wcs::DecodeHello(bytes.data(), bytes.size(), out, &error));
    CHECK(out.major == hello.major);
    CHECK(out.minor == hello.minor);
    CHECK(out.features == hello.features);
    CHECK(out.cols == hello.cols);
    CHECK(out.rows == hello.rows);
    CHECK(out.name == hello.name);
    CHECK(out.shell == hello.shell);
    CHECK(out.token == hello.token);
    CHECK(out.client_id == hello.client_id);
    CHECK(out.env.size() == 1 && out.env[0] == hello.env[0]);
}

void TestHelloAckRoundTrip() {
    wcs::HelloAck ack;
    ack.cols = 160;
    ack.rows = 50;
    ack.session_id = 42;
    ack.shell = "cmd";
    ack.features = wcs::FeatureResize;

    std::vector<uint8_t> bytes = wcs::EncodeHelloAck(ack);
    wcs::HelloAck out;
    std::string error;
    CHECK(wcs::DecodeHelloAck(bytes.data(), bytes.size(), out, &error));
    CHECK(out.cols == ack.cols);
    CHECK(out.rows == ack.rows);
    CHECK(out.session_id == ack.session_id);
    CHECK(out.shell == ack.shell);
    CHECK(out.features == ack.features);
}

void TestFrameRoundTrip() {
    wcs::Frame frame;
    frame.type = wcs::FrameType::Output;
    frame.flags = wcs::FlagPriority;
    frame.stream_id = 7;
    frame.payload = { 0x1b, '[', '3', '1', 'm', 'x' };

    std::vector<uint8_t> bytes = wcs::EncodeFrame(frame);
    CHECK(bytes.size() == 8 + frame.payload.size());

    uint32_t len = 0;
    wcs::FrameType type;
    uint8_t flags = 0;
    uint16_t stream = 0;
    CHECK(wcs::DecodeHeader(bytes.data(), bytes.size(), len, type, flags, stream));
    CHECK(len == frame.payload.size());
    CHECK(type == frame.type);
    CHECK(flags == frame.flags);
    CHECK(stream == frame.stream_id);
    CHECK(std::memcmp(bytes.data() + 8, frame.payload.data(), frame.payload.size()) == 0);
}

void TestSmallMessages() {
    wcs::ResizeInfo resize{ 100, 30 };
    auto rb = wcs::EncodeResize(resize);
    wcs::ResizeInfo rout;
    CHECK(wcs::DecodeResize(rb.data(), rb.size(), rout));
    CHECK(rout.cols == 100 && rout.rows == 30);

    wcs::ResizedInfo resized{ 100, 30, wcs::ResizeReason::Clamped };
    auto zb = wcs::EncodeResized(resized);
    wcs::ResizedInfo zout;
    CHECK(wcs::DecodeResized(zb.data(), zb.size(), zout));
    CHECK(zout.reason == wcs::ResizeReason::Clamped);

    wcs::ResizedInfo peer{ 120, 40, wcs::ResizeReason::Peer };
    auto pb2 = wcs::EncodeResized(peer);
    CHECK(wcs::DecodeResized(pb2.data(), pb2.size(), zout));
    CHECK(zout.cols == 120 && zout.rows == 40);
    CHECK(zout.reason == wcs::ResizeReason::Peer);
    CHECK(wcs::kProtocolMinor == 2);
    CHECK((wcs::kDefaultFeatures & wcs::FeatureSizeSync) != 0);

    wcs::ExitInfo exit{ wcs::ExitReason::ChildExited, 7 };
    auto eb = wcs::EncodeExit(exit);
    wcs::ExitInfo eout;
    CHECK(wcs::DecodeExit(eb.data(), eb.size(), eout));
    CHECK(eout.reason == wcs::ExitReason::ChildExited);
    CHECK(eout.exit_code == 7);

    wcs::ErrorInfo err{ wcs::ErrorCode::AuthFailed, "bad token" };
    auto xb = wcs::EncodeError(err);
    wcs::ErrorInfo xout;
    CHECK(wcs::DecodeError(xb.data(), xb.size(), xout));
    CHECK(xout.code == wcs::ErrorCode::AuthFailed);
    CHECK(xout.message == "bad token");

    auto pb = wcs::EncodePing(0x1122334455667788ull);
    uint64_t nonce = 0;
    CHECK(wcs::DecodePing(pb.data(), pb.size(), nonce));
    CHECK(nonce == 0x1122334455667788ull);
}

void TestSessionListRoundTrip() {
    std::vector<wcs::SessionInfo> sessions;
    sessions.push_back({ 7, 0, "wcsp-powershell", "powershell.exe" });
    sessions.push_back({ 3, 1, "wcsp-cmd", "cmd.exe" });

    auto bytes = wcs::EncodeSessionList(sessions);
    std::vector<wcs::SessionInfo> out;
    std::string error;
    CHECK(wcs::DecodeSessionList(bytes.data(), bytes.size(), out, &error));
    CHECK(out.size() == 2);
    CHECK(out[0].id == 7 && out[0].state == 0);
    CHECK(out[0].name == "wcsp-powershell");
    CHECK(out[0].command == "powershell.exe");
    CHECK(out[1].id == 3 && out[1].state == 1);
    CHECK(out[1].name == "wcsp-cmd");

    std::vector<wcs::SessionInfo> empty;
    auto emptyBytes = wcs::EncodeSessionList(empty);
    CHECK(wcs::DecodeSessionList(emptyBytes.data(), emptyBytes.size(), out, &error));
    CHECK(out.empty());

    auto truncated = bytes;
    truncated.resize(truncated.size() - 3);
    CHECK(!wcs::DecodeSessionList(truncated.data(), truncated.size(), out, &error));
}

void TestBadInput() {
    wcs::Hello out;
    std::string error;
    std::vector<uint8_t> empty;
    CHECK(!wcs::DecodeHello(empty.data(), empty.size(), out, &error));

    std::vector<uint8_t> truncated = wcs::EncodeHello(wcs::Hello{});
    truncated.resize(truncated.size() / 2);
    CHECK(!wcs::DecodeHello(truncated.data(), truncated.size(), out, &error));
}

} // namespace

int main() {
    TestHelloRoundTrip();
    TestHelloAckRoundTrip();
    TestFrameRoundTrip();
    TestSmallMessages();
    TestSessionListRoundTrip();
    TestBadInput();

    if (failures == 0) {
        std::printf("test_protocol: OK\n");
        return 0;
    }
    std::printf("test_protocol: %d failure(s)\n", failures);
    return 1;
}
