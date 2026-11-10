// CodeTrace — minimal portable "run a command and capture stdout".
#pragma once

#include <string>
#include <vector>

namespace codetrace::proc {

struct Result {
    bool started = false;
    int exitCode = -1;
    std::string output;   // stdout only; stderr is discarded
};

/// Runs argv[0] with arguments (each argument is quoted for the platform shell).
Result run(const std::vector<std::string>& argv);

/// A file in the system temp directory that is deleted when this object dies.
class TempFile {
public:
    explicit TempFile(const std::string& suffix);
    ~TempFile();
    TempFile(const TempFile&) = delete;
    TempFile& operator=(const TempFile&) = delete;

    const std::string& path() const { return path_; }
    bool write(const std::string& content) const;

private:
    std::string path_;
};

}  // namespace codetrace::proc
