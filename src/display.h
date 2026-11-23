#pragma once
#include "solder.h"

// Optional OLED (build with -DSOLDER_OLED=1). Without it every call does nothing,
// the browser and the LED are the only output.
namespace display
{
    void begin();
    void boot(const String &apName, const String &apPass);
    void idle(const String &address, bool locked);
    void request(const Request &r);   // what is being signed + hold progress
    void words(const String &mnemonic, int page);
    void text(const String &line1, const String &line2 = "");
}
