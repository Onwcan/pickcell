// SPDX-License-Identifier: Apache-2.0
//
// pickcell-validation-package -- writes the frozen external-validation files.
//
// Regenerating must be idempotent: the committed files are the pre-registration,
// and a test asserts they still match what this produces. Running it is how the
// package is created; running it again must change nothing.
//
// Usage: pickcell-validation-package <output-dir>

#include <cstdio>
#include <fstream>
#include <string>

#include "cell/validation_package.hpp"

namespace {

bool write(const std::string& path, const std::string& content) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out) {
    std::fprintf(stderr, "could not open %s\n", path.c_str());
    return false;
  }
  out << content;
  return out.good();
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) {
    std::fprintf(stderr, "usage: %s <output-dir>\n", argv[0]);
    return 2;
  }
  const std::string dir = argv[1];
  const std::string version = std::to_string(pickcell::validation::kActiveVersion);

  const bool ok = write(dir + "/scenario-v" + version + ".json",
                        pickcell::validation::scenarioJson()) &&
                  write(dir + "/prediction-v" + version + ".json",
                        pickcell::validation::predictionJson()) &&
                  write(dir + "/braking-reference-trace-v" + version + ".csv",
                        pickcell::validation::referenceTraceCsv());
  if (!ok) {
    return 1;
  }
  std::printf("wrote the frozen package for %s v%s into %s\n",
              pickcell::validation::kScenarioId, version.c_str(), dir.c_str());
  return 0;
}
