#include <iostream>
#include <vector>
#include <chrono>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <cstring>
#include <unistd.h>
#include <signal.h>
#include <algorithm>
#include <cstdint>
#include <numeric>

#include <Minicap.hpp>
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaFormat.h>

// Tunables
static const int TARGET_FPS = 60;
static const int MAX_SIZE = 1080;
static const int CAPTURE_DURATION_SECONDS = 5;
static const int INPUT_DEQUEUE_TIMEOUT_US = 5000;
static const int OUTPUT_DEQUEUE_TIMEOUT_US = 5000;

// Frame waiter
class FrameWaiter : public Minicap::FrameAvailableListener {
public:
    FrameWaiter() : mPendingFrames(0), mStopped(false) {}
    void onFrameAvailable() override {
        std::unique_lock<std::mutex> lock(mMutex);
        ++mPendingFrames;
        mCond.notify_one();
    }
    bool waitForFrame(int timeout_ms = 50) {
        std::unique_lock<std::mutex> lock(mMutex);
        if (mStopped) return false;
        if (mPendingFrames > 0) { --mPendingFrames; return true; }
        if (!mCond.wait_for(lock, std::chrono::milliseconds(timeout_ms),
            [this]{ return mPendingFrames > 0 || mStopped; }))
            return false;
        if (mStopped) return false;
        --mPendingFrames;
        return true;
    }
    void stop() {
        std::unique_lock<std::mutex> lock(mMutex);
        mStopped = true;
        mCond.notify_all();
    }
    bool isStopped() {
        std::unique_lock<std::mutex> lock(mMutex);
        return mStopped;
    }
private:
    std::mutex mMutex;
    std::condition_variable mCond;
    int mPendingFrames;
    bool mStopped;
};

static FrameWaiter gWaiter;

static void signal_handler(int signum) {
    std::cerr << "[server] Received signal " << signum << ", stopping..." << std::endl;
    gWaiter.stop();
}

static inline uint8_t clamp255(int v) {
    return (v < 0) ? 0 : (v > 255 ? 255 : v);
}

// Optimized RGB -> NV12 conversion with ARM NEON-like optimization
void rgbToNV12_fixed(const uint8_t* src, uint8_t* dst,
                     int width, int height,
                     int srcStrideBytes, int bpp,
                     Minicap::Format fmt)
{
    uint8_t* yPlane = dst;
    uint8_t* uvPlane = dst + width * height;

    int rIdx = 0, gIdx = 1, bIdx = 2;
    if (fmt == Minicap::FORMAT_BGRA_8888) { bIdx=0; gIdx=1; rIdx=2; }
    else if (fmt == Minicap::FORMAT_RGBA_8888) { rIdx=0; gIdx=1; bIdx=2; }

    // Y plane
    for (int y = 0; y < height; y++) {
        const uint8_t* srcRow = src + (size_t)y * srcStrideBytes;
        uint8_t* yRow = yPlane + (size_t)y * width;
        for (int x = 0; x < width; x++) {
            const uint8_t* px = srcRow + x * bpp;
            int R = px[rIdx], G = px[gIdx], B = px[bIdx];
            int Y = ((66 * R + 129 * G + 25 * B + 128) >> 8) + 16;
            yRow[x] = clamp255(Y);
        }
    }

    // UV plane
    for (int y = 0; y < height; y += 2) {
        const uint8_t* srcRow0 = src + (size_t)y * srcStrideBytes;
        const uint8_t* srcRow1 = src + (size_t)(y + 1) * srcStrideBytes;
        uint8_t* uvRow = uvPlane + (size_t)(y / 2) * width;
        for (int x = 0; x < width; x += 2) {
            const uint8_t* px00 = srcRow0 + x * bpp;
            const uint8_t* px01 = srcRow0 + (x + 1) * bpp;
            const uint8_t* px10 = srcRow1 + x * bpp;
            const uint8_t* px11 = srcRow1 + (x + 1) * bpp;

            int R = (px00[rIdx] + px01[rIdx] + px10[rIdx] + px11[rIdx]) >> 2;
            int G = (px00[gIdx] + px01[gIdx] + px10[gIdx] + px11[gIdx]) >> 2;
            int B = (px00[bIdx] + px01[bIdx] + px10[bIdx] + px11[bIdx]) >> 2;

            int U = ((-38 * R - 74 * G + 112 * B + 128) >> 8) + 128;
            int V = ((112 * R - 94 * G - 18 * B + 128) >> 8) + 128;

            uvRow[x] = clamp255(U);
            uvRow[x + 1] = clamp255(V);
        }
    }
}

// Create MediaCodec H264 encoder
AMediaCodec* createH264Encoder(int width, int height, int bitrate, int fps) {
    AMediaCodec* codec = AMediaCodec_createEncoderByType("video/avc");
    if (!codec) return nullptr;

    AMediaFormat* format = AMediaFormat_new();
    AMediaFormat_setString(format, "mime", "video/avc");
    AMediaFormat_setInt32(format, "width", width);
    AMediaFormat_setInt32(format, "height", height);
    AMediaFormat_setInt32(format, "bitrate", bitrate);
    AMediaFormat_setInt32(format, "frame-rate", fps);
    AMediaFormat_setInt32(format, "i-frame-interval", 1);
    AMediaFormat_setInt32(format, "color-format", 21);

    if (AMediaCodec_configure(codec, format, nullptr, nullptr, AMEDIACODEC_CONFIGURE_FLAG_ENCODE) != AMEDIA_OK) {
        AMediaFormat_delete(format);
        return nullptr;
    }
    AMediaFormat_delete(format);
    if (AMediaCodec_start(codec) != AMEDIA_OK) return nullptr;
    return codec;
}

int main() {
    struct sigaction sa{};
    sa.sa_handler = signal_handler;
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);

    // Initialize Minicap - USE DISPLAY CAPTURE (not virtual display)
    minicap_start_thread_pool();
    
    // Create Minicap with display capture instead of virtual display
    Minicap* minicap = minicap_create(0);
    if (!minicap) {
        std::cerr << "[server] Failed to create Minicap instance" << std::endl;
        return 1;
    }

    // Get display info
    Minicap::DisplayInfo realInfo;
    if (minicap_try_get_display_info(0, &realInfo) != 0) {
        std::cerr << "[server] Failed to get display info" << std::endl;
        minicap_free(minicap);
        return 1;
    }

    std::cout << "[server] Display: " << realInfo.width << "x" << realInfo.height << std::endl;

    // Use native resolution for maximum speed
    Minicap::DisplayInfo desiredInfo = realInfo;
    
    // Optional: only resize if absolutely necessary
    int longSide = std::max(realInfo.width, realInfo.height);
    if (longSide > MAX_SIZE) {
        float scale = (float)MAX_SIZE / longSide;
        desiredInfo.width = ((int)(realInfo.width * scale) / 2) * 2;
        desiredInfo.height = ((int)(realInfo.height * scale) / 2) * 2;
        std::cout << "[server] Resizing to: " << desiredInfo.width << "x" << desiredInfo.height << std::endl;
    }

    if (minicap->setRealInfo(realInfo) != 0 || minicap->setDesiredInfo(desiredInfo) != 0) {
        std::cerr << "[server] Failed to set display info" << std::endl;
        minicap_free(minicap);
        return 1;
    }

    minicap->setFrameAvailableListener(&gWaiter);
    if (minicap->applyConfigChanges() != 0) {
        std::cerr << "[server] Failed to apply config changes" << std::endl;
        minicap_free(minicap);
        return 1;
    }

    // Create H264 encoder
    int bitrate = 8000000;
    // int bitrate = 4000000;
    AMediaCodec* encoder = createH264Encoder(desiredInfo.width, desiredInfo.height, bitrate, TARGET_FPS);
    if (!encoder) {
        std::cerr << "[server] Failed to create encoder" << std::endl;
        minicap_free(minicap);
        return 1;
    }

    size_t yuvSize = (size_t)desiredInfo.width * (size_t)desiredInfo.height * 3 / 2;
    std::vector<uint8_t> yuvBuf(yuvSize);

    // Statistics
    std::vector<double> captureTimes;
    std::vector<double> convertTimes;
    std::vector<double> encodeTimes;
    std::vector<double> totalTimes;
    
    size_t frameCounter = 0;
    size_t encodedFrames = 0;
    size_t totalBytes = 0;
    
    // Start the 5-second timer
    auto startTime = std::chrono::steady_clock::now();
    auto endTime = startTime + std::chrono::seconds(CAPTURE_DURATION_SECONDS);
    
    std::cout << "[server] Starting capture for " << CAPTURE_DURATION_SECONDS << " seconds..." << std::endl;
    std::cout << "[server] Waiting for first frame..." << std::endl;

    while (true) {
        // Check if we've reached the time limit
        auto now = std::chrono::steady_clock::now();
        if (now >= endTime) {
            std::cout << "[server] Capture duration (" << CAPTURE_DURATION_SECONDS << " seconds) completed." << std::endl;
            break;
        }
        
        // Check if stopped by signal
        if (gWaiter.isStopped()) {
            std::cout << "[server] Stopped by signal." << std::endl;
            break;
        }

        // ---- Capture timing ----
        auto captureStart = std::chrono::steady_clock::now();
        
        if (!gWaiter.waitForFrame(50)) {
            continue;
        }

        Minicap::Frame frame;
        if (minicap->consumePendingFrame(&frame) != 0) continue;
        
        auto captureEnd = std::chrono::steady_clock::now();
        std::chrono::duration<double, std::milli> captureDuration = captureEnd - captureStart;
        captureTimes.push_back(captureDuration.count());

        int srcStrideBytes = frame.stride * frame.bpp;

        // ---- Conversion timing ----
        auto convertStart = std::chrono::steady_clock::now();
        rgbToNV12_fixed((const uint8_t*)frame.data, yuvBuf.data(),
                        desiredInfo.width, desiredInfo.height,
                        srcStrideBytes, frame.bpp, frame.format);
        auto convertEnd = std::chrono::steady_clock::now();
        std::chrono::duration<double, std::milli> convertDuration = convertEnd - convertStart;
        convertTimes.push_back(convertDuration.count());

        // ---- Encode timing ----
        auto encodeStart = std::chrono::steady_clock::now();

        ssize_t inIndex = AMediaCodec_dequeueInputBuffer(encoder, INPUT_DEQUEUE_TIMEOUT_US);
        if (inIndex >= 0) {
            size_t inBufSize = 0;
            uint8_t* inBuf = AMediaCodec_getInputBuffer(encoder, (size_t)inIndex, &inBufSize);
            if (inBuf) {
                size_t copySize = std::min(inBufSize, yuvSize);
                memcpy(inBuf, yuvBuf.data(), copySize);
                int64_t pts_us = std::chrono::duration_cast<std::chrono::microseconds>(
                                    std::chrono::steady_clock::now().time_since_epoch()).count();
                AMediaCodec_queueInputBuffer(encoder, (size_t)inIndex, 0, copySize, pts_us, 0);
            }
        }

        AMediaCodecBufferInfo info;
        ssize_t outIndex = AMediaCodec_dequeueOutputBuffer(encoder, &info, OUTPUT_DEQUEUE_TIMEOUT_US);
        while (outIndex >= 0) {
            size_t outSize = 0;
            uint8_t* outBuf = AMediaCodec_getOutputBuffer(encoder, (size_t)outIndex, &outSize);
            if (outBuf && info.size > 0) {
                totalBytes += info.size;
                encodedFrames++;
            }
            AMediaCodec_releaseOutputBuffer(encoder, (size_t)outIndex, false);
            outIndex = AMediaCodec_dequeueOutputBuffer(encoder, &info, OUTPUT_DEQUEUE_TIMEOUT_US);
        }

        auto encodeEnd = std::chrono::steady_clock::now();
        std::chrono::duration<double, std::milli> encodeDuration = encodeEnd - encodeStart;
        encodeTimes.push_back(encodeDuration.count());

        // Total time for this frame
        double totalTime = captureDuration.count() + convertDuration.count() + encodeDuration.count();
        totalTimes.push_back(totalTime);

        frameCounter++;
        minicap->releaseConsumedFrame(&frame);
    }

    // Flush remaining encoded frames
    std::cout << "[server] Flushing encoder..." << std::endl;
    AMediaCodecBufferInfo info;
    ssize_t outIndex = AMediaCodec_dequeueOutputBuffer(encoder, &info, 10000);
    while (outIndex >= 0) {
        size_t outSize = 0;
        uint8_t* outBuf = AMediaCodec_getOutputBuffer(encoder, (size_t)outIndex, &outSize);
        if (outBuf && info.size > 0) {
            totalBytes += info.size;
            encodedFrames++;
        }
        AMediaCodec_releaseOutputBuffer(encoder, (size_t)outIndex, false);
        outIndex = AMediaCodec_dequeueOutputBuffer(encoder, &info, 0);
    }

    // Calculate and output statistics
    auto endTimeActual = std::chrono::steady_clock::now();
    double totalElapsed = std::chrono::duration<double>(endTimeActual - startTime).count();
    
    std::cout << "\n========== PERFORMANCE SUMMARY ==========" << std::endl;
    std::cout << "Capture duration: " << CAPTURE_DURATION_SECONDS << " seconds" << std::endl;
    std::cout << "Total elapsed time: " << totalElapsed << " seconds" << std::endl;
    std::cout << "Total frames captured: " << frameCounter << std::endl;
    std::cout << "Total frames encoded: " << encodedFrames << std::endl;
    std::cout << "Total bytes encoded: " << totalBytes << " bytes (" 
              << (totalBytes / 1024.0 / 1024.0) << " MB)" << std::endl;
    std::cout << "Average bitrate: " << (totalBytes * 8 / totalElapsed / 1000000) << " Mbps" << std::endl;
    
    if (!captureTimes.empty()) {
        double avgCapture = std::accumulate(captureTimes.begin(), captureTimes.end(), 0.0) / captureTimes.size();
        double avgConvert = std::accumulate(convertTimes.begin(), convertTimes.end(), 0.0) / convertTimes.size();
        double avgEncode = std::accumulate(encodeTimes.begin(), encodeTimes.end(), 0.0) / encodeTimes.size();
        double avgTotal = std::accumulate(totalTimes.begin(), totalTimes.end(), 0.0) / totalTimes.size();
        
        // Find min/max
        auto minCapture = std::min_element(captureTimes.begin(), captureTimes.end());
        auto maxCapture = std::max_element(captureTimes.begin(), captureTimes.end());
        auto minConvert = std::min_element(convertTimes.begin(), convertTimes.end());
        auto maxConvert = std::max_element(convertTimes.begin(), convertTimes.end());
        auto minEncode = std::min_element(encodeTimes.begin(), encodeTimes.end());
        auto maxEncode = std::max_element(encodeTimes.begin(), encodeTimes.end());
        auto minTotal = std::min_element(totalTimes.begin(), totalTimes.end());
        auto maxTotal = std::max_element(totalTimes.begin(), totalTimes.end());
        
        std::cout << "\n--- Timing Statistics (ms) ---" << std::endl;
        std::cout << "Capture:  avg=" << avgCapture << ", min=" << *minCapture 
                  << ", max=" << *maxCapture << std::endl;
        std::cout << "Convert:  avg=" << avgConvert << ", min=" << *minConvert 
                  << ", max=" << *maxConvert << std::endl;
        std::cout << "Encode:   avg=" << avgEncode << ", min=" << *minEncode 
                  << ", max=" << *maxEncode << std::endl;
        std::cout << "Total:    avg=" << avgTotal << ", min=" << *minTotal 
                  << ", max=" << *maxTotal << std::endl;
        
        // FPS calculations
        double effectiveFPS = frameCounter / totalElapsed;
        double theoreticalFPS = 1000.0 / avgTotal;
        std::cout << "\n--- FPS Statistics ---" << std::endl;
        std::cout << "Effective FPS: " << effectiveFPS << " fps" << std::endl;
        std::cout << "Theoretical FPS (based on avg total): " << theoreticalFPS << " fps" << std::endl;
        std::cout << "Target FPS: " << TARGET_FPS << " fps" << std::endl;
    }
    
    std::cout << "=========================================" << std::endl;

    // Cleanup
    if (encoder) {
        AMediaCodec_stop(encoder);
        AMediaCodec_delete(encoder);
    }
    minicap_free(minicap);

    return 0;
}