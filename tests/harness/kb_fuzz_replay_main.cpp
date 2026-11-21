/**
 * @file kb_fuzz_replay_main.cpp
 * @brief Runs a fuzz target over saved inputs, for compilers without libFuzzer.
 *
 * `fuzz_replay_<target> <file-or-directory>...` feeds every file to LLVMFuzzerTestOneInput() once.
 * CTest runs it over the target's seed corpus, so every input the fuzzer kept and every crash it
 * found stays a regression test on GCC and Clang. Host-only test code: it uses the standard library
 * freely.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 The kb-core authors
 */
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <system_error>
#include <vector>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, std::size_t size);

namespace {

bool RunFile(const std::filesystem::path& path) {
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    static_cast<void>(std::fprintf(stderr, "cannot open %s\n", path.string().c_str()));
    return false;
  }
  const std::vector<uint8_t> bytes{std::istreambuf_iterator<char>(file),
                                   std::istreambuf_iterator<char>()};
  static_cast<void>(LLVMFuzzerTestOneInput(bytes.data(), bytes.size()));
  return true;
}

} // namespace

int main(int argc, char** argv) {
  int files = 0;
  for (int i = 1; i < argc; ++i) {
    const std::filesystem::path arg(argv[i]);
    std::error_code ec;
    if (std::filesystem::is_directory(arg, ec)) {
      for (const auto& entry : std::filesystem::directory_iterator(arg, ec)) {
        const std::string name = entry.path().filename().string();
        if (entry.is_regular_file(ec) && (!name.empty()) && (name.front() != '.') &&
            (name != "README.md")) {
          files += RunFile(entry.path()) ? 1 : 0;
        }
      }
    } else if (RunFile(arg)) {
      ++files;
    }
  }
  static_cast<void>(std::printf("replayed %d inputs\n", files));
  return (files > 0) ? 0 : 1;
}
