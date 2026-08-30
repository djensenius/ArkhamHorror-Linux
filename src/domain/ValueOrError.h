#pragma once

#include <QString>
#include <optional>
#include <utility>

namespace Arkham {

struct Failure {
  QString message;
};

inline Failure failure(QString message) { return Failure{std::move(message)}; }

template <typename T> class ValueOrError {
public:
  ValueOrError(T value) : value_(std::move(value)) {}
  ValueOrError(Failure error) : error_(std::move(error.message)) {}

  [[nodiscard]] bool has_value() const noexcept { return value_.has_value(); }
  [[nodiscard]] explicit operator bool() const noexcept { return has_value(); }

  [[nodiscard]] T &operator*() { return *value_; }
  [[nodiscard]] const T &operator*() const { return *value_; }
  [[nodiscard]] T *operator->() { return &*value_; }
  [[nodiscard]] const T *operator->() const { return &*value_; }

  [[nodiscard]] const QString &error() const { return error_; }

private:
  std::optional<T> value_;
  QString error_;
};

} // namespace Arkham
