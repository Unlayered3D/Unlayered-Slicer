#include "UiException.hpp"

#include <new>
#include <regex>
#include <typeinfo>

#include <boost/core/demangle.hpp>
#include <boost/format.hpp>

namespace Slic3r {
namespace UiException {

std::string short_what(const std::string &what, size_t max_len)
{
    // A path names the person's files or folders: never in a log line that leaves the machine.
    // It starts after a space, a quote or "=" and runs to a quote, a ": " or ", ", or the end of the
    // line, so a path with spaces in it ("My Drive/part 2.stl") goes whole.
    static const std::regex path(R"((^|[\s"'(=])((?:[A-Za-z]:[\\/]|\\\\|~[\\/]|/)[^"'<>|\n]*?)(?=$|\n|["'<>|]|[:,]\s))");
    std::string out = std::regex_replace(what, path, "$1<path>");
    for (char &c : out)
        if (c == '\n' || c == '\r' || c == '\t')
            c = ' ';
    if (out.size() > max_len)
        out = out.substr(0, max_len > 3 ? max_len - 3 : 0) + "...";
    return out;
}

Info classify(std::exception_ptr e)
{
    Info info;
    if (!e) {
        info.type = "unknown";
        return info;
    }
    try {
        std::rethrow_exception(e);
    } catch (const std::bad_alloc &ex) {
        info.type = "std::bad_alloc";
        info.what = short_what(ex.what());
    } catch (const boost::io::bad_format_string &ex) {
        // a translation with a broken format string: every later message using it fails the same way
        info.type = "boost::io::bad_format_string";
        info.what = short_what(ex.what());
    } catch (const std::exception &ex) {
        info.fatal = false;
        info.type  = boost::core::demangle(typeid(ex).name());
        // MSVC names a type "class std::runtime_error" / "struct ...", clang and gcc "std::runtime_error":
        // one spelling, so reports from every platform group together.
        for (const char *prefix : {"class ", "struct "})
            if (info.type.rfind(prefix, 0) == 0) {
                info.type.erase(0, std::char_traits<char>::length(prefix));
                break;
            }
        info.what  = short_what(ex.what());
    } catch (...) {
        info.type = "unknown";
    }
    return info;
}

bool recover(std::exception_ptr e, const std::function<void(const Info &)> &report)
{
    const Info info = classify(e);
    if (info.fatal)
        return false;
    if (report)
        report(info);
    return true;
}

} // namespace UiException
} // namespace Slic3r
