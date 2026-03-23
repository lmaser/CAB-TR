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

    void prepare (double sampleRate, int blockSize)
    {
        _blockSize = blockSize;
        _sampleRate = sampleRate;
        _scratch.setSize(2, blockSize);
        _crossScratch.setSize(2, blockSize);
        _prepared = true;
    }

    // Load an impulse response (may be mono or stereo).
    // Called from the message thread. The swap is protected by _swapMutex;
    // old engines are kept alive for crossfading, then destroyed when done.
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

        // Swap under lock — old convolvers kept for crossfade
        std::unique_ptr<MonoThreadedConvolver> discardL, discardR;
        {
            std::lock_guard<std::mutex> lock(_swapMutex);
            // If a previous crossfade is still in progress, discard those old convolvers
            discardL = std::move(_oldConvolverL);
            discardR = std::move(_oldConvolverR);
            // Move current → old for crossfade
            _oldConvolverL = std::move(_convolverL);
            _oldConvolverR = std::move(_convolverR);
            _convolverL = std::move(newL);
            _convolverR = std::move(newR);
            _irLoaded.store(true);
            // ~50ms crossfade
            _crossfadeTotal = static_cast<int>(_sampleRate * 0.05);
            _crossfadeRemaining = _crossfadeTotal;
        }
        // discardL / discardR destroyed here — outside lock
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
        for (int ch = 0; ch < numChannels; ++ch)
            _scratch.copyFrom(ch, 0, buffer, ch, 0, numSamples);

        // Process through new (current) convolvers
        _convolverL->process(_scratch.getReadPointer(0),
                             buffer.getWritePointer(0),
                             static_cast<size_t>(numSamples));

        if (numChannels >= 2)
        {
            _convolverR->process(_scratch.getReadPointer(1),
                                 buffer.getWritePointer(1),
                                 static_cast<size_t>(numSamples));
        }

        // Crossfade from old → new convolver output
        if (_crossfadeRemaining > 0 && _oldConvolverL && _oldConvolverR)
        {
            // Process same input through OLD convolvers
            _oldConvolverL->process(_scratch.getReadPointer(0),
                                    _crossScratch.getWritePointer(0),
                                    static_cast<size_t>(numSamples));
            if (numChannels >= 2)
            {
                _oldConvolverR->process(_scratch.getReadPointer(1),
                                        _crossScratch.getWritePointer(1),
                                        static_cast<size_t>(numSamples));
            }

            // Blend: old * (1-t) + new * t, with S-curve
            // Counter decremented once per sample (not per channel)
            for (int i = 0; i < numSamples && _crossfadeRemaining > 0; ++i)
            {
                const float t = 1.0f - static_cast<float>(_crossfadeRemaining)
                                        / static_cast<float>(_crossfadeTotal);
                const float wet = t * t * (3.0f - 2.0f * t); // S-curve
                --_crossfadeRemaining;

                for (int ch = 0; ch < numChannels; ++ch)
                {
                    auto* newOut = buffer.getWritePointer(ch);
                    const auto* oldOut = _crossScratch.getReadPointer(ch);
                    newOut[i] = oldOut[i] + (newOut[i] - oldOut[i]) * wet;
                }
            }

            // Crossfade done — release old convolvers
            if (_crossfadeRemaining <= 0)
            {
                _oldConvolverL.reset();
                _oldConvolverR.reset();
            }
        }
    }

    void reset()
    {
        std::lock_guard<std::mutex> lock(_swapMutex);
        _convolverL.reset();
        _convolverR.reset();
        _oldConvolverL.reset();
        _oldConvolverR.reset();
        _irLoaded.store(false);
        _crossfadeRemaining = 0;
    }

private:
    int _blockSize = 64;
    double _sampleRate = 48000.0;
    bool _prepared = false;
    std::atomic<bool> _irLoaded { false };
    std::mutex _swapMutex;
    std::unique_ptr<MonoThreadedConvolver> _convolverL;
    std::unique_ptr<MonoThreadedConvolver> _convolverR;
    std::unique_ptr<MonoThreadedConvolver> _oldConvolverL;
    std::unique_ptr<MonoThreadedConvolver> _oldConvolverR;
    juce::AudioBuffer<float> _scratch;       // raw-input copy to avoid in-place feedback
    juce::AudioBuffer<float> _crossScratch;  // old convolver output for crossfade
    int _crossfadeTotal = 0;
    int _crossfadeRemaining = 0;
};
