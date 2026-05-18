#pragma once

#include <string>
#include <vector>

#include "buffer.h"

namespace flowstate {

enum class DiagnosticSeverity {
    Error,
    Warning,
    Information,
    Hint,
};

struct DiagnosticRange {
    Cursor start;
    Cursor end;
};

struct Diagnostic {
    DiagnosticRange range;
    DiagnosticSeverity severity = DiagnosticSeverity::Error;
    std::string message;
    std::string source;
};

bool HasErrorDiagnosticAt(const std::vector<Diagnostic>& diagnostics, size_t row, size_t col);
const Diagnostic* ErrorDiagnosticAt(const std::vector<Diagnostic>& diagnostics, size_t row, size_t col);

}  // namespace flowstate
