#pragma once

#include <functional>

namespace Arkham {

// Which mode the current process invocation is running in. Kept separate
// from any command-line-parsing type so it can be constructed directly in
// tests without touching QCommandLineParser.
enum class ProcessMode {
  Normal,    ///< Ordinary interactive startup.
  SmokeTest, ///< --smoke-test: must be fully hermetic (see bootstrapSession).
};

// The entire hermetic guarantee for --smoke-test lives here: bootstrapSession
// invokes |composeSession| if and only if |mode| is ProcessMode::Normal.
// main() must route ALL production session composition (constructing
// QSettingsProfileStore, NetworkCapabilityProbe, QtKeychainTokenStore,
// NetworkAuthenticationClient, and SessionCoordinator, then calling
// SessionCoordinator::start()) exclusively through the |composeSession|
// callback passed here -- never directly in a SmokeTest branch -- so that
// as long as this function is used as intended, none of those objects (and
// therefore no QSettings access, network request, or keychain job) can ever
// be constructed when --smoke-test is requested. This function is pure and
// synchronous (no Qt event loop, network, QSettings, or keychain access of
// its own), so it can be unit-tested directly with a spy in place of a real
// composer -- see AppBootstrapTests.cpp -- rather than relying on a comment
// in main.cpp being kept accurate by hand.
inline void bootstrapSession(ProcessMode mode,
                             const std::function<void()> &composeSession) {
  if (mode == ProcessMode::Normal && composeSession) {
    composeSession();
  }
}

} // namespace Arkham
