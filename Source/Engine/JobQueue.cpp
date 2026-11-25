#include "JobQueue.h"
#include <juce_events/juce_events.h>
#include <algorithm>

namespace ve
{
    MessageThreadDispatcher::MessageThreadDispatcher()
        : alive (std::make_shared<std::atomic<bool>> (true)) {}

    MessageThreadDispatcher::~MessageThreadDispatcher() { alive->store (false); }

    void MessageThreadDispatcher::dispatch (std::function<void()> fn)
    {
        juce::MessageManager::callAsync ([a = alive, f = std::move (fn)]
        {
            if (a->load())
                f();
        });
    }

    //==========================================================================

    JobQueue::JobQueue (int numWorkers, std::unique_ptr<CompletionDispatcher> d)
        : dispatcher (std::move (d))
    {
        for (int i = 0; i < juce::jmax (1, numWorkers); ++i)
            workers.emplace_back ([this] { workerLoop(); });
    }

    JobQueue::~JobQueue()
    {
        {
            std::lock_guard<std::mutex> lock (mutex);
            stopping = true;
            for (auto& j : queued)  j->cancel();
            for (auto& j : running) j->cancel();
        }
        wake.notify_all();
        for (auto& t : workers)
            t.join();
    }

    JobHandle JobQueue::submit (juce::String name, juce::String group, JobPriority priority,
                                std::function<juce::Result (JobContext&)> work,
                                std::function<void (const JobResult&)> onComplete)
    {
        auto job = std::make_shared<JobRecord>();
        job->name = std::move (name);
        job->group = std::move (group);
        job->priority = (int) priority;
        job->work = std::move (work);
        job->onComplete = std::move (onComplete);

        if (job->group.isNotEmpty())
            cancelGroup (job->group);

        {
            std::lock_guard<std::mutex> lock (mutex);
            job->id = nextId++;
            job->sequence = nextSequence++;
            queued.push_back (job);
            std::push_heap (queued.begin(), queued.end(), [] (const JobHandle& a, const JobHandle& b)
            {
                if (a->priority != b->priority) return a->priority < b->priority;
                return a->sequence > b->sequence;
            });
        }
        wake.notify_one();
        return job;
    }

    void JobQueue::cancelGroup (const juce::String& group)
    {
        std::lock_guard<std::mutex> lock (mutex);
        for (auto& j : queued)  if (j->group == group) j->cancel();
        for (auto& j : running) if (j->group == group) j->cancel();
    }

    void JobQueue::cancelAll()
    {
        std::lock_guard<std::mutex> lock (mutex);
        for (auto& j : queued)  j->cancel();
        for (auto& j : running) j->cancel();
    }

    JobHandle JobQueue::popNext()
    {
        std::pop_heap (queued.begin(), queued.end(), [] (const JobHandle& a, const JobHandle& b)
        {
            if (a->priority != b->priority) return a->priority < b->priority;
            return a->sequence > b->sequence;
        });
        auto j = queued.back();
        queued.pop_back();
        return j;
    }

    void JobQueue::workerLoop()
    {
        for (;;)
        {
            JobHandle job;
            {
                std::unique_lock<std::mutex> lock (mutex);
                wake.wait (lock, [this] { return stopping || ! queued.empty(); });
                if (queued.empty())
                    return; // stopping and nothing left
                job = popNext();
                running.push_back (job);
                job->state.store (JobRecord::State::running, std::memory_order_release);
            }

            JobResult result;
            if (job->isCancelled())
            {
                result.cancelled = true;
            }
            else
            {
                JobContext ctx (job->progress, job->cancelled);
                juce::Result r = juce::Result::fail ("Job threw an exception");
                try { r = job->work (ctx); }
                catch (const std::exception& e) { r = juce::Result::fail (e.what()); }
                catch (...) {}

                result.cancelled = job->isCancelled();
                result.success = r.wasOk() && ! result.cancelled;
                result.error = r.getErrorMessage();
                if (result.success)
                    job->progress.store (1.0f);
            }

            job->work = nullptr; // release captured resources on the worker

            bool deliver;
            {
                std::lock_guard<std::mutex> lock (mutex);
                deliver = ! stopping;
            }
            if (deliver && job->onComplete)
            {
                auto cb = std::move (job->onComplete);
                dispatcher->dispatch ([cb = std::move (cb), result] { cb (result); });
            }
            job->onComplete = nullptr;

            {
                std::lock_guard<std::mutex> lock (mutex);
                running.erase (std::remove (running.begin(), running.end(), job), running.end());
                job->state.store (JobRecord::State::finished, std::memory_order_release);
            }
            idle.notify_all();
        }
    }

    bool JobQueue::waitUntilIdle (int timeoutMs)
    {
        std::unique_lock<std::mutex> lock (mutex);
        return idle.wait_for (lock, std::chrono::milliseconds (timeoutMs),
                              [this] { return queued.empty() && running.empty(); });
    }

    std::vector<JobQueue::Status> JobQueue::getStatus() const
    {
        std::lock_guard<std::mutex> lock (mutex);
        std::vector<Status> out;
        for (auto& j : running) out.push_back ({ j->id, j->name, j->getProgress(), true });
        for (auto& j : queued)  if (! j->isCancelled()) out.push_back ({ j->id, j->name, 0.0f, false });
        return out;
    }
}
