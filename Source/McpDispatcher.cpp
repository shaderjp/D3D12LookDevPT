#include "stdafx.h"
#include "McpDispatcher.h"

#include <algorithm>

namespace mcp
{
SubmitResult Dispatcher::Submit(CommandRequest request, bool requiresApproval)
{
    SubmitResult result;
    auto command = std::make_shared<PendingCommand>();
    command->request = std::move(request);
    command->requiresApproval = requiresApproval;

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_commands.size() >= m_queueLimit)
        {
            result.diagnostics = "MCP mutation queue is full.";
            return result;
        }

        command->request.id = m_nextId++;
        result.id = command->request.id;
        result.future = command->promise.get_future();
        m_commands.push_back(command);
    }

    result.accepted = true;
    return result;
}

void Dispatcher::ProcessOne(const Executor& executor)
{
    std::vector<std::shared_ptr<PendingCommand>> expired;
    std::shared_ptr<PendingCommand> ready;

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        ExpireLocked(expired);

        const auto it = std::find_if(m_commands.begin(), m_commands.end(), [](const std::shared_ptr<PendingCommand>& command)
        {
            return !command->requiresApproval || command->approved;
        });
        if (it != m_commands.end())
        {
            ready = *it;
            m_commands.erase(it);
        }
    }

    for (const std::shared_ptr<PendingCommand>& command : expired)
    {
        CommandResult result;
        result.ok = false;
        result.diagnostics = "MCP request timed out.";
        result.structuredJson = "{\"ok\":false,\"diagnostics\":\"MCP request timed out.\"}";
        command->promise.set_value(result);
    }

    if (!ready)
    {
        return;
    }

    CommandResult result = executor(ready->request);
    ready->promise.set_value(result);
}

void Dispatcher::Approve(uint64_t id)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    for (const std::shared_ptr<PendingCommand>& command : m_commands)
    {
        if (command->request.id == id)
        {
            command->approved = true;
            return;
        }
    }
}

void Dispatcher::Reject(uint64_t id, const std::string& reason)
{
    Cancel(id, reason.empty() ? "MCP request was rejected." : reason);
}

void Dispatcher::Cancel(uint64_t id, const std::string& reason)
{
    std::shared_ptr<PendingCommand> cancelled;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        const auto it = std::find_if(m_commands.begin(), m_commands.end(), [id](const std::shared_ptr<PendingCommand>& command)
        {
            return command->request.id == id;
        });
        if (it != m_commands.end())
        {
            cancelled = *it;
            m_commands.erase(it);
        }
    }

    if (cancelled)
    {
        CommandResult result;
        result.ok = false;
        result.diagnostics = reason;
        result.structuredJson = "{\"ok\":false,\"diagnostics\":\"" + cld::EscapeJson(reason) + "\"}";
        cancelled->promise.set_value(result);
    }
}

void Dispatcher::CancelAll(const std::string& reason)
{
    std::deque<std::shared_ptr<PendingCommand>> commands;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        commands.swap(m_commands);
    }

    for (const std::shared_ptr<PendingCommand>& command : commands)
    {
        CommandResult result;
        result.ok = false;
        result.diagnostics = reason;
        result.structuredJson = "{\"ok\":false,\"diagnostics\":\"" + cld::EscapeJson(reason) + "\"}";
        command->promise.set_value(result);
    }
}

std::vector<PendingApproval> Dispatcher::PendingApprovals() const
{
    std::vector<PendingApproval> approvals;
    const auto now = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lock(m_mutex);
    for (const std::shared_ptr<PendingCommand>& command : m_commands)
    {
        if (!command->requiresApproval || command->approved)
        {
            continue;
        }

        PendingApproval approval;
        approval.id = command->request.id;
        approval.toolName = command->request.toolName;
        approval.actionMethod = command->request.actionMethod;
        approval.summary = command->request.summary;
        approval.secondsRemaining = static_cast<int>(std::chrono::duration_cast<std::chrono::seconds>(command->request.deadline - now).count());
        approvals.push_back(approval);
    }
    return approvals;
}

size_t Dispatcher::PendingCount() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_commands.size();
}

void Dispatcher::SetQueueLimit(size_t limit)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_queueLimit = limit;
}

void Dispatcher::ExpireLocked(std::vector<std::shared_ptr<PendingCommand>>& expired)
{
    const auto now = std::chrono::steady_clock::now();
    for (auto it = m_commands.begin(); it != m_commands.end();)
    {
        if ((*it)->request.deadline <= now)
        {
            expired.push_back(*it);
            it = m_commands.erase(it);
        }
        else
        {
            ++it;
        }
    }
}
}
