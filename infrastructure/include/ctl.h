#pragma once

#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace dts::ctl {

// 命令：注册表条目，新增/删除/修改命令 = 改表（contracts/infrastructure.md §2）
struct Command {
    const char* name;                 // "set_log_level" / "get_threads"
    const char* usage;                // 用法提示
    int (*fn)(const std::vector<std::string>& args, std::string& out);  // 执行：0 成功
};

// 命令注册表（进程内单例）：命令集中登记，供 console/control 共用
class CommandRegistry {
public:
    static CommandRegistry& Instance();

    // 登记/查询；注册失败返回非 0（重名）
    int Register(const Command& cmd);
    const Command* Find(const char* name) const;
    // 遍历（console help 用）
    void ForEach(const std::function<void(const Command&)>& fn) const;

private:
    CommandRegistry() = default;
    mutable std::mutex m_mutex;
    std::vector<Command> m_commands;
};

// 执行器：解析 "name arg1 arg2" -> 查表 -> 执行。返回 0 成功；非 0 失败（out 带错误）
int Execute(const std::string& line, std::string& out);

// 注册内置命令（help / get_threads / set_log_level）。幂等，control 线程启动前调用。
void RegisterBuiltins();

}  // namespace dts::ctl
