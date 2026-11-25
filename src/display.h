#pragma once
#include "solder.h"

// Optional OLED (build with -DSOLDER_OLED=1). Without it every call does nothing,
// the browser and the LED are the only output.
namespace display
{
    void begin();
    bool available();
    void boot(const String &apName, const String &apPass);
    void idle(const String &address, bool locked);
    unsigned reviewPages(const Request &r);
    void request(const Request &r, unsigned page = 0);
    void words(const String &mnemonic, int page);
#if SOLDER_OLED
    void backupQuestion(unsigned question, unsigned position, const char *const choices[3], unsigned selected);
#endif
    void text(const String &line1, const String &line2 = "");
}
