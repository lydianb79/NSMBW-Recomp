/*
 * AudioOutput.m - an AudioQueue at the game's native 32 kHz, 16-bit stereo.
 * The runtime's AX mixer fills a ring buffer every 3 ms; the queue callback
 * pulls from it. Small buffers keep latency low (~3 x 512 frames = 48 ms).
 */
#import <AudioToolbox/AudioToolbox.h>
#import <Foundation/Foundation.h>

#include "AudioOutput.h"
#include "rt.h"

#define FRAMES_PER_BUFFER 512
#define NUM_BUFFERS 3

static AudioQueueRef g_queue;

static void fill(void *user, AudioQueueRef q, AudioQueueBufferRef buf) {
    (void)user;
    rt_audio_read((int16_t *)buf->mAudioData, FRAMES_PER_BUFFER);
    buf->mAudioDataByteSize = FRAMES_PER_BUFFER * 4;
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
        if (AudioQueueAllocateBuffer(g_queue, FRAMES_PER_BUFFER * 4, &b) == noErr) {
            memset(b->mAudioData, 0, FRAMES_PER_BUFFER * 4);
            b->mAudioDataByteSize = FRAMES_PER_BUFFER * 4;
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
