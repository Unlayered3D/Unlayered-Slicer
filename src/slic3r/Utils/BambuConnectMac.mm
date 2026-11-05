// UNLAYERED_MOD -- Bambu Connect hand-off, macOS only: which application would open a bambu-connect:// link.
// wxLaunchDefaultBrowser() hands the link to NSWorkspace but cannot tell "Connect is not installed" from
// "the link was opened": this asks Launch Services first, so the UI can say Connect is missing instead of
// claiming a hand-off. A registered handler is a precheck, not proof the import worked.
#import <AppKit/AppKit.h>

#include "BambuConnect.hpp"

#include <string>

namespace Slic3r { namespace BambuConnect {

std::string mac_handler_app_path()
{
    @autoreleasepool {
        NSURL *probe = [NSURL URLWithString:@"bambu-connect://import-file"];
        if (probe == nil)
            return {};
        NSURL *app = [[NSWorkspace sharedWorkspace] URLForApplicationToOpenURL:probe];
        if (app == nil || [app path] == nil)
            return {};
        return std::string([[app path] fileSystemRepresentation]);
    }
}

}} // namespace Slic3r::BambuConnect
