#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSet>
#include <QTemporaryDir>
#include <QTextStream>
#include <QtTest>

#include <algorithm>

using namespace Qt::StringLiterals;

// Standing guard against the exact class of bug the cumulative reviews on
// issue #19/PR #20 kept re-finding across rounds: a public response/request
// encoder silently bypassing the resource-limit/duplicate-key/embedded-
// Undefined/lone-surrogate invariants Value::toExactQJson()/
// toExactQJsonObject()/toExactQJsonArray() (RawJson.h) enforce.
//
// Earlier versions of this test tried to police that by pattern-matching
// specific *shapes* a lossy encoder's body might take (a hand-built local
// QJsonObject, a direct return, an adapter-call token appearing somewhere
// in an extracted body, an overload/file/name allowlist keyed by a
// "file#type#name" identity). Every round found a new shape or a new
// identity-collapsing edge case the matcher didn't cover -- most recently:
// two overloads sharing a name being deduplicated by that identity key,
// and a comment merely *mentioning* the adapter being enough to satisfy a
// body-substring check.
//
// This version does not try to out-clever the next shape. It structurally
// removes the prohibited API surface instead of policing it: no public
// method or free function in the CardCatalog/Decks/Games/Identifiers
// domain model may be *declared* to return QJsonObject/QJsonArray/
// QJsonValue (bare or wrapped in ValueOrError<>/std::optional<>), full
// stop. Domain values expose only toRawJson()/toJsonBytes(); the one
// place permitted to turn a validated Json::Value into a Qt JSON type is
// the central, already-exact-checked Value::toExactQJson() family in
// RawJson.h, which tests/DomainJsonTestAdapter.h composes with each
// value's toRawJson() for every test assertion in this codebase.
//
// Given that, the only thing left to check is *absence*: no domain
// header may contain a declaration of that shape at all. That check does
// not care how many overloads share a name, whether the declaration is a
// direct return or `auto`-deduced, whether it is public or private (this
// codebase's domain headers have no legitimate need for the shape at
// any access level), or whether a comment nearby happens to say the word
// "toExactQJson" -- there is no body to fool it, because nothing here
// looks at bodies at all.
//
//   findViolations() strips block/line comments and string/char literals
//   (so neither can smuggle a false match, in either direction -- see
//   scannerIgnoresACommentMentioningABannedType() and
//   scannerIgnoresAStringLiteralMentioningABannedType()) and then flags,
//   anywhere in the remaining text:
//     (a) a same-line or multi-line "TYPE name(" / "ValueOrError<TYPE> name("
//         / "std::optional<TYPE> name(" return-type-then-name-then-paren
//         adjacency, for TYPE in {QJsonObject, QJsonArray, QJsonValue};
//     (b) a trailing "-> TYPE" return type, optionally wrapped the same way;
//     (c) a `using X = ...TYPE...;` / `typedef ... TYPE ...;` alias that
//         would let a banned type hide behind a new name;
//     (d) any bare deduced-`auto` function declaration with no trailing
//         `-> Type` at all -- this codebase's domain headers have no
//         legitimate need for a deduced return type, so a declaration
//         shape that could hide *any* return type (banned or not) behind
//         `auto` is refused outright, independently of (a)-(c).
//
//   discoverHeaderFiles() lists every *.h file under a directory via QDir
//   at test run time, not a fixed list, so a brand new file is scanned
//   the very next run with no test-source change (see
//   scannerFlagsANewlyAddedFileAutomatically(), which proves this against
//   a scratch QTemporaryDir rather than mutating the real src/ tree).
//
//   noDomainHeaderDeclaresAJsonReturningEncoder() applies findViolations()
//   to every src/*.h file except the three narrow, explicitly named
//   adapters that must legitimately return Qt JSON types: RawJson.h (the
//   central exact converter itself), JsonDecode.h (decode-only narrowing
//   helpers that consume, not produce, request/response JSON), and the
//   pre-existing, out-of-scope AuthModels.h (see
//   NetworkAuthenticationClient.cpp; unrelated to issue #19's
//   CardCatalog/Decks/Games/Identifiers domain).
class EncoderHygieneTests final : public QObject {
  Q_OBJECT

private slots:
  void noDomainHeaderDeclaresAJsonReturningEncoder();
  void exemptAdapterHeadersAreExactlyThreeNamedFiles();

  void scannerFlagsASameLineReturnTypeDeclaration();
  void scannerFlagsAValueOrErrorWrappedReturnTypeDeclaration();
  void scannerFlagsAnOptionalWrappedReturnTypeDeclaration();
  void scannerFlagsAMultiLineReturnTypeDeclaration();
  void scannerFlagsATrailingReturnTypeDeclaration();
  void scannerFlagsATrailingValueOrErrorWrappedReturnTypeDeclaration();
  void scannerFlagsAUsingAliasOfABannedContainer();
  void scannerFlagsATypedefOfABannedContainer();
  void scannerFlagsABareDeducedAutoFunctionRegardlessOfItsBody();
  void scannerFlagsBothOverloadsSharingAName();
  void scannerFlagsADeclarationSplitByAnEmptyBlockComment();
  void scannerFlagsADeclarationSplitByAnEmptyLineComment();
  void scannerFlagsAReferenceReturnTypeDeclaration();
  void scannerFlagsAPointerReturnTypeDeclaration();
  void scannerIgnoresALegitimateTrailingReturnType();
  void scannerIgnoresACommentMentioningABannedType();
  void scannerIgnoresAStringLiteralMentioningABannedType();
  void scannerIgnoresAByValueOrByRefParameterUsage();
  void scannerIgnoresAnUnnamedParameter();
  void scannerFlagsANewlyAddedFileAutomatically();
};

namespace {

struct Violation {
  QString file;
  int line = 0;
  QString rule;
  QString snippet;
};

QString describe(const Violation &v) {
  return u"%1:%2: [%3] %4"_s.arg(v.file).arg(v.line).arg(v.rule,
                                                         v.snippet.trimmed());
}

QString describeAll(const QList<Violation> &violations) {
  QStringList lines;
  lines.reserve(violations.size());
  for (const Violation &v : violations)
    lines.append(describe(v));
  return lines.join(u'\n');
}

// Comments and string/char literals are replaced with equal-length
// whitespace (preserving newlines, so downstream line numbers stay
// accurate) rather than deleted outright, so neither can ever
// contribute a false match in either direction: a banned type merely
// *mentioned* in prose or a literal cannot trip the scanner, and the
// scanner cannot be fooled into treating a real declaration as "just a
// comment" by a nearby unrelated one.
QString stripCommentsAndLiterals(const QString &source) {
  QString out;
  out.reserve(source.size());
  const qsizetype n = source.size();
  qsizetype i = 0;
  while (i < n) {
    const QChar c = source.at(i);
    if (c == u'/' && i + 1 < n && source.at(i + 1) == u'/') {
      // Every character of a "//" comment, including its own leading
      // delimiter, becomes a space -- not merely skipped -- so a
      // same-line comment with little or no content (e.g. "//") still
      // separates whatever tokens surrounded it by at least one space
      // after stripping, exactly as it did before stripping, rather
      // than only the comment's *content* becoming whitespace while its
      // delimiter silently vanishes and could concatenate the tokens
      // on either side of it.
      while (i < n && source.at(i) != u'\n') {
        out.append(u' ');
        ++i;
      }
      continue;
    }
    if (c == u'/' && i + 1 < n && source.at(i + 1) == u'*') {
      // Same principle for "/* */": both delimiters become whitespace
      // too, so an empty block comment ("/**/") between two tokens
      // still leaves 4 spaces of separation rather than 0 -- see
      // scannerFlagsADeclarationSplitByAnEmptyBlockComment() below,
      // which is exactly the false negative an unpatched version of
      // this function could produce.
      out.append(u' ');
      out.append(u' ');
      i += 2;
      while (i < n &&
             !(source.at(i) == u'*' && i + 1 < n && source.at(i + 1) == u'/')) {
        out.append(source.at(i) == u'\n' ? u'\n' : u' ');
        ++i;
      }
      if (i < n) {
        out.append(u' ');
        ++i;
      }
      if (i < n) {
        out.append(u' ');
        ++i;
      }
      continue;
    }
    if (c == u'"' || c == u'\'') {
      const QChar quote = c;
      out.append(u' ');
      ++i;
      while (i < n && source.at(i) != quote) {
        if (source.at(i) == u'\\' && i + 1 < n) {
          out.append(u' ');
          ++i;
        }
        out.append(source.at(i) == u'\n' ? u'\n' : u' ');
        ++i;
      }
      if (i < n) {
        out.append(u' ');
        ++i;
      }
      continue;
    }
    out.append(c);
    ++i;
  }
  return out;
}

int lineOf(const QString &strippedText, qsizetype pos) {
  return static_cast<int>(strippedText.left(pos).count(u'\n')) + 1;
}

QString snippetAt(const QString &strippedText, qsizetype start,
                  qsizetype length) {
  const qsizetype lineStart = strippedText.lastIndexOf(u'\n', start) + 1;
  qsizetype lineEnd = strippedText.indexOf(u'\n', start + length);
  if (lineEnd < 0)
    lineEnd = strippedText.size();
  return strippedText.mid(lineStart, lineEnd - lineStart).simplified();
}

// Matches a banned Qt JSON container name immediately followed (only
// whitespace, an optional generic-wrapper prefix/suffix, an optional
// `const`/reference/pointer return-type qualifier, and any number of
// closing '>' characters in between) by an identifier and its parameter
// list's opening '(' -- the textual shape of "this declaration's return
// type is (or wraps, or refers/points to) a banned type", independent of
// how many lines it spans and independent of a hidden body. A parameter
// usage such as `const QJsonValue &v` or `QJsonObject obj = QJsonObject()`
// never has an identifier *directly* followed by '(' immediately after
// the type name, so it cannot match this adjacency.
const QRegularExpression &returnTypePattern() {
  static const QRegularExpression re(
      uR"((?:(?:ValueOrError|std::optional)\s*<\s*)*\b(?:QJsonObject|QJsonArray|QJsonValue)\b\s*>*\s*(?:const\s*)?[&*]*\s*[A-Za-z_]\w*\s*\()"_s);
  return re;
}

const QRegularExpression &trailingReturnTypePattern() {
  static const QRegularExpression re(
      uR"(->\s*(?:(?:ValueOrError|std::optional)\s*<\s*)*\b(?:QJsonObject|QJsonArray|QJsonValue)\b)"_s);
  return re;
}

const QRegularExpression &aliasPattern() {
  static const QRegularExpression re(
      uR"((?:\busing\s+\w+\s*=|\btypedef\b)[^;]*\b(?:QJsonObject|QJsonArray|QJsonValue)\b[^;]*;)"_s);
  return re;
}

// A bare deduced-return function declaration: `auto name(...)` with no
// trailing `-> Type` anywhere before the terminating ';' or '{'. The
// parameter list is allowed one level of nested parens (e.g. a function-
// pointer parameter) via the alternation below; this codebase's domain
// headers need nothing deeper.
const QRegularExpression &bareAutoPattern() {
  static const QRegularExpression re(
      uR"(\bauto\s+[A-Za-z_]\w*\s*\((?:[^()]|\([^()]*\))*\)\s*(?:const\s*)?(?:noexcept\b(?:\s*\([^()]*\))?\s*)?(?:override\s*)?(?:final\s*)?(?=;|\{)(?!\s*->))"_s);
  return re;
}

void collect(const QString &strippedText, const QString &fileLabel,
             const QRegularExpression &pattern, const QString &rule,
             QList<Violation> &out) {
  auto it = pattern.globalMatch(strippedText);
  while (it.hasNext()) {
    const QRegularExpressionMatch m = it.next();
    out.append(Violation{
        fileLabel, lineOf(strippedText, m.capturedStart()), rule,
        snippetAt(strippedText, m.capturedStart(), m.capturedLength())});
  }
}

// The one function every test below (including the real policy check)
// goes through -- there is exactly one code path from "raw header text"
// to "list of violations", so a mutation test proving a given shape is
// caught proves it is caught for the real src/ scan too.
QList<Violation> findViolations(const QString &fileLabel,
                                const QString &rawSource) {
  const QString stripped = stripCommentsAndLiterals(rawSource);
  QList<Violation> violations;
  collect(stripped, fileLabel, returnTypePattern(), u"return-type"_s,
          violations);
  collect(stripped, fileLabel, trailingReturnTypePattern(),
          u"trailing-return-type"_s, violations);
  collect(stripped, fileLabel, aliasPattern(), u"type-alias"_s, violations);
  collect(stripped, fileLabel, bareAutoPattern(), u"bare-auto"_s, violations);
  return violations;
}

QList<Violation> findViolationsInFile(const QString &path) {
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
    return {Violation{path, 0, u"unreadable"_s,
                      u"could not open file for reading"_s}};
  QTextStream stream(&file);
  return findViolations(path, stream.readAll());
}

// Every *.h file directly under dir, discovered at run time (not a fixed
// list), mirroring how the actual src/ layout is scanned.
QStringList discoverHeaderFiles(const QString &dir) {
  QStringList paths;
  const QStringList names =
      QDir(dir).entryList(QStringList{u"*.h"_s}, QDir::Files, QDir::Name);
  paths.reserve(names.size());
  for (const QString &name : names)
    paths.append(QDir(dir).filePath(name));
  return paths;
}

// Basenames legitimately exempt from the "no Qt-JSON-returning
// declaration" policy -- see this file's class doc comment for why each
// one is necessary. Anything else under src/ is in scope, including any
// file added after this list was written.
QSet<QString> exemptHeaderBasenames() {
  return {u"RawJson.h"_s, u"JsonDecode.h"_s, u"AuthModels.h"_s};
}

QString writeTempHeader(QTemporaryDir &dir, const QString &name,
                        const QString &content) {
  const QString path = dir.filePath(name);
  QFile file(path);
  const bool opened = file.open(QIODevice::WriteOnly | QIODevice::Text);
  Q_ASSERT(opened);
  QTextStream stream(&file);
  stream << content;
  return path;
}

} // namespace

// --- The real policy, applied to this repository's actual src/ headers ---

void EncoderHygieneTests::noDomainHeaderDeclaresAJsonReturningEncoder() {
  const QString srcDir = QString::fromUtf8(ARKHAM_TEST_SRC_DIR);
  QVERIFY2(QDir(srcDir).exists(),
           qPrintable(u"missing src dir: %1"_s.arg(srcDir)));

  const QSet<QString> exempt = exemptHeaderBasenames();
  const QStringList headers = discoverHeaderFiles(srcDir);
  QVERIFY2(
      headers.size() > 30,
      "expected to discover the full src/ header set; discovery is broken");

  QList<Violation> violations;
  int scanned = 0;
  for (const QString &path : headers) {
    if (exempt.contains(QFileInfo(path).fileName()))
      continue;
    ++scanned;
    violations.append(findViolationsInFile(path));
  }
  QVERIFY2(
      scanned > 25,
      "expected most headers to be in scope; exemption list may be too broad");

  QVERIFY2(
      violations.isEmpty(),
      qPrintable(u"public Qt-JSON-returning declaration(s) found in domain "
                 "headers -- domain values must expose only toRawJson()/"
                 "toJsonBytes() and compose with the central "
                 "Value::toExactQJson() family instead:\n%1"_s.arg(
                     describeAll(violations))));
}

void EncoderHygieneTests::exemptAdapterHeadersAreExactlyThreeNamedFiles() {
  const QSet<QString> exempt = exemptHeaderBasenames();
  QCOMPARE(exempt.size(), 3);
  QVERIFY(exempt.contains(u"RawJson.h"_s));
  QVERIFY(exempt.contains(u"JsonDecode.h"_s));
  QVERIFY(exempt.contains(u"AuthModels.h"_s));

  // The exemption is matched by exact basename, not by content shape: the
  // same genuinely-legitimate-looking adapter declaration flags when it
  // appears under any other file name, proving the allowlist cannot be
  // satisfied merely by writing adapter-shaped code.
  const QString content =
      u"class C { [[nodiscard]] ValueOrError<QJsonObject> toExactQJsonObject() const; };\n"_s;
  const QList<Violation> underExemptName =
      findViolations(u"RawJson.h"_s, content);
  const QList<Violation> underOtherName =
      findViolations(u"NotExempt.h"_s, content);
  // The scanner itself does not consult the exemption list (that
  // filtering happens one layer up, in noDomainHeaderDeclaresA...()) --
  // it flags this shape unconditionally by file content alone.
  QCOMPARE(underExemptName.size(), underOtherName.size());
  QVERIFY(!underOtherName.isEmpty());
}

// --- Mutation tests: each proves one specific evasion vector still gets
//     caught (or, for the negative cases, is correctly never flagged) ---

void EncoderHygieneTests::scannerFlagsASameLineReturnTypeDeclaration() {
  const auto v = findViolations(
      u"T.h"_s, u"  [[nodiscard]] QJsonObject toJson() const;\n"_s);
  QCOMPARE(v.size(), 1);
  QCOMPARE(v.first().rule, u"return-type"_s);
}

void EncoderHygieneTests::
    scannerFlagsAValueOrErrorWrappedReturnTypeDeclaration() {
  const auto v = findViolations(
      u"T.h"_s,
      u"  [[nodiscard]] ValueOrError<QJsonArray> toRows() const;\n"_s);
  QCOMPARE(v.size(), 1);
  QCOMPARE(v.first().rule, u"return-type"_s);
}

void EncoderHygieneTests::scannerFlagsAnOptionalWrappedReturnTypeDeclaration() {
  const auto v = findViolations(
      u"T.h"_s,
      u"  [[nodiscard]] std::optional<QJsonValue> maybeRaw() const;\n"_s);
  QCOMPARE(v.size(), 1);
  QCOMPARE(v.first().rule, u"return-type"_s);
}

void EncoderHygieneTests::scannerFlagsAMultiLineReturnTypeDeclaration() {
  const auto v = findViolations(
      u"T.h"_s,
      u"[[nodiscard]] QJsonObject\nencodeGameList(const QList<GameListRow> &rows);\n"_s);
  QCOMPARE(v.size(), 1);
  QCOMPARE(v.first().rule, u"return-type"_s);
}

void EncoderHygieneTests::scannerFlagsATrailingReturnTypeDeclaration() {
  const auto v = findViolations(
      u"T.h"_s, u"  [[nodiscard]] auto toJson() const -> QJsonObject;\n"_s);
  // A bare-shape return-type match and the dedicated trailing-return-type
  // rule both legitimately see this (the identifier before '->' also
  // happens not to precede '(' directly, so only the trailing rule fires
  // here); assert on the trailing rule specifically being present.
  const bool sawTrailing =
      std::any_of(v.begin(), v.end(), [](const Violation &x) {
        return x.rule == u"trailing-return-type"_s;
      });
  QVERIFY(sawTrailing);
}

void EncoderHygieneTests::
    scannerFlagsATrailingValueOrErrorWrappedReturnTypeDeclaration() {
  const auto v = findViolations(
      u"T.h"_s,
      u"  [[nodiscard]] auto toRows() const -> ValueOrError<QJsonArray>;\n"_s);
  const bool sawTrailing =
      std::any_of(v.begin(), v.end(), [](const Violation &x) {
        return x.rule == u"trailing-return-type"_s;
      });
  QVERIFY(sawTrailing);
}

void EncoderHygieneTests::scannerFlagsAUsingAliasOfABannedContainer() {
  const auto v = findViolations(u"T.h"_s, u"using JsonMap = QJsonObject;\n"_s);
  QCOMPARE(v.size(), 1);
  QCOMPARE(v.first().rule, u"type-alias"_s);
}

void EncoderHygieneTests::scannerFlagsATypedefOfABannedContainer() {
  const auto v = findViolations(u"T.h"_s, u"typedef QJsonArray JsonList;\n"_s);
  QCOMPARE(v.size(), 1);
  QCOMPARE(v.first().rule, u"type-alias"_s);
}

void EncoderHygieneTests::
    scannerFlagsABareDeducedAutoFunctionRegardlessOfItsBody() {
  // No banned type name appears in the text at all; only the bare-auto
  // shape itself is enough to flag this, since the return type is
  // entirely hidden by deduction and this codebase's domain headers have
  // no legitimate need for that.
  const auto v =
      findViolations(u"T.h"_s, u"  auto toJson() const { return 5; }\n"_s);
  QCOMPARE(v.size(), 1);
  QCOMPARE(v.first().rule, u"bare-auto"_s);
}

void EncoderHygieneTests::scannerFlagsBothOverloadsSharingAName() {
  // The reported bypass: an overload sharing a name with an already-known
  // declaration must not be collapsed/deduped by any file#type#name-style
  // identity. There is no such identity key anywhere in this scanner --
  // every matched occurrence is reported independently.
  const auto v =
      findViolations(u"T.h"_s, u"  QJsonObject toJson() const;\n"
                               "  QJsonObject toJson(bool legacy) const;\n"_s);
  QCOMPARE(v.size(), 2);
}

void EncoderHygieneTests::scannerFlagsADeclarationSplitByAnEmptyBlockComment() {
  // A prior version of stripCommentsAndLiterals() silently skipped a
  // block comment's own "/*"/"*/" delimiters instead of turning them
  // into whitespace like the rest of the comment; for a short/empty
  // comment this could concatenate the return type and the name into a
  // single token ("QJsonObjecttoJson"), which the \b-bounded return-type
  // regex would then fail to match -- a genuine false negative for
  // otherwise-valid, declaration-splitting C++ like
  // `QJsonObject/**/toJson()`. Every character of both delimiters must
  // become whitespace so the boundary is always preserved.
  const auto v =
      findViolations(u"T.h"_s, u"  QJsonObject/**/toJson() const;\n"_s);
  QCOMPARE(v.size(), 1);
  QCOMPARE(v.first().rule, u"return-type"_s);

  const auto withContent =
      findViolations(u"T.h"_s, u"  QJsonObject/*x*/toJson() const;\n"_s);
  QCOMPARE(withContent.size(), 1);
  QCOMPARE(withContent.first().rule, u"return-type"_s);
}

void EncoderHygieneTests::scannerFlagsADeclarationSplitByAnEmptyLineComment() {
  // Same principle for "//": its own delimiter must become whitespace,
  // not vanish, even though a trailing "//" comment always runs to the
  // (separately preserved) newline in practice and so this specific form
  // did not previously reproduce the bug -- kept as an explicit parity
  // regression for the sibling block-comment fix.
  const auto v =
      findViolations(u"T.h"_s, u"  QJsonObject//\n  toJson() const;\n"_s);
  QCOMPARE(v.size(), 1);
  QCOMPARE(v.first().rule, u"return-type"_s);
}

void EncoderHygieneTests::scannerFlagsAReferenceReturnTypeDeclaration() {
  // Reported gap: a banned type returned by reference (or rvalue
  // reference) breaks the plain "type-then-whitespace-then-name"
  // adjacency the original regex required, letting it slip past
  // undetected even though it is exactly the same prohibited return
  // type. The qualifier is now tolerated between the type and the name.
  const auto lvalueRef =
      findViolations(u"T.h"_s, u"  QJsonObject& mutableJson();\n"_s);
  QCOMPARE(lvalueRef.size(), 1);
  QCOMPARE(lvalueRef.first().rule, u"return-type"_s);

  const auto rvalueRef =
      findViolations(u"T.h"_s, u"  ValueOrError<QJsonArray>&& takeRows();\n"_s);
  QCOMPARE(rvalueRef.size(), 1);
  QCOMPARE(rvalueRef.first().rule, u"return-type"_s);
}

void EncoderHygieneTests::scannerFlagsAPointerReturnTypeDeclaration() {
  const auto v =
      findViolations(u"T.h"_s, u"  const QJsonValue* g() const;\n"_s);
  QCOMPARE(v.size(), 1);
  QCOMPARE(v.first().rule, u"return-type"_s);
}

void EncoderHygieneTests::scannerIgnoresALegitimateTrailingReturnType() {
  const auto v = findViolations(
      u"T.h"_s, u"  [[nodiscard]] auto toRawJson() const -> Json::Value;\n"_s);
  QVERIFY(v.isEmpty());
}

void EncoderHygieneTests::scannerIgnoresACommentMentioningABannedType() {
  const auto v = findViolations(
      u"T.h"_s,
      u"  // Historically this returned QJsonObject toJson() const; not any more.\n"_s);
  QVERIFY(v.isEmpty());
}

void EncoderHygieneTests::scannerIgnoresAStringLiteralMentioningABannedType() {
  const auto v = findViolations(
      u"T.h"_s, u"  const char *note = \"QJsonObject toJson() const;\";\n"_s);
  QVERIFY(v.isEmpty());
}

void EncoderHygieneTests::scannerIgnoresAByValueOrByRefParameterUsage() {
  const auto v = findViolations(
      u"T.h"_s, u"  [[nodiscard]] static ValueOrError<CardDef> fromJson(const "
                u"QJsonValue &v, "
                "QStringView path);\n"
                "  void accept(QJsonObject obj = QJsonObject());\n"_s);
  QVERIFY(v.isEmpty());
}

void EncoderHygieneTests::scannerIgnoresAnUnnamedParameter() {
  const auto v = findViolations(u"T.h"_s, u"  void accept(QJsonObject);\n"_s);
  QVERIFY(v.isEmpty());
}

void EncoderHygieneTests::scannerFlagsANewlyAddedFileAutomatically() {
  // Proves dynamic discovery against a scratch directory (never mutating
  // the real src/ tree): a file this test has never named before is
  // still found by discoverHeaderFiles() and scanned.
  QTemporaryDir dir;
  QVERIFY(dir.isValid());
  writeTempHeader(dir, u"AlreadyClean.h"_s,
                  u"class Clean { public: int value() const; };\n"_s);
  const QString badPath = writeTempHeader(
      dir, u"BrandNewFile.h"_s,
      u"class New { public: QJsonObject toJson() const; };\n"_s);

  const QStringList discovered = discoverHeaderFiles(dir.path());
  QVERIFY(discovered.contains(badPath));

  QList<Violation> violations;
  for (const QString &path : discovered)
    violations.append(findViolationsInFile(path));
  QCOMPARE(violations.size(), 1);
  QCOMPARE(violations.first().file, badPath);
}

QTEST_MAIN(EncoderHygieneTests)
#include "EncoderHygieneTests.moc"
