#pragma once

#include "SimpleJson.h"

#include <chrono>
#include <cstdint>
#include <deque>
#include <future>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace mcp
{
enum class AccessMode
{
    ReadOnly,
    ConfirmMutations,
    AllowMutations,
};

struct CommandRequest
{
    uint64_t id = 0;
    std::string toolName;
    std::string actionMethod;
    cld::JsonValue params;
    bool validateOnly = false;
    bool mutation = false;
    std::string summary;
    std::chrono::steady_clock::time_point deadline;
};

struct CommandResult
{
    bool ok = false;
    std::string diagnostics;
    std::string structuredJson = "{}";
    std::string contentJson;
};

struct PendingApproval
{
    uint64_t id = 0;
    std::string toolName;
    std::string actionMethod;
    std::string summary;
    int secondsRemaining = 0;
};

struct SubmitResult
{
    bool accepted = false;
    uint64_t id = 0;
    std::string diagnostics;
    std::future<CommandResult> future;
};

class Dispatcher
{
public:
    using Executor = std::function<CommandResult(const CommandRequest&)>;

    SubmitResult Submit(CommandRequest request, bool requiresApproval);
    void ProcessOne(const Executor& executor);
    void Approve(uint64_t id);
    void Reject(uint64_t id, const std::string& reason);
    void Cancel(uint64_t id, const std::string& reason);
    void CancelAll(const std::string& reason);
    std::vector<PendingApproval> PendingApprovals() const;
    size_t PendingCount() const;
    void SetQueueLimit(size_t limit);

private:
    struct PendingCommand
    {
        CommandRequest request;
        bool requiresApproval = false;
        bool approved = false;
        std::promise<CommandResult> promise;
    };

    void ExpireLocked(std::vector<std::shared_ptr<PendingCommand>>& expired);

    mutable std::mutex m_mutex;
    std::deque<std::shared_ptr<PendingCommand>> m_commands;
    size_t m_queueLimit = 16;
    uint64_t m_nextId = 1;
};
}
