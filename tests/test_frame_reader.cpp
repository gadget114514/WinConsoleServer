#include "frame_reader.h"

#include <cstdio>
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

wcs::Frame MakeFrame(wcs::FrameType type, std::vector<uint8_t> payload) {
    wcs::Frame frame;
    frame.type = type;
    frame.payload = std::move(payload);
    return frame;
}

void TestSplitEveryByte() {
    wcs::Frame a = MakeFrame(wcs::FrameType::Output, { 'a', 'b', 'c' });
    wcs::Frame b = MakeFrame(wcs::FrameType::Resized, { 1, 0, 2, 0, 0, 0 });
    std::vector<uint8_t> bytes = wcs::EncodeFrame(a);
    std::vector<uint8_t> bytesB = wcs::EncodeFrame(b);
    bytes.insert(bytes.end(), bytesB.begin(), bytesB.end());

    wcs::FrameReader reader;
    std::vector<wcs::Frame> frames;
    for (uint8_t byte : bytes) {
        reader.Append(&byte, 1);
        for (;;) {
            wcs::Frame frame;
            std::string error;
            wcs::ReadResult rr = reader.Next(frame, &error);
            if (rr == wcs::ReadResult::NeedMore) break;
            CHECK(rr == wcs::ReadResult::Frame);
            frames.push_back(std::move(frame));
        }
    }

    CHECK(frames.size() == 2);
    if (frames.size() == 2) {
        CHECK(frames[0].type == wcs::FrameType::Output);
        CHECK(frames[0].payload == a.payload);
        CHECK(frames[1].type == wcs::FrameType::Resized);
        CHECK(frames[1].payload == b.payload);
    }
}

void TestManyFramesOneAppend() {
    std::vector<uint8_t> bytes;
    for (int i = 0; i < 100; ++i) {
        std::vector<uint8_t> payload(3, (uint8_t)i);
        std::vector<uint8_t> encoded = wcs::EncodeFrame(
            MakeFrame(wcs::FrameType::Output, std::move(payload)));
        bytes.insert(bytes.end(), encoded.begin(), encoded.end());
    }

    wcs::FrameReader reader;
    reader.Append(bytes.data(), bytes.size());

    int count = 0;
    for (;;) {
        wcs::Frame frame;
        std::string error;
        wcs::ReadResult rr = reader.Next(frame, &error);
        if (rr == wcs::ReadResult::NeedMore) break;
        CHECK(rr == wcs::ReadResult::Frame);
        ++count;
    }
    CHECK(count == 100);
    CHECK(reader.BufferedBytes() == 0);
}

void TestOversizedFrame() {
    wcs::Frame frame = MakeFrame(wcs::FrameType::Output, std::vector<uint8_t>(128, 0));
    std::vector<uint8_t> bytes = wcs::EncodeFrame(frame);

    wcs::FrameReader reader(64);
    reader.Append(bytes.data(), bytes.size());
    wcs::Frame out;
    std::string error;
    wcs::ReadResult rr = reader.Next(out, &error);
    CHECK(rr == wcs::ReadResult::Error);
    CHECK(!error.empty());
}

void TestTruncatedHeader() {
    wcs::FrameReader reader;
    const uint8_t partial[4] = { 0, 0, 0, 0 };
    reader.Append(partial, sizeof(partial));
    wcs::Frame out;
    std::string error;
    CHECK(reader.Next(out, &error) == wcs::ReadResult::NeedMore);
}

} // namespace

int main() {
    TestSplitEveryByte();
    TestManyFramesOneAppend();
    TestOversizedFrame();
    TestTruncatedHeader();

    if (failures == 0) {
        std::printf("test_frame_reader: OK\n");
        return 0;
    }
    std::printf("test_frame_reader: %d failure(s)\n", failures);
    return 1;
}
