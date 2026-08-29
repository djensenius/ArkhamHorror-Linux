// Aggregator entry point for the single "dedicated test executable" for
// the native asset delivery/caching feature
// (djensenius/ArkhamHorror-Linux#17), following Qt's documented pattern
// for combining multiple QObject-derived test classes into one binary:
// each class is a self-contained private-slots test fixture with no
// QTEST_MAIN/QTEST_GUILESS_MAIN macro of its own, and this file drives
// each of them in turn via QTest::qExec(), aggregating their exit codes.
//
// A QCoreApplication is required (not merely a bare event loop) because
// several of these test classes exercise asynchronous QNetworkReply /
// QTcpServer / QTimer-based delivery through Qt::QueuedConnection, which
// needs a running event loop identical to production code's.

#include <QCoreApplication>
#include <QtTest>

#include "AssetCacheTests.h"
#include "AssetImageRequestTests.h"
#include "AssetLocaleDigestTests.h"
#include "AssetLocatorTests.h"
#include "AssetNetworkFetcher.h"
#include "AssetNetworkFetcherTests.h"
#include "AssetRequestCoordinatorTests.h"

int main(int argc, char **argv) {
  QCoreApplication app(argc, argv);

  // Test-only subprocess entry point (see
  // AssetNetworkFetcherTests::
  // jpegDecodeWarningDetectorFatalFallbackTerminatesProcessLikeQtDefaultHandler()):
  // when invoked with this exact sentinel argument, skip every normal
  // QTest suite entirely and instead call the [[noreturn]] test-only hook
  // directly, so a parent test process can observe this subprocess being
  // terminated abnormally rather than exiting cleanly.
  for (int i = 1; i < argc; ++i) {
    if (QString::fromLocal8Bit(argv[i]) ==
        QStringLiteral("--test-only-trigger-jpeg-fatal-abort")) {
      Arkham::AssetNetworkFetcher::
          triggerJpegDecodeWarningDetectorFatalMessageForTesting();
    }
  }

  int status = 0;

  {
    AssetLocatorTests test;
    status |= QTest::qExec(&test, argc, argv);
  }
  {
    AssetLocaleDigestTests test;
    status |= QTest::qExec(&test, argc, argv);
  }
  {
    AssetNetworkFetcherTests test;
    status |= QTest::qExec(&test, argc, argv);
  }
  {
    AssetCacheTests test;
    status |= QTest::qExec(&test, argc, argv);
  }
  {
    AssetRequestCoordinatorTests test;
    status |= QTest::qExec(&test, argc, argv);
  }
  {
    AssetImageRequestTests test;
    status |= QTest::qExec(&test, argc, argv);
  }

  return status;
}
