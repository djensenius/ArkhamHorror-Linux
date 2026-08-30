#include <QDir>
#include <QFile>
#include <QRegularExpression>
#include <QSet>
#include <QStringList>
#include <QtTest>

using namespace Qt::StringLiterals;

// Standing guard against the exact class of bug the cumulative reviews on
// issue #19/PR #20 kept re-finding across rounds: a public response/request
// encoder silently bypassing the resource-limit/duplicate-key/embedded-
// Undefined/lone-surrogate invariants Value::toExactQJson()/
// toExactQJsonObject()/toExactQJsonArray() (RawJson.h) enforce.
//
// An earlier version of this test regexed for one specific hand-built-
// container *shape* ("declare a QJsonObject/QJsonArray local variable,
// then insert/append into it") across six hardcoded file names. A review
// round correctly identified that shape-matching as a syntax blind spot:
// a direct `return QJsonObject{...};` with no named local, an `auto`
// local, a type alias, a helper-function indirection, or simply a new
// file the hardcoded list never named would all evade it while still
// producing exactly the same class of bug.
//
// This version instead makes the prohibited API surface *declaration*-
// based, so no amount of cleverness in how a function's *body* is
// written can evade it:
//
//  1. discoverHeaderFiles()/discoverImplementationFiles() list every
//     *.h/*.cpp file under src/ at test run time (QDir, not a fixed
//     list) -- a brand new file is scanned the very next time this test
//     runs, with no CMakeLists.txt or test-source change required.
//  2. findPublicJsonReturningDeclarations() inventories every *public*
//     function declaration in those headers whose return type is (or
//     wraps) QJsonObject/QJsonArray/QJsonValue -- matching a same-line
//     "TYPE name(", a return type alone on its own line (the shape this
//     codebase uses for longer free-function declarations), and a
//     trailing "-> TYPE" return type, so it does not matter whether a
//     future encoder's signature happens to fit on one line. Decode-side
//     helpers (whose first parameter is itself JSON-typed -- e.g.
//     JsonDecode.h's requireObjectField(const QJsonObject &obj, ...))
//     and private/protected members are excluded by construction, not by
//     naming them.
//  3. publicJsonApiSurfaceMatchesTheReviewedAllowlist() below fails if
//     that inventory contains anything not in kAllowlistedEncoders (a new,
//     unreviewed public JSON-returning declaration -- anywhere, in any
//     file) or if kAllowlistedEncoders names something the inventory no
//     longer finds (a stale entry, e.g. after a rename). Because the gate
//     is the *existence of the declaration*, not a pattern match against
//     its body, a direct return, an auto local, or a private helper the
//     body delegates to cannot make a new encoder invisible to it -- see
//     the class doc comment's "does not matter how the body is written"
//     framing in the review this responds to.
//  4. allowlistedDomainEncodersComposeViaTheCentralAdapter() then checks,
//     for every allowlisted entry that is not itself the central adapter
//     (Value::toExactQJson()/toExactQJsonObject()/toExactQJsonArray()) or
//     an explicitly justified exempt fragment, that its *own* extracted
//     function body textually contains a call to one of those three
//     functions. Because this is a positive "does the body demonstrably
//     compose via the adapter" check rather than a negative "does the
//     body avoid one specific hand-built shape" check, direct return,
//     auto locals, and helper indirection all still have to show up
//     here: a body that instead calls an uninspected private helper to
//     do the real construction fails this check, since the adapter-call
//     token would not appear in the visible caller body.
//  5. noDomainHeaderDeclaresADeducedAutoEncoder() and
//     noDomainHeaderAliasesAQtJsonContainerType() close the two remaining
//     evasions no declaration-return-type regex alone can see through:
//     `auto toJson() const { ... }` (deduced, so the return type is not
//     textually present at all) and `using Json = QJsonObject;` (an
//     alias renaming the type could otherwise let it hide from).
class EncoderHygieneTests final : public QObject {
  Q_OBJECT

private slots:
  void publicJsonApiSurfaceMatchesTheReviewedAllowlist();
  void allowlistedDomainEncodersComposeViaTheCentralAdapter();
  void noDomainHeaderDeclaresADeducedAutoEncoder();
  void noDomainHeaderAliasesAQtJsonContainerType();
  void scannerFindsADirectReturnDeclaration();
  void scannerFindsAMultiLineFreeFunctionDeclaration();
  void scannerFindsATrailingReturnTypeDeclaration();
  void scannerIgnoresADecodeSideHelperWithAJsonFirstParameter();
  void scannerIgnoresAPrivateDeclaration();
  void scannerIgnoresAByRefDecodeParameter();
  void adapterCallCheckDetectsAHandBuiltBodyEvenWithAnAutoLocal();
  void adapterCallCheckDetectsHelperIndirectionThatHidesTheRealBuild();
};

namespace {

QString stripLineComment(const QString &line) {
  const qsizetype commentStart = line.indexOf(u"//"_s);
  return commentStart < 0 ? line : line.left(commentStart);
}

QString readFileOrFail(const QString &path) {
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
    qFatal("EncoderHygieneTests: could not open %s (test setup, not "
           "production code)",
           qPrintable(path));
  return QString::fromUtf8(file.readAll());
}

// Every *.h (or *.cpp) file directly under `srcDir`, discovered at test
// run time rather than named in a fixed list -- see the class doc
// comment's point 1.
QStringList discoverFiles(const QString &srcDir, const QString &suffix) {
  QDir dir(srcDir);
  const QStringList names =
      dir.entryList(QStringList{u"*"_s + suffix}, QDir::Files, QDir::Name);
  QStringList paths;
  paths.reserve(names.size());
  for (const QString &name : names)
    paths.append(dir.absoluteFilePath(name));
  return paths;
}

// A single public function declaration whose return type is (or wraps)
// QJsonObject/QJsonArray/QJsonValue, found by
// findPublicJsonReturningDeclarations() below. `enclosingType` is empty for a
// free function.
struct JsonReturningDeclaration {
  QString file;
  int line = 0;
  QString enclosingType;
  QString functionName;
  QString lineText;

  // Stable identity used both to compare against the allowlist and to
  // locate this declaration's implementation body -- deliberately not the
  // raw line text, which reformatting could change without altering the
  // declaration's actual public-API identity.
  [[nodiscard]] QString allowlistKey() const {
    return u"%1#%2#%3"_s.arg(file, enclosingType, functionName);
  }
};

// Matches a return type directly followed by a function name and the
// opening parenthesis of its parameter list on the SAME line, e.g.
// `[[nodiscard]] ValueOrError<QJsonObject> toJson() const;` or
// `ValueOrError<QJsonObject> SkillIcon::toJson() const {`. Deliberately
// requires whitespace directly before the identifier (so `&obj)` -- a
// by-reference decode-side *parameter* -- never matches: the character
// after the type there is `&`, not whitespace-then-letter) and the
// identifier to be immediately followed by `(` (so `QJsonObject obj)` --
// a by-value parameter -- never matches either: the character after the
// identifier there is `)`, not `(`).
const QRegularExpression &sameLineDeclarationPattern() {
  static const QRegularExpression pattern(
      uR"((?:\[\[nodiscard\]\]\s*)?(?:static\s+)?(?:ValueOrError<\s*)?)"
      uR"((?:QJsonObject|QJsonArray|QJsonValue)\s*>?\s+)"
      uR"(([A-Za-z_]\w*)((?:::[A-Za-z_]\w*)?)\s*\()"_s);
  return pattern;
}

// Matches a line whose ENTIRE trimmed content is just the return type
// (optionally `[[nodiscard]]`/`static`/`ValueOrError<...>`-wrapped), the
// shape this codebase uses for a free function declaration too long to
// fit its return type and name on one line, e.g. Games.h's
// `[[nodiscard]] ValueOrError<QJsonArray>` immediately followed by
// `encodeGameList(const QList<GameListRow> &rows);` on the next line.
const QRegularExpression &standaloneReturnTypePattern() {
  static const QRegularExpression pattern(
      uR"(^(?:\[\[nodiscard\]\]\s*)?(?:static\s+)?(?:ValueOrError<\s*)?)"
      uR"((?:QJsonObject|QJsonArray|QJsonValue)\s*>?$)"_s);
  return pattern;
}

// Matches a C++11 trailing return type naming one of these Qt JSON
// container types, e.g. `auto toJson() const -> QJsonObject {` -- a shape
// this codebase does not currently use, but one a same-line-prefix regex
// alone would never see, since the type appears after the parameter list
// rather than before the function name.
const QRegularExpression &trailingReturnTypePattern() {
  static const QRegularExpression pattern(
      uR"(->\s*(?:ValueOrError<\s*)?(?:QJsonObject|QJsonArray|QJsonValue)\s*>?)"_s);
  return pattern;
}

// A function whose return type is fully deduced (bare `auto`, no
// trailing `-> Type`) hides its return type from every regex above
// entirely -- there is no textual return-type token to match. Rather
// than attempt to infer the deduced type (which would require actually
// compiling the body), this codebase simply prohibits the shape for
// anything named like an encoder, so the return type must always be
// written out where this test (and a human reviewer) can see it.
const QRegularExpression &deducedAutoEncoderPattern() {
  static const QRegularExpression pattern(
      uR"(\bauto\s+(?:[A-Za-z_]\w*::)?(?:toJson|toRawJson|encode\w*)\s*\()"_s);
  return pattern;
}

// A `using Alias = QJsonObject;`-style alias would let a future
// declaration name a Qt JSON container type without the literal token
// `QJsonObject`/`QJsonArray`/`QJsonValue` ever appearing at its own
// declaration site, evading every pattern above. Prohibited outright
// rather than chased through every alias indirection.
const QRegularExpression &qtJsonContainerAliasPattern() {
  static const QRegularExpression pattern(
      uR"(\busing\s+[A-Za-z_]\w*\s*=\s*(?:QJsonObject|QJsonArray|QJsonValue)\b)"_s);
  return pattern;
}

// True if the declaration whose parameter list begins at `afterOpenParen`
// (the text starting at, and including, the function's own opening `(`)
// takes a JSON-typed value as its FIRST parameter -- the signature shape
// of a decode-side helper that extracts/validates an already-parsed
// fragment (e.g. JsonDecode.h's requireObject(const QJsonValue &v, ...))
// rather than an encoder that builds new JSON from domain state. Such a
// helper is correctly outside this test's scope: it introduces no new
// precision loss of its own, since its result is a sub-tree of a value
// that was already parsed (and, for the canonical Json::Value overloads,
// already resource-limit-checked) elsewhere.
bool firstParameterIsJsonTyped(QStringView afterOpenParen) {
  static const QRegularExpression pattern(
      uR"(^\(\s*(?:const\s+)?(?:QJsonValue|QJsonObject|QJsonArray|Json::Value)\b)"_s);
  return pattern.matchView(afterOpenParen).hasMatch();
}

// Extracts, from `window` (the candidate declaration's own line plus a
// few following lines -- long enough to always include the full
// parameter list even when the return type alone occupied the first
// line), the identifier immediately followed by `(` -- i.e. the
// function's own name, and (if qualified, only ever true for a .cpp
// definition, never a header declaration) the class it belongs to.
struct ParsedName {
  QString enclosingType;
  QString functionName;
};

ParsedName parseFunctionName(const QString &window) {
  const auto m = sameLineDeclarationPattern().match(window);
  if (!m.hasMatch())
    return {};
  const QString qualifier = m.captured(2); // "" or "::Name"
  if (qualifier.isEmpty())
    return {QString(), m.captured(1)};
  return {m.captured(1), qualifier.mid(2)};
}

// Scans `text` (already the full content of `file`) for every PUBLIC
// function declaration whose return type is one of the three Qt JSON
// container types, tracking class/struct nesting and the current access
// section with a small brace-depth state machine: a `struct` defaults to
// public, a `class` defaults to private, and an explicit `public:`/
// `private:`/`protected:` line updates the innermost open class/struct's
// access until the next such line or until that class/struct's own
// closing brace is reached. A free function at namespace scope (no
// enclosing class/struct on the stack) is always in scope, matching how
// C++ access specifiers work.
QList<JsonReturningDeclaration>
findPublicJsonReturningDeclarations(const QString &file, const QString &text) {
  QList<JsonReturningDeclaration> hits;
  const QStringList rawLines = text.split(u'\n');
  QStringList lines;
  lines.reserve(rawLines.size());
  for (const QString &raw : rawLines)
    lines.append(stripLineComment(raw));

  static const QRegularExpression classOrStructOpen(
      uR"(\b(class|struct)\s+([A-Za-z_]\w*))"_s);
  static const QRegularExpression publicSpecifier(uR"(^public\s*:$)"_s);
  static const QRegularExpression privateSpecifier(uR"(^private\s*:$)"_s);
  static const QRegularExpression protectedSpecifier(uR"(^protected\s*:$)"_s);

  struct Frame {
    QString typeName; // empty: not a class/struct body (namespace, fn body,
                      // enum, ...)
    QString access;   // "public" | "private" | "protected"
  };
  QList<Frame> stack;

  for (qsizetype i = 0; i < lines.size(); ++i) {
    const QString trimmed = lines[i].trimmed();
    const QString enclosing =
        stack.isEmpty() ? QString() : stack.last().typeName;
    const QString access = stack.isEmpty() ? u"public"_s : stack.last().access;

    if (publicSpecifier.match(trimmed).hasMatch()) {
      if (!stack.isEmpty())
        stack.last().access = u"public"_s;
    } else if (privateSpecifier.match(trimmed).hasMatch()) {
      if (!stack.isEmpty())
        stack.last().access = u"private"_s;
    } else if (protectedSpecifier.match(trimmed).hasMatch()) {
      if (!stack.isEmpty())
        stack.last().access = u"protected"_s;
    } else if (access == u"public"_s) {
      const QString window = [&] {
        QStringList w;
        for (qsizetype j = i; j < std::min(i + 6, lines.size()); ++j)
          w.append(lines[j]);
        return w.join(u'\n');
      }();
      const bool sameLine =
          sameLineDeclarationPattern().match(trimmed).hasMatch();
      const bool standalone =
          standaloneReturnTypePattern().match(trimmed).hasMatch();
      const bool trailing =
          trailingReturnTypePattern().match(trimmed).hasMatch();
      if (sameLine || standalone || trailing) {
        const qsizetype paren = window.indexOf(u'(');
        const bool isDecodeHelper =
            paren >= 0 &&
            firstParameterIsJsonTyped(QStringView(window).mid(paren));
        if (!isDecodeHelper) {
          const ParsedName parsed = parseFunctionName(window);
          JsonReturningDeclaration decl;
          decl.file = file;
          decl.line = static_cast<int>(i + 1);
          decl.enclosingType =
              parsed.enclosingType.isEmpty() ? enclosing : parsed.enclosingType;
          decl.functionName = parsed.functionName;
          decl.lineText = trimmed;
          hits.append(decl);
        }
      }
    }

    // Update the class/struct nesting stack for subsequent lines. A
    // `struct Tag {};` one-liner (or an inline single-line encoder body,
    // e.g. Identifiers.h's `QJsonValue toJson() const { return m_value; }`)
    // opens and closes within the same line and so never affects any
    // later line's enclosing frame.
    const qsizetype firstBrace = lines[i].indexOf(u'{');
    const QString beforeFirstBrace =
        firstBrace >= 0 ? lines[i].left(firstBrace) : lines[i];
    auto classMatch = firstBrace >= 0
                          ? classOrStructOpen.match(beforeFirstBrace)
                          : QRegularExpressionMatch();
    for (const QChar ch : lines[i]) {
      if (ch == u'{') {
        if (classMatch.hasMatch()) {
          const bool isClassKeyword = classMatch.captured(1) == u"class"_s;
          stack.append({classMatch.captured(2),
                        isClassKeyword ? u"private"_s : u"public"_s});
          classMatch = QRegularExpressionMatch();
        } else {
          stack.append({QString(), u"public"_s});
        }
      } else if (ch == u'}') {
        if (!stack.isEmpty())
          stack.removeLast();
      }
    }
  }
  return hits;
}

// The complete, reviewed inventory of every public function declaration
// (anywhere under src/*.h) whose return type is (or wraps) one of Qt's
// JSON container types, keyed by "file#enclosingType#functionName" (see
// JsonReturningDeclaration::allowlistKey()). A NEW entry appearing here
// that findPublicJsonReturningDeclarations() does not also find is a
// stale allowlist entry; a declaration findPublicJsonReturningDeclarations()
// finds that is not listed here is an unreviewed new public JSON-
// returning encoder -- both fail publicJsonApiSurfaceMatchesTheReviewed
// Allowlist() below, forcing a deliberate, reviewed addition/removal
// rather than letting either drift silently.
const QSet<QString> &allowlistedEncoders() {
  static const QSet<QString> keys = {
      // The central adapter itself (RawJson.h) -- every other entry below
      // is expected to compose through one of these three.
      u"RawJson.h#Value#toExactQJson"_s,
      u"RawJson.h#Value#toExactQJsonObject"_s,
      u"RawJson.h#Value#toExactQJsonArray"_s,
      // Pre-existing authentication request encoders (AuthModels.h/.cpp),
      // predating issue #19/this contract-pinning domain entirely: email/
      // username/password are plain, backend-unconstrained strings with
      // no pinned-schema exactness contract of their own, so these are
      // inventoried (a NEW public encoder appearing here still requires a
      // reviewed addition) but not required to route through the central
      // adapter the way every CardCatalog/Decks/Games/Identifiers
      // encoder below must.
      u"AuthModels.h#AuthenticateRequest#toJson"_s,
      u"AuthModels.h#RegisterRequest#toJson"_s,
      // Identifiers.h
      u"Identifiers.h#CardCode#toJson"_s,
      u"Identifiers.h#CardName#toJson"_s,
      u"Identifiers.h#NonEmptyString#toJson"_s,
      // TypedId::toJson() is the one deliberately non-fallible, non-
      // adapter-routed identifier fragment encoder left in this file --
      // see its own doc comment in Identifiers.h: parse()/fromJson() are
      // its only construction paths and both always store a canonical,
      // fixed-length QUuid::WithoutBraces string, so a lone/mismatched
      // UTF-16 surrogate or over-length value is structurally
      // unreachable, not merely unvalidated.
      u"Identifiers.h#TypedId#toJson"_s,
      // CardCatalog.h/.cpp
      u"CardCatalog.h#SkillIcon#toJson"_s,
      u"CardCatalog.h#CardCost#toJson"_s,
      u"CardCatalog.h#GameValue#toJson"_s,
      u"CardCatalog.h#CardDef#toJson"_s,
      // Decks.h/.cpp
      u"Decks.h#ExternalDeckId#toJson"_s,
      u"Decks.h#DeckListInput#toJson"_s,
      u"Decks.h#DeckList#toJson"_s,
      u"Decks.h#Deck#toJson"_s,
      u"Decks.h#CreateDeckRequest#toJson"_s,
      u"Decks.h#FetchDeckRequest#toJson"_s,
      u"Decks.h#DeckValidationError#toJson"_s,
      u"Decks.h#DeckValidationResult#toJson"_s,
      u"Decks.h#DeckOperationError#toJson"_s,
      // Games.h/.cpp
      u"Games.h#InvestigatorSummary#toJson"_s,
      u"Games.h#ScenarioSummary#toJson"_s,
      u"Games.h#CampaignSummary#toJson"_s,
      u"Games.h#GameState#toJson"_s,
      u"Games.h#GameListRow#toJson"_s,
      u"Games.h##encodeGameList"_s,
      u"Games.h#CampaignOption#toJson"_s,
      u"Games.h#CampaignOptionRequest#toJson"_s,
      u"Games.h#CreateGameRequest#toJson"_s,
      u"Games.h#ChooseDeckRequest#toJson"_s,
      u"Games.h#ClaimSeatRequest#toJson"_s,
      u"Games.h##encodeOpenSeats"_s,
  };
  return keys;
}

// The subset of allowlistedEncoders() that IS the central adapter --
// exempt from allowlistedDomainEncodersComposeViaTheCentralAdapter()'s
// "calls the adapter" check since these functions are what every other
// entry composes through, not a caller of themselves.
const QSet<QString> &centralAdapterKeys() {
  static const QSet<QString> keys = {
      u"RawJson.h#Value#toExactQJson"_s,
      u"RawJson.h#Value#toExactQJsonObject"_s,
      u"RawJson.h#Value#toExactQJsonArray"_s,
  };
  return keys;
}

// The subset of allowlistedEncoders() explicitly exempted from the
// "calls the central adapter" requirement, each with its own narrow,
// audited justification recorded next to its allowlist entry above.
const QSet<QString> &exemptFromAdapterCallRequirement() {
  static const QSet<QString> keys = {
      u"AuthModels.h#AuthenticateRequest#toJson"_s,
      u"AuthModels.h#RegisterRequest#toJson"_s,
      u"Identifiers.h#TypedId#toJson"_s,
  };
  return keys;
}

// Matches a local QJsonObject/QJsonArray variable DEFINITION (with or
// without an initializer) -- i.e. exactly the "declare it, then insert/
// append into it piecemeal" shape this domain's non-exempt encoders must
// never use. Applied only to an individual exempt entry's own already-
// located body text below (see allowlistedDomainEncodersComposeViaThe
// CentralAdapter()), as a narrow secondary check that an explicitly
// justified non-adapter-routed fragment has not grown a hand-built
// container since it was last audited -- not as this test's primary
// defense, which is the declaration-based allowlist above.
bool bodyContainsHandBuiltJsonContainer(const QString &body) {
  static const QRegularExpression pattern(
      uR"(\bQJson(?:Object|Array)\s+[A-Za-z_]\w*\s*[{(;=])"_s);
  for (const QString &line : body.split(u'\n'))
    if (pattern.match(stripLineComment(line)).hasMatch())
      return true;
  return false;
}

// Finds the matching closing brace for the `{` at `openBraceIndex` in
// `text`, returning -1 if `text` ends before it is found (malformed
// input, since every real function body in this codebase is complete).
qsizetype matchingCloseBrace(const QString &text, qsizetype openBraceIndex) {
  int depth = 0;
  for (qsizetype i = openBraceIndex; i < text.size(); ++i) {
    if (text[i] == u'{')
      ++depth;
    else if (text[i] == u'}') {
      --depth;
      if (depth == 0)
        return i;
    }
  }
  return -1;
}

struct FunctionBody {
  bool ok = false;
  QString text;
};

// Scans every match of `pattern` in `text` (there may be several: a
// header declaration, which ends in `;`, matches the same
// "TYPE name(" shape as its own out-of-line `.cpp` definition, which ends
// in `{`) and returns the body of the first one that is an actual
// definition -- skipping any `const`/`noexcept`/trailing-return-type text
// between the parameter list and whichever of `{`/`;` comes next to tell
// the two apart.
FunctionBody findDefinitionMatching(const QString &text,
                                    const QRegularExpression &pattern) {
  auto it = pattern.globalMatch(text);
  while (it.hasNext()) {
    const auto m = it.next();
    const qsizetype openParen = text.indexOf(u'(', m.capturedStart());
    if (openParen < 0)
      continue;
    // Skip the parameter list (no candidate declaration in this domain
    // nests a literal '(' inside its parameter types/defaults).
    qsizetype depth = 1;
    qsizetype i = openParen + 1;
    for (; i < text.size() && depth > 0; ++i) {
      if (text[i] == u'(')
        ++depth;
      else if (text[i] == u')')
        --depth;
    }
    qsizetype j = i;
    while (j < text.size() && text[j] != u'{' && text[j] != u';')
      ++j;
    if (j >= text.size() || text[j] != u'{')
      continue;
    const qsizetype closeBrace = matchingCloseBrace(text, j);
    if (closeBrace < 0)
      continue;
    return {true, text.mid(j + 1, closeBrace - j - 1)};
  }
  return {false, QString()};
}

// Returns the substring strictly between the matching braces of
// `class TypeName { ... }`/`struct TypeName { ... }` in `text` (tolerating
// a preceding `template <...>` line, and any base-class/attribute text
// before the opening brace), or an empty, not-found result if `typeName`
// names no class/struct in `text` -- used to SCOPE a search for an inline
// class-body definition (e.g. Identifiers.h's `NonEmptyString::toJson()`,
// defined directly inside its class rather than out-of-line) to the one
// class it belongs to, so a same-named inline method on a DIFFERENT class
// earlier in the same file (e.g. CardCode also has an inline `toJson()`)
// can never be mistaken for it.
struct TextRange {
  bool ok = false;
  QString text;
};

TextRange extractEnclosingTypeBody(const QString &text,
                                   const QString &typeName) {
  if (typeName.isEmpty())
    return {false, QString()};
  const QRegularExpression opener(uR"(\b(?:class|struct)\s+)"_s +
                                  QRegularExpression::escape(typeName) +
                                  uR"(\b[^{;]*\{)"_s);
  const auto m = opener.match(text);
  if (!m.hasMatch())
    return {false, QString()};
  const qsizetype openBrace = m.capturedEnd() - 1;
  const qsizetype closeBrace = matchingCloseBrace(text, openBrace);
  if (closeBrace < 0)
    return {false, QString()};
  return {true, text.mid(openBrace + 1, closeBrace - openBrace - 1)};
}

// Locates `enclosingType::functionName`'s (or, if `enclosingType` is
// empty, the free function `functionName`'s) own implementation body,
// trying -- in order -- (1) a qualified out-of-line `.cpp` definition
// (`ReturnType Enclosing::name(...) { ... }`, the shape most definitions
// in this domain use), then (2) an unqualified definition scoped to
// `enclosingType`'s own class/struct body within `declaringFile`
// specifically (the shape Identifiers.h's NonEmptyString::toJson()/
// CardCode::toJson() use: defined directly inside the class, so never
// qualified with `ClassName::` at their own definition site), then (3),
// only when `enclosingType` is empty, an unqualified definition anywhere
// (a free function). Returns the text strictly between (and not
// including) the body's own opening/closing braces, or an empty (but
// distinguishable-by-`ok`) result if no definition is found anywhere.
FunctionBody
findFunctionBody(const QList<std::pair<QString, QString>> &candidateFiles,
                 const QString &declaringFile, const QString &enclosingType,
                 const QString &functionName) {
  const QString escapedName = QRegularExpression::escape(functionName);
  const QString escapedEnclosing = QRegularExpression::escape(enclosingType);
  const QString returnTypePrefix =
      uR"((?:ValueOrError<\s*)?(?:QJsonObject|QJsonArray|QJsonValue)\s*>?\s+)"_s;

  if (!enclosingType.isEmpty()) {
    const QRegularExpression qualified(returnTypePrefix + escapedEnclosing +
                                       u"::"_s + escapedName + uR"(\s*\()"_s);
    for (const auto &[path, text] : candidateFiles) {
      const FunctionBody found = findDefinitionMatching(text, qualified);
      if (found.ok)
        return found;
    }
    const QRegularExpression unqualified(returnTypePrefix + escapedName +
                                         uR"(\s*\()"_s);
    for (const auto &[path, text] : candidateFiles) {
      if (!path.endsWith(declaringFile))
        continue;
      const TextRange classBody = extractEnclosingTypeBody(text, enclosingType);
      if (!classBody.ok)
        continue;
      const FunctionBody found =
          findDefinitionMatching(classBody.text, unqualified);
      if (found.ok)
        return found;
    }
    return {false, QString()};
  }

  const QRegularExpression free(returnTypePrefix + escapedName + uR"(\s*\()"_s);
  for (const auto &[path, text] : candidateFiles) {
    const FunctionBody found = findDefinitionMatching(text, free);
    if (found.ok)
      return found;
  }
  return {false, QString()};
}

} // namespace

void EncoderHygieneTests::publicJsonApiSurfaceMatchesTheReviewedAllowlist() {
  const QString srcDir = QStringLiteral(ARKHAM_TEST_SRC_DIR);
  const QStringList headers = discoverFiles(srcDir, u".h"_s);
  QVERIFY2(headers.size() > 10,
           "sanity check: src/ should contain many more than 10 headers -- "
           "if this fails, ARKHAM_TEST_SRC_DIR or the discovery glob itself "
           "is broken, and every other check in this test would silently "
           "pass over an empty file list");

  QSet<QString> discovered;
  QStringList deducedAutoHits;
  QStringList aliasHits;
  QHash<QString, QString> declarationLineByKey;
  for (const QString &path : headers) {
    const QString file = QFileInfo(path).fileName();
    const QString text = readFileOrFail(path);
    for (const JsonReturningDeclaration &decl :
         findPublicJsonReturningDeclarations(file, text)) {
      discovered.insert(decl.allowlistKey());
      declarationLineByKey.insert(
          decl.allowlistKey(),
          u"%1:%2: %3"_s.arg(decl.file).arg(decl.line).arg(decl.lineText));
    }
    for (const QString &line : text.split(u'\n'))
      if (deducedAutoEncoderPattern().match(stripLineComment(line)).hasMatch())
        deducedAutoHits.append(file + u": "_s + line.trimmed());
    for (const QString &line : text.split(u'\n'))
      if (qtJsonContainerAliasPattern()
              .match(stripLineComment(line))
              .hasMatch())
        aliasHits.append(file + u": "_s + line.trimmed());
  }

  QVERIFY2(deducedAutoHits.isEmpty(),
           qPrintable(u"found a deduced (bare `auto`) return type on a "_s
                      u"function named like an encoder -- write out its "_s
                      u"return type explicitly so this test's declaration "_s
                      u"scan can see it:\n"_s +
                      deducedAutoHits.join(u'\n')));
  QVERIFY2(
      aliasHits.isEmpty(),
      qPrintable(u"found a `using Alias = QJsonObject/QJsonArray/"_s
                 u"QJsonValue;`-style alias -- this hides a future "_s
                 u"encoder's return type from this test's declaration "_s
                 u"scan; do not alias these types in a domain header:\n"_s +
                 aliasHits.join(u'\n')));

  const QSet<QString> &allowlist = allowlistedEncoders();
  QStringList unexpected;
  for (const QString &key : discovered)
    if (!allowlist.contains(key))
      unexpected.append(declarationLineByKey.value(key, key));
  std::sort(unexpected.begin(), unexpected.end());
  QVERIFY2(
      unexpected.isEmpty(),
      qPrintable(
          u"found a public function declaration returning QJsonObject/"_s
          u"QJsonArray/QJsonValue that is not in allowlistedEncoders() -- "_s
          u"every such declaration anywhere under src/*.h must be a "_s
          u"deliberately reviewed addition (see this test's class doc "_s
          u"comment): either add it to the allowlist (and, unless it is "_s
          u"provably safe and added to exemptFromAdapterCallRequirement() "_s
          u"with its own justification, make its body compose via "_s
          u"Value::toExactQJson()/toExactQJsonObject()/toExactQJsonArray()) "_s
          u"or remove/privatize it:\n"_s +
          unexpected.join(u'\n')));

  QStringList stale;
  for (const QString &key : allowlist)
    if (!discovered.contains(key))
      stale.append(key);
  std::sort(stale.begin(), stale.end());
  QVERIFY2(stale.isEmpty(),
           qPrintable(u"allowlistedEncoders() names a declaration this "_s
                      u"test's scanner no longer finds (renamed, removed, "_s
                      u"or the scanner regressed) -- remove the stale "_s
                      u"entry or fix the scanner:\n"_s +
                      stale.join(u'\n')));
}

void EncoderHygieneTests::
    allowlistedDomainEncodersComposeViaTheCentralAdapter() {
  const QString srcDir = QStringLiteral(ARKHAM_TEST_SRC_DIR);
  QList<std::pair<QString, QString>> candidateFiles;
  // Headers first (an inline body, e.g. Identifiers.h's TypedId::toJson(),
  // is defined directly where it is declared), then implementation files.
  for (const QString &path : discoverFiles(srcDir, u".h"_s))
    candidateFiles.append({path, readFileOrFail(path)});
  for (const QString &path : discoverFiles(srcDir, u".cpp"_s))
    candidateFiles.append({path, readFileOrFail(path)});

  const QSet<QString> &allowlist = allowlistedEncoders();
  const QSet<QString> &central = centralAdapterKeys();
  const QSet<QString> &exempt = exemptFromAdapterCallRequirement();

  for (const QString &key : allowlist) {
    if (central.contains(key))
      continue; // the adapter itself; nothing to compose through
    const QStringList parts = key.split(u'#');
    QCOMPARE(parts.size(), 3);
    const QString &declaringFile = parts[0];
    const QString &enclosingType = parts[1];
    const QString &functionName = parts[2];

    const FunctionBody body = findFunctionBody(candidateFiles, declaringFile,
                                               enclosingType, functionName);
    QVERIFY2(body.ok,
             qPrintable(u"could not locate the implementation body for "_s +
                        key +
                        u" -- findFunctionBody()'s definition-finding "_s
                        u"regex may need updating for this signature's "_s
                        u"shape"_s));

    if (exempt.contains(key)) {
      QVERIFY2(!bodyContainsHandBuiltJsonContainer(body.text),
               qPrintable(u"exempt entry "_s + key +
                          u" has grown a hand-built QJsonObject/QJsonArray "_s
                          u"local variable since it was last audited -- "_s
                          u"either restore its previously-audited trivial "_s
                          u"shape or remove it from "_s
                          u"exemptFromAdapterCallRequirement() and require "_s
                          u"it to compose via the central adapter instead"_s));
      continue;
    }

    const bool callsAdapter = body.text.contains(u"toExactQJsonObject("_s) ||
                              body.text.contains(u"toExactQJsonArray("_s) ||
                              body.text.contains(u"toExactQJson("_s);
    QVERIFY2(
        callsAdapter,
        qPrintable(
            u"allowlisted encoder "_s + key +
            u" does not visibly call toExactQJson()/toExactQJsonObject()/"_s
            u"toExactQJsonArray() anywhere in its own function body -- "_s
            u"either it hand-builds JSON directly (regardless of whether "_s
            u"via a named local, an auto local, or a direct return) or it "_s
            u"delegates the real construction to a helper this check "_s
            u"cannot see into, which is exactly the indirection this test "_s
            u"exists to catch: inline the adapter call into this "_s
            u"function's own body"_s));
  }
}

void EncoderHygieneTests::noDomainHeaderDeclaresADeducedAutoEncoder() {
  // Proves deducedAutoEncoderPattern() is not vacuous (also exercised
  // end-to-end, against the real source tree, by
  // publicJsonApiSurfaceMatchesTheReviewedAllowlist() above).
  QVERIFY(deducedAutoEncoderPattern()
              .match(u"auto toJson() const { return QJsonObject{}; }"_s)
              .hasMatch());
  QVERIFY(deducedAutoEncoderPattern()
              .match(u"auto Foo::encodeThing() const {"_s)
              .hasMatch());
  QVERIFY(!deducedAutoEncoderPattern()
               .match(u"ValueOrError<QJsonObject> toJson() const;"_s)
               .hasMatch());
}

void EncoderHygieneTests::noDomainHeaderAliasesAQtJsonContainerType() {
  QVERIFY(qtJsonContainerAliasPattern()
              .match(u"using Json = QJsonObject;"_s)
              .hasMatch());
  QVERIFY(!qtJsonContainerAliasPattern()
               .match(u"using Json = Arkham::Json::Value;"_s)
               .hasMatch());
}

void EncoderHygieneTests::scannerFindsADirectReturnDeclaration() {
  // Proves the declaration-based scanner catches the exact shape a prior
  // review found evading the old body-content regex: a direct
  // `return QJsonObject{...};` with no named local at all.
  const auto hits = findPublicJsonReturningDeclarations(
      u"synthetic.h"_s,
      u"struct Foo {\n"_s
      u"  [[nodiscard]] ValueOrError<QJsonObject> toJson() const;\n"_s
      u"};\n"_s);
  QCOMPARE(hits.size(), 1);
  QCOMPARE(hits.first().enclosingType, u"Foo"_s);
  QCOMPARE(hits.first().functionName, u"toJson"_s);
}

void EncoderHygieneTests::scannerFindsAMultiLineFreeFunctionDeclaration() {
  const auto hits = findPublicJsonReturningDeclarations(
      u"synthetic.h"_s, u"[[nodiscard]] ValueOrError<QJsonArray>\n"_s
                        u"encodeThings(const QList<int> &things);\n"_s);
  QCOMPARE(hits.size(), 1);
  QVERIFY(hits.first().enclosingType.isEmpty());
  QCOMPARE(hits.first().functionName, u"encodeThings"_s);
}

void EncoderHygieneTests::scannerFindsATrailingReturnTypeDeclaration() {
  const auto hits = findPublicJsonReturningDeclarations(
      u"synthetic.h"_s,
      u"struct Foo {\n"_s
      u"  [[nodiscard]] auto toJson() const -> QJsonObject;\n"_s
      u"};\n"_s);
  QCOMPARE(hits.size(), 1);
  QCOMPARE(hits.first().enclosingType, u"Foo"_s);
}

void EncoderHygieneTests::
    scannerIgnoresADecodeSideHelperWithAJsonFirstParameter() {
  const auto hits = findPublicJsonReturningDeclarations(
      u"synthetic.h"_s,
      u"[[nodiscard]] ValueOrError<QJsonObject>\n"_s
      u"requireObjectField(const QJsonObject &obj, QLatin1StringView key,\n"_s
      u"                  QStringView path);\n"_s);
  QVERIFY(hits.isEmpty());
}

void EncoderHygieneTests::scannerIgnoresAPrivateDeclaration() {
  const auto hits = findPublicJsonReturningDeclarations(
      u"synthetic.h"_s,
      u"class Foo {\n"_s
      u"public:\n"_s
      u"  void bar();\n"_s
      u"private:\n"_s
      u"  [[nodiscard]] ValueOrError<QJsonObject> internalFragment() const;\n"_s
      u"};\n"_s);
  QVERIFY(hits.isEmpty());
}

void EncoderHygieneTests::scannerIgnoresAByRefDecodeParameter() {
  const auto hits = findPublicJsonReturningDeclarations(
      u"synthetic.h"_s,
      u"ValueOrError<CardDef> CardDef::fromJson(const QJsonObject &obj,\n"_s
      u"                                        QStringView path) {\n"_s);
  QVERIFY(hits.isEmpty());
}

void EncoderHygieneTests::
    adapterCallCheckDetectsAHandBuiltBodyEvenWithAnAutoLocal() {
  // Proves the positive "calls the adapter" check -- not a negative
  // pattern match against one specific hand-built shape -- correctly
  // rejects a body using an `auto` local rather than a named
  // `QJsonObject`/`QJsonArray` local: the old body-content regex would
  // have missed this exact shape.
  const QList<std::pair<QString, QString>> files = {
      {u"synthetic.cpp"_s,
       u"ValueOrError<QJsonObject> Foo::toJson() const {\n"_s
       u"  auto object = QJsonObject{{\"name\"_L1, m_name}};\n"_s
       u"  return object;\n"_s
       u"}\n"_s}};
  const FunctionBody body =
      findFunctionBody(files, u"synthetic.cpp"_s, u"Foo"_s, u"toJson"_s);
  QVERIFY(body.ok);
  QVERIFY(!body.text.contains(u"toExactQJsonObject("_s));
  QVERIFY(!body.text.contains(u"toExactQJson("_s));
}

void EncoderHygieneTests::
    adapterCallCheckDetectsHelperIndirectionThatHidesTheRealBuild() {
  // Proves helper-indirection is caught too: the PUBLIC function's own
  // body just delegates to a private helper, so the adapter-call token
  // never appears where this check looks, exactly as intended -- it
  // cannot see into buildFragment() to discover whether that helper
  // itself hand-builds or composes correctly, so it must (and does)
  // treat this as a failure, forcing the real composition to happen
  // visibly in the public function's own body instead.
  const QList<std::pair<QString, QString>> files = {
      {u"synthetic.cpp"_s,
       u"ValueOrError<QJsonObject> Foo::toJson() const {\n"_s
       u"  return buildFragment(m_name);\n"_s
       u"}\n"_s}};
  const FunctionBody body =
      findFunctionBody(files, u"synthetic.cpp"_s, u"Foo"_s, u"toJson"_s);
  QVERIFY(body.ok);
  QVERIFY(!body.text.contains(u"toExactQJsonObject("_s));
}

QTEST_MAIN(EncoderHygieneTests)
#include "EncoderHygieneTests.moc"
