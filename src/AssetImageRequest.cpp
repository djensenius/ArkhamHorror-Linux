#include "AssetImageRequest.h"

#include <QPointer>

namespace Arkham {

namespace {

// A short, human-readable category label for the accessibility
// description only -- not used for any path/URL/cache-key construction,
// where AssetCategory is always handled as a typed enumerator.
QString categoryLabel(AssetCategory category) {
  switch (category) {
  case AssetCategory::Card:
    return QStringLiteral("card");
  case AssetCategory::InvestigatorPortrait:
    return QStringLiteral("investigator portrait");
  case AssetCategory::ChaosToken:
    return QStringLiteral("chaos token");
  case AssetCategory::SetIcon:
    return QStringLiteral("set icon");
  case AssetCategory::CampaignBox:
    return QStringLiteral("campaign box");
  case AssetCategory::SlotIcon:
    return QStringLiteral("slot icon");
  case AssetCategory::HomebrewCard:
    return QStringLiteral("homebrew card");
  case AssetCategory::HomebrewSet:
    return QStringLiteral("homebrew set");
  case AssetCategory::HomebrewBox:
    return QStringLiteral("homebrew box");
  }
  // Consistent with every other AssetCategory switch in this codebase
  // (see AssetLocator.cpp's categoryRoot()/canonicalExtensionFor()/etc.):
  // no `default:` case, so adding a new AssetCategory enumerator without
  // updating this function is a compile-time -Wswitch warning rather
  // than a silently-wrong runtime fallback string, and Q_UNREACHABLE()
  // additionally traps it immediately in debug builds if the warning is
  // ever missed.
  Q_UNREACHABLE_RETURN(QStringLiteral("asset"));
}

// The base accessible description text this request is "carrying", per
// djensenius/ArkhamHorror-Linux#17's requirement to carry a
// caller-provided accessible description rather than an internally
// invented one: `callerAccessibleDescription` is used verbatim when the
// caller supplied one (e.g. a card's title, an investigator's name), and
// only a generic category-derived label is synthesised as a fallback for
// callers that have not (yet) been updated to supply their own.
QString
baseAccessibleDescriptionFor(const AssetKey &key,
                             const QString &callerAccessibleDescription) {
  if (!callerAccessibleDescription.isEmpty()) {
    return callerAccessibleDescription;
  }
  return QStringLiteral("%1 \"%2\"")
      .arg(categoryLabel(key.category), key.identifier);
}

} // namespace

AssetImageRequest::AssetImageRequest(AssetRequestCoordinator &coordinator,
                                     QObject *parent)
    : QObject(parent), m_coordinator(coordinator) {}

AssetImageRequest::~AssetImageRequest() {
  // Bump the generation before cancelling. This alone does NOT protect a
  // queued coordinator callback that captures `this` -- that callback is
  // guarded independently by the QPointer in load() below, since
  // cancel()'s own queued delivery of the Cancelled result can run after
  // this destructor has already finished.
  ++m_generation;
  if (m_handle.isValid()) {
    m_coordinator.cancel(m_handle);
    m_handle = {};
  }
}

void AssetImageRequest::load(const AssetKey &key,
                             const QString &callerAccessibleDescription) {
  // Round-N+ review ("load emits direct signals before registering
  // request; a directly-connected observer that reentrantly calls
  // load()/cancel() again -- or that destroys this object outright --
  // from inside one of THIS call's own synchronous NOTIFY emissions can
  // leave a coordinator handle unreachable, or run this function's
  // remaining body against an already-destroyed object"): every
  // synchronous emission below (including the ones inside cancel(),
  // which this function calls first) is followed by a QPointer liveness
  // check before this function touches `this` again. `guard` is
  // deliberately the SAME kind of QPointer used for the async callback
  // below (see its own comment) -- both defend against exactly the same
  // hazard, just at different points in this object's lifetime.
  QPointer<AssetImageRequest> guard(this);
  // cancel() already bumps m_generation unconditionally (invalidating any
  // in-flight callback tied to the previous generation, whether or not a
  // handle was actually live). A second bump here would be redundant --
  // this new request's generation only needs to be distinct from
  // whatever generation preceded it, which cancel()'s bump already
  // guarantees.
  cancel();
  if (!guard) {
    return; // destroyed synchronously by a signal cancel() emitted above
  }
  const quint64 generation = m_generation;
  const QString base =
      baseAccessibleDescriptionFor(key, callerAccessibleDescription);

  // Round-N+ review ("statusChanged() emitted before error/image/
  // accessibleDescription state is cleared/updated, exposing an
  // inconsistent property snapshot to a directly-connected observer --
  // including a QML binding -- that reads those OTHER properties from
  // within its statusChanged() handler"): every member this transition
  // touches is updated FIRST, with no signal emitted yet, so there is no
  // window in which `status` has already changed to Loading while
  // `errorString()`/`errorCode()`/`image()`/`accessibleDescription()`
  // still return a stale, previous-load() value. Only once every member
  // holds its final value for this transition are the NOTIFY signals
  // emitted -- statusChanged() last, since it is the highest-level
  // "something changed, re-inspect me" signal most observers (QML
  // bindings included) key off of to decide when to re-read the other
  // properties.
  const bool statusChangedFlag = m_status != Status::Loading;
  m_status = Status::Loading;
  m_progress = 0.0;
  const QString loadingDescription = QStringLiteral("Loading %1...").arg(base);
  const bool descriptionChangedFlag =
      m_accessibleDescription != loadingDescription;
  m_accessibleDescription = loadingDescription;
  // Clearing the error state is itself a state change QML bindings to
  // errorString()/errorCode() must observe: without emitting
  // errorChanged() here, a binding would keep showing the previous
  // load()'s error message throughout the new Loading phase.
  const bool errorChangedFlag = !m_errorString.isEmpty() || m_errorCode != 0;
  if (errorChangedFlag) {
    m_errorString.clear();
    m_errorCode = 0;
  }
  // Clear any previously-loaded image immediately: otherwise a caller
  // that reuses one AssetImageRequest across two different keys (e.g. a
  // QML Image binding that switches source identifiers) would keep
  // showing the OLD image throughout the new Loading phase.
  const bool imageChangedFlag = !m_image.isNull();
  if (imageChangedFlag) {
    m_image = QImage();
  }

  emit progressChanged();
  if (!guard) {
    return; // destroyed synchronously by a progressChanged() observer
  }
  if (descriptionChangedFlag) {
    emit accessibleDescriptionChanged();
    if (!guard) {
      return;
    }
  }
  if (errorChangedFlag) {
    emit errorChanged();
    if (!guard) {
      return;
    }
  }
  if (imageChangedFlag) {
    emit imageChanged();
    if (!guard) {
      return;
    }
  }
  if (statusChangedFlag) {
    emit statusChanged();
    if (!guard) {
      return;
    }
  }

  // The coordinator retains this callback (via std::function, possibly
  // queued past this object's own lifetime -- e.g. a cancel() during
  // destruction still queues one final Cancelled delivery for exactly
  // this consumer). A raw `this` capture would be a use-after-free once
  // that queued delivery runs after this object is destroyed, so every
  // member access below is guarded by a QPointer liveness check first.
  // `self` and `guard` alias the exact same object -- kept as two named
  // QPointers only because they answer two different questions: `guard`
  // (checked synchronously, right here, right now) asks "is this object
  // still alive after the signals THIS call just emitted?"; `self`
  // (captured into the callback, checked whenever the coordinator
  // eventually invokes it -- possibly much later, possibly never) asks
  // the same question at a completely different point in time.
  QPointer<AssetImageRequest> self(guard);
  AssetRequestCoordinator::RequestHandle handle = m_coordinator.request(
      key,
      [self, generation, base](AssetOutcome<AssetCache::CachedEntry> result) {
        if (!self) {
          return; // this AssetImageRequest was destroyed
        }
        // A stale callback from a superseded load()/cancel() is rejected
        // by generation number: only the most recent load() may mutate
        // state.
        if (generation != self->m_generation) {
          return;
        }
        self->m_handle = {};
        if (!result) {
          const AssetError &error = result.error();
          // Round-N+ review (see load()'s own comment above for the
          // full rationale): every member this transition touches is
          // assigned FIRST -- including via setAccessibleDescription(),
          // whose own internal emit already only fires once
          // m_status/m_errorString/m_errorCode/m_image above are
          // already final -- and statusChanged() is emitted LAST, after
          // every other NOTIFY signal for this transition, so no
          // observer can ever see Loading-or-later status alongside a
          // property that has not yet caught up.
          self->m_status = Status::Error;
          self->m_errorString = error.message;
          self->m_errorCode = static_cast<int>(error.code);
          self->m_image = QImage();
          self->m_progress = 1.0;
          self->setAccessibleDescription(
              QStringLiteral("Failed to load %1: %2").arg(base, error.message));
          if (!self) {
            return; // destroyed synchronously by accessibleDescriptionChanged()
          }
          emit self->errorChanged();
          if (!self) {
            return;
          }
          emit self->imageChanged();
          if (!self) {
            return;
          }
          emit self->progressChanged();
          if (!self) {
            return;
          }
          emit self->statusChanged();
          return;
        }

        self->m_image = result->decodedImage;
        self->m_status = Status::Ready;
        self->m_progress = 1.0;
        self->setAccessibleDescription(base);
        if (!self) {
          return; // destroyed synchronously by accessibleDescriptionChanged()
        }
        emit self->imageChanged();
        if (!self) {
          return;
        }
        emit self->progressChanged();
        if (!self) {
          return;
        }
        emit self->statusChanged();
      });
  // Round-N+ review (HIGH, "reentrant signal handler load(B) registers,
  // then outer load(A) overwrites m_handle; B unreachable, stale A
  // consumer/network/cache publication lives"): a directly-connected
  // observer reacting to one of THIS call's own signal emissions above
  // may have reentrantly called load() (or cancel()) again on this exact
  // object -- which runs to completion, including registering ITS OWN
  // handle into m_handle -- before this call resumes here. Unconditionally
  // assigning `m_handle = handle` in that case would silently clobber the
  // newer, currently-current registration, permanently orphaning it (never
  // cancelled, never tracked, its underlying fetch/decode left running
  // forever). `generation` was captured before any of this call's own
  // signals were emitted, so if it no longer matches `m_generation`, some
  // reentrant load()/cancel() has already superseded this call entirely:
  // this registration is already stale and must be cancelled immediately
  // instead of installed.
  if (!guard) {
    // Destroyed synchronously by the coordinator's own bookkeeping is not
    // expected (request() never emits into this object), but if some
    // future change ever made that possible, the newly-minted handle
    // would otherwise leak with no owner left to cancel it.
    m_coordinator.cancel(handle);
    return;
  }
  if (generation == m_generation) {
    m_handle = handle;
  } else {
    m_coordinator.cancel(handle);
  }
}

void AssetImageRequest::cancel() {
  QPointer<AssetImageRequest> guard(this);
  ++m_generation;
  if (m_handle.isValid()) {
    m_coordinator.cancel(m_handle);
    m_handle = {};
  }
  if (m_status == Status::Loading) {
    // Round-N+ review (see load()'s own comment for the full
    // rationale): m_progress and m_accessibleDescription are reset to
    // their final values BEFORE m_status changes and statusChanged()
    // emits, so no observer reacting to statusChanged() can see a
    // stale progress/description left over from the just-cancelled
    // Loading phase.
    m_status = Status::Idle;
    m_progress = 0.0;
    const bool descriptionChangedFlag = !m_accessibleDescription.isEmpty();
    m_accessibleDescription.clear();
    emit progressChanged();
    if (!guard) {
      return; // destroyed synchronously by a progressChanged() observer
    }
    if (descriptionChangedFlag) {
      emit accessibleDescriptionChanged();
      if (!guard) {
        return;
      }
    }
    emit statusChanged();
  }
}

void AssetImageRequest::setAccessibleDescription(const QString &description) {
  if (m_accessibleDescription == description) {
    return;
  }
  m_accessibleDescription = description;
  emit accessibleDescriptionChanged();
}

} // namespace Arkham
