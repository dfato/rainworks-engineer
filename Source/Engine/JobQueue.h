#pragma once

#include <juce_core/juce_core.h>
#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace ve
{
    // Passed to running work so it can report progress and notice cancellation.
    class JobContext
    {
    public:
        JobContext (std::atomic<float>& p, const std::atomic<bool>& c) noexcept : progress (p), cancelled (c) {}

        void setProgress (float v) noexcept { progress.store (juce::jlimit (0.0f, 1.0f, v), std::memory_order_relaxed); }
        bool shouldCancel() const noexcept  { return cancelled.load (std::memory_order_relaxed); }

    private:
        std::atomic<float>& progress;
        const std::atomic<bool>& cancelled;
    };

    struct JobResult
    {
        bool success = false;
        bool cancelled = false;
        juce::String error;
    };

    enum class JobPriority : int { low = 0, normal = 10, high = 20, urgent = 30 };

    // Shared state for one submitted job. Safe to hold from any non-audio thread.
    class JobRecord
    {
    public:
        enum class State { queued, running, finished };

        std::uint64_t getId() const noexcept       { return id; }
        const juce::String& getName() const noexcept  { return name; }
        const juce::String& getGroup() const noexcept { return group; }
        float getProgress() const noexcept         { return progress.load (std::memory_order_relaxed); }
        State getState() const noexcept            { return state.load (std::memory_order_acquire); }
        void cancel() noexcept                     { cancelled.store (true, std::memory_order_relaxed); }
        bool isCancelled() const noexcept          { return cancelled.load (std::memory_order_relaxed); }

    private:
        friend class JobQueue;
        std::uint64_t id = 0, sequence = 0;
        int priority = 0;
        juce::String name, group;
        std::function<juce::Result (JobContext&)> work;
        std::function<void (const JobResult&)> onComplete;
        std::atomic<float> progress { 0.0f };
        std::atomic<bool> cancelled { false };
        std::atomic<State> state { State::queued };
    };

    using JobHandle = std::shared_ptr<JobRecord>;

    // Where completion callbacks run.
    class CompletionDispatcher
    {
    public:
        virtual ~CompletionDispatcher() = default;
        virtual void dispatch (std::function<void()> fn) = 0;
    };

    // Default: completion callbacks run on the JUCE message thread.
    // Callbacks posted after this dispatcher is destroyed are dropped.
    class MessageThreadDispatcher final : public CompletionDispatcher
    {
    public:
        MessageThreadDispatcher();
        ~MessageThreadDispatcher() override;
        void dispatch (std::function<void()> fn) override;

    private:
        std::shared_ptr<std::atomic<bool>> alive;
    };

    // Runs callbacks directly on the worker. For tests and command line tools.
    class ImmediateDispatcher final : public CompletionDispatcher
    {
    public:
        void dispatch (std::function<void()> fn) override { fn(); }
    };

    // Priority job queue with a fixed pool of worker threads.
    // Never call into this from the audio thread.
    class JobQueue
    {
    public:
        struct Status
        {
            std::uint64_t id;
            juce::String name;
            float progress;
            bool running;
        };

        explicit JobQueue (int numWorkers = 2,
                           std::unique_ptr<CompletionDispatcher> dispatcher = std::make_unique<MessageThreadDispatcher>());
        ~JobQueue();

        // `work` returns Result::ok() on success. `onComplete` runs through the dispatcher.
        // Submitting a job with a non-empty group cancels queued and running jobs in that group.
        JobHandle submit (juce::String name,
                          juce::String group,
                          JobPriority priority,
                          std::function<juce::Result (JobContext&)> work,
                          std::function<void (const JobResult&)> onComplete = {});

        void cancelGroup (const juce::String& group);
        void cancelAll();

        // Blocks until no jobs are queued or running (test helper; not for the message thread
        // when using MessageThreadDispatcher callbacks you then wait on).
        bool waitUntilIdle (int timeoutMs);

        std::vector<Status> getStatus() const;

    private:
        void workerLoop();
        JobHandle popNext();

        std::unique_ptr<CompletionDispatcher> dispatcher;

        mutable std::mutex mutex;
        std::condition_variable wake, idle;
        std::vector<JobHandle> queued;     // kept as a heap
        std::vector<JobHandle> running;
        std::uint64_t nextId = 1, nextSequence = 0;
        bool stopping = false;

        std::vector<std::thread> workers;

        JUCE_DECLARE_NON_COPYABLE (JobQueue)
    };
}
