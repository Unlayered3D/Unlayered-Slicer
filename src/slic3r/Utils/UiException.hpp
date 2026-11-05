#pragma once

// What the app does with a C++ exception a UI handler let escape (GUI_App::OnExceptionInMainLoop),
// kept free of wx so it can be tested. 0.1.9 crash, 11/01: a std::exception from the Slice button's
// click handler went OnExceptionInMainLoop -> rethrow -> wxEvtHandler::WXConsumeException -> wxAbort(),
// so an ordinary slicing error closed the app and the crash report never said what it was.

#include <cstddef>
#include <exception>
#include <functional>
#include <string>

namespace Slic3r {
namespace UiException {

struct Info
{
    bool        fatal = true; // the app cannot go on: out of memory, a broken translation, not a std::exception
    std::string type;         // "std::runtime_error", "Slic3r::SlicingError", ... ("unknown" when not a std::exception)
    std::string what;         // short_what() of its message
};

// The exception `e` (from std::current_exception()) as the main loop treats it.
Info classify(std::exception_ptr e);

// An exception message for logs and diagnostics: one line, file paths replaced by "<path>", at most
// `max_len` characters (cut with "...").
std::string short_what(const std::string &what, size_t max_len = 120);

// The main loop's decision: true = reported through `report` and the app keeps running (a
// recoverable std::exception); false = fatal, the caller ends the app as before.
bool recover(std::exception_ptr e, const std::function<void(const Info &)> &report);

} // namespace UiException
} // namespace Slic3r

