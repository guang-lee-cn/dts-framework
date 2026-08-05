#include "ctl.h"

#include <cstring>
#include <sstream>

#include "log.h"
#include "thread_api.h"

namespace dts::ctl {

namespace {

// 按空白拆分命令行："name arg1 arg2" -> {name, arg1, arg2}
std::vector<std::string> SplitLine(const std::string& line) {
    std::vector<std::string> tokens;
    std::istringstream iss(line);
    std::string tok;
    while (iss >> tok) {
        tokens.push_back(tok);
    }
    return tokens;
}

std::string Trim(const std::string& s) {
    const size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) {
        return "";
    }
    const size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

// ---- 内置命令 ----

int CmdHelp(const std::vector<std::string>&, std::string& out) {
    CommandRegistry::Instance().ForEach([&out](const Command& cmd) {
        out += std::string(cmd.name) + "  " + cmd.usage + "\n";
    });
    return 0;
}

int CmdGetThreads(const std::vector<std::string>&, std::string& out) {
    std::vector<detsched::ThreadInfo> info(64);
    const size_t n = detsched::QueryThreads(info.data(), info.size());
    for (size_t i = 0; i < n; i++) {
        out += info[i].name + "  seg=" + std::to_string(info[i].segIndex) +
               " prio=" + std::to_string(info[i].prio) +
               " policy=" + std::to_string(info[i].schedPolicy) +
               " cpu=" + std::to_string(info[i].cpuAffinity) +
               " tid=" + std::to_string(info[i].tid) + "\n";
    }
    return 0;
}

int CmdSetLogLevel(const std::vector<std::string>& args, std::string& out) {
    if (args.empty()) {
        out = "usage: set_log_level <trace|debug|info|warn|error>";
        return 1;
    }
    const std::string& lvl = args[0];
    log::Level level;
    if (lvl == "trace") {
        level = log::Level::TRACE;
    } else if (lvl == "debug") {
        level = log::Level::DEBUG;
    } else if (lvl == "info") {
        level = log::Level::INFO;
    } else if (lvl == "warn") {
        level = log::Level::WARN;
    } else if (lvl == "error") {
        level = log::Level::ERROR;
    } else {
        out = "invalid level: " + lvl + " (trace|debug|info|warn|error)";
        return 1;
    }
    log::SetLevel(level);
    out = "log level -> " + lvl;
    return 0;
}

}  // namespace

CommandRegistry& CommandRegistry::Instance() {
    static CommandRegistry inst;
    return inst;
}

int CommandRegistry::Register(const Command& cmd) {
    if (cmd.name == nullptr || cmd.fn == nullptr) {
        return -1;
    }
    std::lock_guard<std::mutex> lk(m_mutex);
    for (const auto& c : m_commands) {
        if (std::strcmp(c.name, cmd.name) == 0) {
            return -1;  // 重名
        }
    }
    m_commands.push_back(cmd);
    return 0;
}

const Command* CommandRegistry::Find(const char* name) const {
    std::lock_guard<std::mutex> lk(m_mutex);
    for (const auto& c : m_commands) {
        if (std::strcmp(c.name, name) == 0) {
            return &c;
        }
    }
    return nullptr;
}

void CommandRegistry::ForEach(const std::function<void(const Command&)>& fn) const {
    std::lock_guard<std::mutex> lk(m_mutex);
    for (const auto& c : m_commands) {
        fn(c);
    }
}

int Execute(const std::string& line, std::string& out) {
    const std::string trimmed = Trim(line);
    if (trimmed.empty()) {
        return 0;  // 空行忽略
    }
    const std::vector<std::string> tokens = SplitLine(trimmed);
    const Command* cmd = CommandRegistry::Instance().Find(tokens[0].c_str());
    if (cmd == nullptr) {
        out = "not found: " + tokens[0];
        return 1;
    }
    std::vector<std::string> args(tokens.begin() + 1, tokens.end());
    return cmd->fn(args, out);
}

void RegisterBuiltins() {
    static bool registered = false;
    if (registered) {
        return;
    }
    auto& reg = CommandRegistry::Instance();
    reg.Register(Command{"help", "列全部命令", CmdHelp});
    reg.Register(Command{"get_threads", "全进程线程注册表", CmdGetThreads});
    reg.Register(Command{"set_log_level", "set_log_level <trace|debug|info|warn|error>", CmdSetLogLevel});
    registered = true;
}

}  // namespace dts::ctl
