// ==================================================================================
// ThreadedConvolver.h — Stereo convolver with background tail processing
// Uses FFTConvolver (HiFi-LoFi, MIT) with tail partitions on a worker thread.
// ==================================================================================

#pragma once

#include "../ThirdParty/FFTConvolver/TwoStageFFTConvolver.h"
#include <JuceHeader.h>
#include <atomic>
#include <thread>
#include <mutex>
#include <condition_variable>

// ==================================================================================
// MonoThreadedConvolver — single-channel TwoStageFFTConvolver with a background
// thread for the big tail partitions.
// ==================================================================================
class MonoThreadedConvolver : public fftconvolver::TwoStageFFTConvolver
{
public:
    MonoThreadedConvolver()
    {
        _workerThread = std::thread([this] { workerLoop(); });
    }

    ~MonoThreadedConvolver() override
    {
        {
            std::lock_guard<std::mutex> lock(_mutex);
            _shutdown = true;
        }
        _cv.notify_one();
        if (_workerThread.joinable())
            _workerThread.join();
    }

protected:
    void startBackgroundProcessing() override
    {
        {
            std::lock_guard<std::mutex> lock(_mutex);
            _workReady = true;
        }
        _cv.notify_one();
    }

    void waitForBackgroundProcessing() override
    {
        std::unique_lock<std::mutex> lock(_mutex);
        _cv.wait(lock, [this] { return !_workReady; });
    }

private:
    void workerLoop()
    {
        while (true)
        {
            std::unique_lock<std::mutex> lock(_mutex);
            _cv.wait(lock, [this] { return _workReady || _shutdown; });

            if (_shutdown)
                return;

            // Do the heavy tail convolution on this background thread
            doBackgroundProcessing();

            _workReady = false;
            lock.unlock();
            _cv.notify_one();
        }
    }

    std::thread _workerThread;
    std::mutex _mutex;
    std::condition_variable _cv;
    bool _workReady = false;
    bool _shutdown = false;
};


// ==================================================================================
// StereoThreadedConvolver — wraps two MonoThreadedConvolvers (L+R).
// Drop-in replacement for juce::dsp::Convolution in our IR loader pipeline.
//
// Usage:
//   convolver.prepare (sampleRate, blockSize);
//   convolver.loadIR (irBuffer, irSampleRate);
//   convolver.process (audioBuffer);  // in processBlock
//   convolver.reset();
// ==================================================================================
class StereoThreadedConvolver
{
public:
    StereoThreadedConvolver() = default;

    void prepare (double /*sampleRate*/, int blockSize)
    {
        _blockSize = blockSize;
        _scratch.setSize(2, blockSize);
        _prepared = true;
    }

    // Load an impulse response (may be mono or stereo).
    // Called from the message thread. The swap is protected by _swapMutex;
    // old engines are destroyed OUTSIDE the lock to avoid priority inversion.
    void loadIR (const juce::AudioBuffer<float>& ir, double /*irSampleRate*/)
    {
        if (!_prepared || ir.getNumSamples() == 0)
            return;

        const int numSamples = ir.getNumSamples();
        const int numChannels = ir.getNumChannels();

        // Head block = host block size (low latency). Tail block = 4096 (efficiency).
        const size_t headBlock = static_cast<size_t>(_blockSize);
        const size_t tailBlock = 4096;

        auto newL = std::make_unique<MonoThreadedConvolver>();
        auto newR = std::make_unique<MonoThreadedConvolver>();

        const float* irL = ir.getReadPointer(0);
        const float* irR = (numChannels >= 2) ? ir.getReadPointer(1) : irL;

        newL->init(headBlock, tailBlock, irL, static_cast<size_t>(numSamples));
        newR->init(headBlock, tailBlock, irR, static_cast<size_t>(numSamples));

        // Swap under lock — old engines moved to locals for deferred destruction
        std::unique_ptr<MonoThreadedConvolver> oldL, oldR;
        {
            std::lock_guard<std::mutex> lock(_swapMutex);
            oldL = std::move(_convolverL);
            oldR = std::move(_convolverR);
            _convolverL = std::move(newL);
            _convolverR = std::move(newR);
            _irLoaded.store(true);
        }
        // oldL / oldR destroyed here — outside lock, so audio thread is never
        // blocked waiting for worker thread joins.
    }

    // Process a stereo buffer in-place. Audio-thread safe.
    // Lock is held for the entire duration so the convolvers cannot be
    // destroyed from under us (eliminates use-after-free).
    void process (juce::AudioBuffer<float>& buffer)
    {
        if (!_irLoaded.load())
            return;

        std::lock_guard<std::mutex> lock(_swapMutex);

        if (!_convolverL || !_convolverR)
            return;

        const int numSamples = buffer.getNumSamples();
        const int numChannels = buffer.getNumChannels();

        // Copy raw input to scratch buffer BEFORE convolution.
        // TwoStageFFTConvolver's tail reads from 'input' AFTER the head
        // has written to 'output'. When input==output (in-place), the tail
        // would read head-convolved data instead of raw input → feedback loop.
        // Using separate buffers avoids this.
        for (int ch = 0; ch < numChannels; ++ch)
            _scratch.copyFrom(ch, 0, buffer, ch, 0, numSamples);

        // Process left channel (scratch = raw input, buffer = output)
        _convolverL->process(_scratch.getReadPointer(0),
                             buffer.getWritePointer(0),
                             static_cast<size_t>(numSamples));

        // Process right channel
        if (numChannels >= 2)
        {
            _convolverR->process(_scratch.getReadPointer(1),
                                 buffer.getWritePointer(1),
                                 static_cast<size_t>(numSamples));
        }
    }

    void reset()
    {
        std::lock_guard<std::mutex> lock(_swapMutex);
        _convolverL.reset();
        _convolverR.reset();
        _irLoaded.store(false);
    }

private:
    int _blockSize = 64;
    bool _prepared = false;
    std::atomic<bool> _irLoaded { false };
    std::mutex _swapMutex;
    std::unique_ptr<MonoThreadedConvolver> _convolverL;
    std::unique_ptr<MonoThreadedConvolver> _convolverR;
    juce::AudioBuffer<float> _scratch;  // raw-input copy to avoid in-place feedback
};
