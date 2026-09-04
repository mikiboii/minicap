// minicap_h264_server_nv12_timed.cpp
#include <iostream>
#include <vector>
#include <chrono>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <cstring>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/tcp.h>
#include <errno.h>
#include <signal.h>
#include <algorithm>
#include <cstdint>

#include <Minicap.hpp>
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaFormat.h>
#include <chrono>
#include <vector>
#include <numeric> // for std::accumulate
// Tunables
static const int TARGET_FPS = 60;
static const int MAX_SIZE = 1080;
static const int INPUT_DEQUEUE_TIMEOUT_US = 5000;  // 5 ms
static const int OUTPUT_DEQUEUE_TIMEOUT_US = 5000; // 5 ms

// Frame waiter
class FrameWaiter : public Minicap::FrameAvailableListener {
public:
    FrameWaiter() : mPendingFrames(0), mStopped(false) {}
    void onFrameAvailable() override {
        std::unique_lock<std::mutex> lock(mMutex);
        ++mPendingFrames;
        mCond.notify_one();
    }
    bool waitForFrame(int timeout_ms = 200) {
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

// Convert BGRA/RGBA -> NV12
void rgbToNV12_fixed(const uint8_t* src, uint8_t* dst,
                     int width, int height,
                     int srcStrideBytes, int bpp,
                     Minicap::Format fmt)
{
    uint8_t* yPlane  = dst;
    uint8_t* uvPlane = dst + width * height;

    int rIdx = 0, gIdx = 1, bIdx = 2;
    if (fmt == Minicap::FORMAT_BGRA_8888) { bIdx=0; gIdx=1; rIdx=2; }
    else if (fmt == Minicap::FORMAT_RGBA_8888) { rIdx=0; gIdx=1; bIdx=2; }

    // Y plane
    for (int y=0;y<height;y++) {
        const uint8_t* srcRow = src + (size_t)y*srcStrideBytes;
        uint8_t* yRow = yPlane + (size_t)y*width;
        for (int x=0;x<width;x++) {
            const uint8_t* px = srcRow + x*bpp;
            int R=px[rIdx], G=px[gIdx], B=px[bIdx];
            int Y = ((66*R + 129*G + 25*B +128)>>8)+16;
            yRow[x] = clamp255(Y);
        }
    }

    // UV plane (2x2 subsampled)
    for (int y=0;y<height;y+=2) {
        const uint8_t* srcRow0 = src + (size_t)y*srcStrideBytes;
        const uint8_t* srcRow1 = src + (size_t)(y+1)*srcStrideBytes;
        uint8_t* uvRow = uvPlane + (size_t)(y/2)*width;
        for (int x=0;x<width;x+=2) {
            const uint8_t* px00=srcRow0+x*bpp;
            const uint8_t* px01=srcRow0+(x+1)*bpp;
            const uint8_t* px10=srcRow1+x*bpp;
            const uint8_t* px11=srcRow1+(x+1)*bpp;

            int R=(px00[rIdx]+px01[rIdx]+px10[rIdx]+px11[rIdx])/4;
            int G=(px00[gIdx]+px01[gIdx]+px10[gIdx]+px11[gIdx])/4;
            int B=(px00[bIdx]+px01[bIdx]+px10[bIdx]+px11[bIdx])/4;

            int U = ((-38*R - 74*G + 112*B +128)>>8)+128;
            int V = ((112*R - 94*G - 18*B +128)>>8)+128;

            uvRow[x]   = clamp255(U);
            uvRow[x+1] = clamp255(V);
        }
    }
}

// Create MediaCodec H264 encoder
AMediaCodec* createH264Encoder(int width, int height, int bitrate, int fps) {
    AMediaCodec* codec = AMediaCodec_createEncoderByType("video/avc");
    if (!codec) return nullptr;

    AMediaFormat* format = AMediaFormat_new();
    AMediaFormat_setString(format,"mime","video/avc");
    AMediaFormat_setInt32(format,"width",width);
    AMediaFormat_setInt32(format,"height",height);
    AMediaFormat_setInt32(format,"bitrate",bitrate);
    AMediaFormat_setInt32(format,"frame-rate",fps);
    AMediaFormat_setInt32(format,"i-frame-interval",1);
    AMediaFormat_setInt32(format,"color-format",21); // COLOR_FormatYUV420Flexible

    if (AMediaCodec_configure(codec,format,nullptr,nullptr,AMEDIACODEC_CONFIGURE_FLAG_ENCODE)!=AMEDIA_OK) {
        AMediaFormat_delete(format);
        return nullptr;
    }
    AMediaFormat_delete(format);
    if (AMediaCodec_start(codec)!=AMEDIA_OK) return nullptr;
    return codec;
}

// TCP server
int startTcpServer(int port) {
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd<0) return -1;
    int opt=1;
    setsockopt(server_fd,SOL_SOCKET,SO_REUSEADDR,&opt,sizeof(opt));
    sockaddr_in addr{};
    addr.sin_family=AF_INET;
    addr.sin_addr.s_addr=INADDR_ANY;
    addr.sin_port=htons(port);
    if (bind(server_fd,(struct sockaddr*)&addr,sizeof(addr))<0) { close(server_fd); return -1; }
    if (listen(server_fd,1)<0) { close(server_fd); return -1; }
    return server_fd;
}

int main() {
    struct sigaction sa{};
    sa.sa_handler=signal_handler;
    sigaction(SIGINT,&sa,nullptr);
    sigaction(SIGTERM,&sa,nullptr);

    int server_fd = startTcpServer(8080);
    if(server_fd<0) return 1;

    sockaddr_in client_addr{};
    socklen_t client_len=sizeof(client_addr);
    int client_fd=accept(server_fd,(struct sockaddr*)&client_addr,&client_len);
    if(client_fd<0){ close(server_fd); return 1; }
    int one=1; setsockopt(client_fd,IPPROTO_TCP,TCP_NODELAY,&one,sizeof(one));

    minicap_start_thread_pool();
    Minicap* minicap=minicap_create(0);
    if(!minicap){ close(client_fd); close(server_fd); return 1; }

    Minicap::DisplayInfo realInfo;
    if(minicap_try_get_display_info(0,&realInfo)!=0){ minicap_free(minicap); close(client_fd); close(server_fd); return 1; }

    Minicap::DisplayInfo desiredInfo = realInfo;
    int longSide = std::max(realInfo.width, realInfo.height);
    if(longSide>MAX_SIZE){
        float scale = (float)MAX_SIZE/longSide;
        desiredInfo.width = ((int)(realInfo.width*scale)/2)*2;
        desiredInfo.height= ((int)(realInfo.height*scale)/2)*2;
    }

    if(minicap->setRealInfo(realInfo)!=0 || minicap->setDesiredInfo(desiredInfo)!=0){
        minicap_free(minicap); close(client_fd); close(server_fd); return 1;
    }

    minicap->setFrameAvailableListener(&gWaiter);
    if(minicap->applyConfigChanges()!=0){ minicap_free(minicap); close(client_fd); close(server_fd); return 1; }

    int bitrate=8000000;
    AMediaCodec* encoder=createH264Encoder(desiredInfo.width,desiredInfo.height,bitrate,TARGET_FPS);
    if(!encoder){ minicap_free(minicap); close(client_fd); close(server_fd); return 1; }

    size_t yuvSize = (size_t)desiredInfo.width*(size_t)desiredInfo.height*3/2;
    std::vector<uint8_t> yuvBuf(yuvSize);
    Minicap::Frame frame;

    
    size_t frameCounter = 0;
std::vector<double> captureTimes;
std::vector<double> convertTimes;
std::vector<double> encodeTimes;

while (true) {
    // ---- Capture timing ----
    auto captureStart = std::chrono::steady_clock::now();
    if (!gWaiter.waitForFrame(200)) continue;

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

    // ---- Encode + send timing ----
    auto encodeStart = std::chrono::steady_clock::now();

    ssize_t inIndex = AMediaCodec_dequeueInputBuffer(encoder, INPUT_DEQUEUE_TIMEOUT_US);
    if (inIndex >= 0) {
        size_t inBufSize = 0;
        uint8_t* inBuf = AMediaCodec_getInputBuffer(encoder, (size_t)inIndex, &inBufSize);
        if (inBuf) {
            memcpy(inBuf, yuvBuf.data(), std::min(inBufSize, yuvSize));
            int64_t pts_us = std::chrono::duration_cast<std::chrono::microseconds>(
                                std::chrono::steady_clock::now().time_since_epoch()).count();
            AMediaCodec_queueInputBuffer(encoder, (size_t)inIndex, 0, yuvSize, pts_us, 0);
        }
    }

    AMediaCodecBufferInfo info;
    ssize_t outIndex = AMediaCodec_dequeueOutputBuffer(encoder, &info, OUTPUT_DEQUEUE_TIMEOUT_US);
    while (outIndex >= 0) {
        size_t outSize = 0;
        uint8_t* outBuf = AMediaCodec_getOutputBuffer(encoder, (size_t)outIndex, &outSize);
        if (outBuf && info.size > 0) {
            uint32_t lenBE = htonl((uint32_t)info.size);
            send(client_fd, &lenBE, 4, MSG_NOSIGNAL);
            ssize_t left = (ssize_t)info.size;
            const uint8_t* p = outBuf;
            while (left > 0) { 
                ssize_t w = send(client_fd, p, left, MSG_NOSIGNAL);
                if (w <= 0) break;
                left -= w; 
                p += w; 
            }
        }
        AMediaCodec_releaseOutputBuffer(encoder, (size_t)outIndex, false);
        outIndex = AMediaCodec_dequeueOutputBuffer(encoder, &info, OUTPUT_DEQUEUE_TIMEOUT_US);
    }

    auto encodeEnd = std::chrono::steady_clock::now();
    std::chrono::duration<double, std::milli> encodeDuration = encodeEnd - encodeStart;
    encodeTimes.push_back(encodeDuration.count());

    frameCounter++;
    if (frameCounter % 100 == 0) {
        double avgCapture = std::accumulate(captureTimes.end() - 100, captureTimes.end(), 0.0) / 100.0;
        double avgConvert = std::accumulate(convertTimes.end() - 100, convertTimes.end(), 0.0) / 100.0;
        double avgEncode = std::accumulate(encodeTimes.end() - 100, encodeTimes.end(), 0.0) / 100.0;

        std::cout << "[server] Average capture time (last 100 frames): " << avgCapture << " ms/frame" << std::endl;
        std::cout << "[server] Average RGB->NV12 conversion time (last 100 frames): " << avgConvert << " ms/frame" << std::endl;
        std::cout << "[server] Average encode+send time (last 100 frames): " << avgEncode << " ms/frame" << std::endl;
        std::cout << "[server] Average total (capture+convert+encode) : " << avgCapture + avgConvert + avgEncode << " ms/frame" << std::endl;
    }

    minicap->releaseConsumedFrame(&frame);
}




    // Cleanup
    if(encoder){ AMediaCodec_stop(encoder); AMediaCodec_delete(encoder); }
    minicap_free(minicap);
    if(client_fd>0) close(client_fd);
    if(server_fd>0) close(server_fd);

    return 0;
}