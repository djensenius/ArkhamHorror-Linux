// Minimal, non-QtTest standalone helper process used ONLY by
// AssetCacheTests' real-subprocess cross-process root-lock tests (see
// "Cross-process authority" in AssetCache.h's class comment, and
// acquireExclusiveRootOwnershipOrFailClosed()/
// releaseRootOwnershipRegistration() in AssetCache.cpp). A same-process
// second AssetCache instance always cooperates with a live sibling over
// the same root (by design), so proving a genuinely DIFFERENT process is
// correctly denied requires an actual second OS process -- this program
// IS that second process.
//
// Protocol (deliberately simple and fully deterministic -- no sleeps, no
// timing assumptions):
//   argv[1]            -- the cache directory to construct an
//                          Arkham::AssetCache against.
//   stdout, on startup -- exactly one line, either:
//     "LOCK-HOLDER-READY"             (disk cache successfully enabled;
//                                      this process now holds the
//                                      root's exclusive cross-process
//                                      lock)
//     "LOCK-HOLDER-FAILED-TO-ACQUIRE" (disk cache did NOT become
//                                      enabled -- signals a broken test
//                                      precondition, never silently
//                                      mistaken by the parent test for a
//                                      "second process correctly
//                                      denied" result, which the parent
//                                      instead observes directly on ITS
//                                      OWN AssetCache instance)
//   stdin              -- the parent writes a line (or closes stdin)
//                          exactly when it wants this process to exit,
//                          releasing the lock (via AssetCache's own
//                          destructor, reached via normal `main()`
//                          return) deterministically under the
//                          parent's own control.

#include "AssetCache.h"

#include <QCoreApplication>

#include <cstdio>
#include <iostream>
#include <string>

int main(int argc, char *argv[]) {
  QCoreApplication app(argc, argv);
  if (argc < 2) {
    std::fprintf(stderr,
                  "usage: %s <cache-directory>\n", argc > 0 ? argv[0] : "");
    return 2;
  }

  Arkham::AssetCache::Config config;
  config.directory = QString::fromUtf8(argv[1]);
  Arkham::AssetCache cache(config);

  if (cache.isDiskCacheDisabledForTesting()) {
    std::printf("LOCK-HOLDER-FAILED-TO-ACQUIRE\n");
    std::fflush(stdout);
    return 1;
  }

  std::printf("LOCK-HOLDER-READY\n");
  std::fflush(stdout);

  // Block until the parent signals shutdown (a line on stdin, or stdin
  // closing/EOF) -- never a sleep or a fixed timeout, so this process's
  // lock-held duration is entirely under the parent test's own
  // deterministic control.
  std::string line;
  std::getline(std::cin, line);
  return 0;
}
