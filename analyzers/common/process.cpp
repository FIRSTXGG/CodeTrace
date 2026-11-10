#include "analyzers/common/process.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <random>

#ifdef _WIN32
#  include <process.h>
#  define CT_POPEN _popen
#  define CT_PCLOSE _pclose
#else
#  include <sys/wait.h>
#  include <unistd.h>
#  define CT_POPEN popen
#  define CT_PCLOSE pclose
#endif

namespace codetrace::proc {

namespace {

std::string quote(const std::string& arg) {
#ifdef _WIN32
    std::string out = "\"";
    for (char c : arg) {
        if (c == '"') out += "\\\"";
        else out.push_back(c);
    }
    return out + "\"";
#else
    std::string out = "'";
    for (char c : arg) {
        if (c == '\'') out += "'\\''";
        else out.push_back(c);
    }
    return out + "'";
#endif
}

}  // namespace

Result run(const std::vector<std::string>& argv) {
    Result r;
    if (argv.empty()) return r;
    std::string cmd;
    for (const auto& a : argv) {
        if (!cmd.empty()) cmd.push_back(' ');
        cmd += quote(a);
    }
#ifdef _WIN32
    cmd += " 2>NUL";
    cmd = "\"" + cmd + "\"";   // cmd.exe strips the outer quotes
    FILE* pipe = CT_POPEN(cmd.c_str(), "rb");
#else
    cmd += " 2>/dev/null";
    FILE* pipe = CT_POPEN(cmd.c_str(), "r");
#endif
    if (!pipe) return r;
    r.started = true;
    char buf[65536];
    std::size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, pipe)) > 0) r.output.append(buf, n);
    int status = CT_PCLOSE(pipe);
#ifdef _WIN32
    r.exitCode = status;
#else
    r.exitCode = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
#endif
    return r;
}

TempFile::TempFile(const std::string& suffix) {
    static std::atomic<unsigned> counter{0};
    std::random_device rd;
    std::error_code ec;
    auto dir = std::filesystem::temp_directory_path(ec);
    if (ec) dir = ".";
#ifdef _WIN32
    auto pid = _getpid();
#else
    auto pid = getpid();
#endif
    auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    path_ = (dir / ("codetrace-" + std::to_string(pid) + "-" + std::to_string(counter++) + "-" +
                    std::to_string(rd() ^ static_cast<unsigned>(stamp)) + suffix))
                .string();
}

TempFile::~TempFile() {
    std::error_code ec;
    std::filesystem::remove(path_, ec);
}

bool TempFile::write(const std::string& content) const {
    std::ofstream out(path_, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out << content;
    return static_cast<bool>(out);
}

}  // namespace codetrace::proc
