#pragma once

// Kernel-internal: Reporter is the emission sink used by validator.cpp
// (R1..R6 + GlassWorm L1) and cross_file_validator.cpp (CF1..CF7 +
// CFW1..CFW4 + RT1..RT3). Promoted out of validator.cpp's anonymous
// namespace in 0.4.2 so both passes can construct ValidationMessage
// entries through the same surface. Not part of the public
// package-validator API; do not include from outside
// src/kernel/packages/.

#include "kernel/packages/validator.hpp"

#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace plinth::packages::detail {

// `out` is a pointer (not reference) to comply with
// cppcoreguidelines-avoid-const-or-ref-data-members; never null in
// practice — callers construct Reporter with &report.messages.
struct Reporter {
  std::vector<ValidationMessage>* out = nullptr;

  auto error(std::string rule, std::string message,
             std::optional<std::string> path = std::nullopt,
             std::optional<std::string> remediation = std::nullopt,
             Phase phase = Phase::STRUCTURE) const -> void {
    out->push_back(ValidationMessage{
        .severity = Severity::ERROR,
        .phase = phase,
        .rule = std::move(rule),
        .path = std::move(path),
        .message = std::move(message),
        .remediation = std::move(remediation),
    });
  }

  auto warn(std::string rule, std::string message,
            std::optional<std::string> path = std::nullopt,
            std::optional<std::string> remediation = std::nullopt,
            Phase phase = Phase::STRUCTURE) const -> void {
    out->push_back(ValidationMessage{
        .severity = Severity::WARNING,
        .phase = phase,
        .rule = std::move(rule),
        .path = std::move(path),
        .message = std::move(message),
        .remediation = std::move(remediation),
    });
  }
};

} // namespace plinth::packages::detail
