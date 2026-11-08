/*
 * AudioOutput.m - an AudioQueue at the game's native 32 kHz, 16-bit stereo.
 * The runtime's AX mixer fills a ring buffer every 3 ms; the queue callback
 * pulls from it. The buffer size follows Settings > Audio > Latency
 * (three buffers in flight; read on every callback, so changes apply at once).
 */
#import <AudioToolbox/AudioToolbox.h>
#import <Foundation/Foundation.h>

#include "AudioOutput.h"
#include "rt.h"

#define MAX_FRAMES_PER_BUFFER 2400
#define NUM_BUFFERS 3

static int frames_per_buffer(void) {
    int ms = rt_settings.audio_latency_ms < 20 ? 20 : rt_settings.audio_latency_ms > 200 ? 200 : rt_settings.audio_latency_ms;
    int n = ms * 32 / NUM_BUFFERS;             /* 32 frames per ms */
    return n < 128 ? 128 : n > MAX_FRAMES_PER_BUFFER ? MAX_FRAMES_PER_BUFFER : n;
}

static AudioQueueRef g_queue;

static void fill(void *user, AudioQueueRef q, AudioQueueBufferRef buf) {
    (void)user;
    int n = frames_per_buffer();
    rt_audio_read((int16_t *)buf->mAudioData, n);
    buf->mAudioDataByteSize = (UInt32)n * 4;
    AudioQueueEnqueueBuffer(q, buf, 0, NULL);
}

bool rt_audio_output_start(void) {
    if (g_queue) return true;
    AudioStreamBasicDescription f = { 0 };
    f.mSampleRate = 32000;
    f.mFormatID = kAudioFormatLinearPCM;
    f.mFormatFlags = kLinearPCMFormatFlagIsSignedInteger | kLinearPCMFormatFlagIsPacked;
    f.mChannelsPerFrame = 2;
    f.mBitsPerChannel = 16;
    f.mBytesPerFrame = 4;
    f.mFramesPerPacket = 1;
    f.mBytesPerPacket = 4;
    OSStatus st = AudioQueueNewOutput(&f, fill, NULL, NULL, NULL, 0, &g_queue);
    if (st != noErr) { rt_log(0, "audio: AudioQueueNewOutput failed (%d)", (int)st); g_queue = NULL; return false; }
    for (int i = 0; i < NUM_BUFFERS; i++) {
        AudioQueueBufferRef b;
        if (AudioQueueAllocateBuffer(g_queue, MAX_FRAMES_PER_BUFFER * 4, &b) == noErr) {
            memset(b->mAudioData, 0, MAX_FRAMES_PER_BUFFER * 4);
            b->mAudioDataByteSize = (UInt32)frames_per_buffer() * 4;
            AudioQueueEnqueueBuffer(g_queue, b, 0, NULL);
        }
    }
    st = AudioQueueStart(g_queue, NULL);
    if (st != noErr) { rt_log(0, "audio: AudioQueueStart failed (%d)", (int)st); return false; }
    rt_log(1, "audio: Core Audio output at 32 kHz stereo");
    return true;
}

void rt_audio_output_stop(void) {
    if (!g_queue) return;
    AudioQueueStop(g_queue, true);
    AudioQueueDispose(g_queue, true);
    g_queue = NULL;
}
