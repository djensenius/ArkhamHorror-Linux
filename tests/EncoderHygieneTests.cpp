#include <QFile>
#include <QRegularExpression>
#include <QStringList>
#include <QtTest>

using namespace Qt::StringLiterals;

// Standing guard against the exact class of bug the cumulative reviews on
// issue #19/PR #20 kept re-finding across rounds: a public response/request
// encoder in the pinned contract domain (CardCatalog.h/.cpp, Decks.h/.cpp,
// Games.h/.cpp) hand-building a QJsonObject/QJsonArray by declaring one
// locally and calling QJsonObject::insert()/QJsonArray::append() on it
// piecemeal, silently bypassing the resource-limit/duplicate-key/embedded-
// Undefined/lone-surrogate invariants toExactQJson()/toExactQJsonObject()/
// toExactQJsonArray() (RawJson.h) enforce. Every encoder in this domain was
// rewritten this round to instead compose a complete Json::Value AST (via
// toRawJson()) and convert it exactly ONCE via one of those three exact
// converters (see e.g. CardDef::toJson(), Deck::toJson(), GameState::
// toJson()) -- this test reads the actual source text of those three
// domain implementation files (plus their headers, since a future inline
// header-defined encoder is just as reachable) at build/test time and
// fails if that "declare a mutable QJsonObject/QJsonArray, then insert/
// append into it" shape ever reappears anywhere in them, rather than
// relying solely on code-review vigilance to catch the next regression.
//
// This is deliberately a narrow, textual (not full-C++-parse) invariant --
// see hasHandBuiltJsonContainer()'s own doc comment for exactly what it
// does and does not catch -- but it is precise enough that it currently
// passes with zero matches against the real source tree (proven by
// domainFilesContainNoHandBuiltJsonContainers() below) and demonstrably
// fires on a real violation shape (proven by
// checkerDetectsARealHandBuiltObjectViolation() and
// checkerDetectsARealHandBuiltArrayViolation()), so it is not vacuous.
class EncoderHygieneTests final : public QObject {
  Q_OBJECT

private slots:
  void domainFilesContainNoHandBuiltJsonContainers();
  void checkerIgnoresLegitimateDecodeSideByRefParameters();
  void checkerIgnoresExactConverterReturnTypeDeclarations();
  void checkerDetectsARealHandBuiltObjectViolation();
  void checkerDetectsARealHandBuiltArrayViolation();
};

namespace {

// Matches `QJsonObject <name><one of `{`, `(`, `;`, `=`>` or the QJsonArray
// equivalent -- i.e. a local variable/member DEFINITION (with or without
// an initializer, or a default constructor call), which is exactly the
// "declare it, then insert/append into it piecemeal" shape every encoder
// in this domain was rewritten to stop using. Deliberately does NOT match:
//   * `const QJsonObject &obj` (a by-reference decode-side parameter --
//     the identifier there is directly preceded by `&`, not whitespace,
//     so `\s+\w+` never matches across the `&`);
//   * `QJsonObject obj)`/`QJsonObject obj,` (a by-value parameter in a
//     declaration/definition's parameter list -- the character after the
//     identifier is `)` or `,`, neither of which is in the trailing
//     `[{(;=]` class);
//   * `ValueOrError<QJsonObject> Foo::toJson()` (a return type -- the
//     character immediately after `QJsonObject` is `>`, which fails the
//     required `\s+` before the identifier).
// Applied only to each line's text BEFORE any trailing `//` comment (this
// codebase's universal comment style; verified via `grep -c "/\\*"` across
// these six files returning only single-line `/*paramName=*/`-style
// annotations, never a real multi-line block comment, so per-line `//`
// stripping alone is a safe simplification here -- see the class doc
// comment above).
const QRegularExpression &handBuiltContainerPattern() {
  static const QRegularExpression pattern(
      uR"(\bQJson(?:Object|Array)\s+[A-Za-z_]\w*\s*[{(;=])"_s);
  return pattern;
}

// Returns every "path:lineNumber: line text" match of
// handBuiltContainerPattern() found in `text` once each line's trailing
// `//` comment (if any) has been stripped. `path` is used only to make a
// failure message actionable, never parsed.
QStringList hasHandBuiltJsonContainer(const QString &path,
                                      const QString &text) {
  QStringList hits;
  const QStringList lines = text.split(u'\n');
  for (qsizetype i = 0; i < lines.size(); ++i) {
    const QString code = lines[i].left(lines[i].indexOf(u"//"_s) < 0
                                           ? lines[i].size()
                                           : lines[i].indexOf(u"//"_s));
    if (handBuiltContainerPattern().match(code).hasMatch())
      hits.append(u"%1:%2: %3"_s.arg(path).arg(i + 1).arg(lines[i].trimmed()));
  }
  return hits;
}

QString readFileOrFail(const QString &path) {
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
    qFatal("EncoderHygieneTests: could not open %s (test setup, not "
           "production code)",
           qPrintable(path));
  return QString::fromUtf8(file.readAll());
}

} // namespace

void EncoderHygieneTests::domainFilesContainNoHandBuiltJsonContainers() {
  // ARKHAM_TEST_SRC_DIR is this repository's src/ directory (see
  // CMakeLists.txt's arkham-encoder-hygiene-tests target), analogous to
  // ARKHAM_TEST_CONTRACTS_DIR used by the contract-provenance-adjacent
  // tests. Every file the cumulative reviews' findings named directly
  // (CardCatalog/Decks/Games, both header and implementation, since an
  // inline header-defined encoder would be just as reachable) is covered.
  static const QStringList domainFiles = {
      u"CardCatalog.h"_s, u"CardCatalog.cpp"_s, u"Decks.h"_s,
      u"Decks.cpp"_s,     u"Games.h"_s,         u"Games.cpp"_s,
  };
  QStringList allHits;
  for (const QString &name : domainFiles) {
    const QString path = QStringLiteral(ARKHAM_TEST_SRC_DIR) + u'/' + name;
    allHits += hasHandBuiltJsonContainer(path, readFileOrFail(path));
  }
  QVERIFY2(allHits.isEmpty(),
           qPrintable(u"found hand-built QJsonObject/QJsonArray "_s
                      u"declaration(s) in a pinned-contract domain encoder "_s
                      u"file -- every response/request encoder in this "_s
                      u"domain must instead compose a complete Json::Value "_s
                      u"AST and convert it exactly once via "_s
                      u"toExactQJson()/toExactQJsonObject()/"_s
                      u"toExactQJsonArray() (see RawJson.h):\n"_s +
                      allHits.join(u'\n')));
}

void EncoderHygieneTests::checkerIgnoresLegitimateDecodeSideByRefParameters() {
  // Decode-side functions across this domain legitimately take
  // `const QJsonObject &obj` (by reference) as their input parameter --
  // this must never be flagged, or the checker would reject the very
  // decode functions the "closed object"/exact-key fixes in this same
  // round depend on.
  const QStringList hits = hasHandBuiltJsonContainer(
      u"synthetic"_s,
      u"ValueOrError<CardDef> CardDef::fromJson(const QJsonObject &obj,\n"_s
      u"                                        QStringView path) {\n"_s);
  QVERIFY(hits.isEmpty());
}

void EncoderHygieneTests::checkerIgnoresExactConverterReturnTypeDeclarations() {
  // The fallible exact-converter return type shape every encoder in this
  // domain now uses (`ValueOrError<QJsonObject> Foo::toJson() const {`)
  // must never be flagged either -- only an actual local hand-built
  // container declaration should be.
  const QStringList hits = hasHandBuiltJsonContainer(
      u"synthetic"_s, u"ValueOrError<QJsonObject> CardDef::toJson() const {\n"_s
                      u"  return toRawJson().toExactQJsonObject();\n"_s
                      u"}\n"_s);
  QVERIFY(hits.isEmpty());
}

void EncoderHygieneTests::checkerDetectsARealHandBuiltObjectViolation() {
  // Proves the checker is not vacuous: a synthetic reproduction of
  // exactly the pre-fix shape (declare a QJsonObject, then insert into it
  // piecemeal) this test exists to prevent must be caught. Uses the
  // already-fixed fallible ValueOrError<QJsonObject> signature shape for
  // the enclosing method (see checkerIgnoresExactConverterReturnTypeDecl
  // arations() above) so this test isolates the LOCAL declaration
  // violation specifically, rather than also exercising the separate
  // (also real, also desirable) "bare non-fallible QJsonObject return
  // type" shape.
  const QStringList hits = hasHandBuiltJsonContainer(
      u"synthetic"_s, u"ValueOrError<QJsonObject> Foo::toJson() const {\n"_s
                      u"  QJsonObject object;\n"_s
                      u"  object.insert(\"name\"_L1, m_name);\n"_s
                      u"  return object;\n"_s
                      u"}\n"_s);
  QCOMPARE(hits.size(), 1);
  QVERIFY(hits.first().contains(u"QJsonObject object;"_s));
}

void EncoderHygieneTests::checkerDetectsARealHandBuiltArrayViolation() {
  const QStringList hits = hasHandBuiltJsonContainer(
      u"synthetic"_s, u"ValueOrError<QJsonArray> Foo::toJson() const {\n"_s
                      u"  QJsonArray array;\n"_s
                      u"  for (const auto &row : m_rows)\n"_s
                      u"    array.append(row);\n"_s
                      u"  return array;\n"_s
                      u"}\n"_s);
  QCOMPARE(hits.size(), 1);
  QVERIFY(hits.first().contains(u"QJsonArray array;"_s));
}

QTEST_MAIN(EncoderHygieneTests)
#include "EncoderHygieneTests.moc"
