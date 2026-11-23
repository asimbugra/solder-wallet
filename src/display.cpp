#include "display.h"

#if SOLDER_OLED
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

static Adafruit_SSD1306 oled(128, 64, &Wire, -1);
static bool ready = false;

namespace display
{
    void begin()
    {
        Wire.begin(SOLDER_OLED_SDA, SOLDER_OLED_SCL);
        ready = oled.begin(SSD1306_SWITCHCAPVCC, 0x3C);
        if (!ready) return;
        oled.setTextColor(SSD1306_WHITE);
        oled.setTextWrap(false);
    }

    static void head(const char *title)
    {
        oled.clearDisplay();
        oled.setTextSize(1);
        oled.setCursor(0, 0);
        oled.print(title);
        oled.drawFastHLine(0, 10, 128, SSD1306_WHITE);
    }

    void boot(const String &apName, const String &apPass)
    {
        if (!ready) return;
        head("Solder");
        oled.setCursor(0, 16);
        oled.print("WiFi: ");
        oled.print(apName);
        oled.setCursor(0, 28);
        oled.print("Pass: ");
        oled.print(apPass);
        oled.setCursor(0, 44);
        oled.print("Open 192.168.4.1");
        oled.display();
    }

    void idle(const String &address, bool locked)
    {
        if (!ready) return;
        head(locked ? "Solder - locked" : "Solder - unlocked");
        oled.setCursor(0, 16);
        oled.print(address.length() ? solder::shortAddress(address) : String("-"));
        oled.setCursor(0, 30);
        oled.print("WiFi ");
        oled.print(solder::settings.defaultPassword() ? "admin123" : "password set");
        oled.setCursor(0, 44);
        oled.print("http://192.168.4.1");
        oled.display();
    }

    void request(const Request &r)
    {
        if (!ready) return;
        switch (r.kind)
        {
        case ReqKind::SendSol:
        case ReqKind::SendToken:
            head("Send");
            oled.setTextSize(2);
            oled.setCursor(0, 14);
            oled.print(r.amountText);
            oled.setTextSize(1);
            oled.print(" ");
            oled.print(r.symbol);
            oled.setCursor(0, 34);
            oled.print("to ");
            oled.print(solder::shortAddress(r.to));
            break;
        case ReqKind::SignMessage:
            head("Sign message");
            oled.setCursor(0, 14);
            for (size_t i = 0; i < r.message.size() && i < 42; i++)
            {
                char c = (char)r.message[i];
                oled.print(c >= 32 && c < 127 ? c : '.');
                if (i == 20) oled.setCursor(0, 24);
            }
            break;
        case ReqKind::Wipe:
            head("ERASE WALLET");
            oled.setCursor(0, 16);
            oled.print("Keys are deleted.");
            oled.setCursor(0, 26);
            oled.print("Only the words restore.");
            break;
        }
        oled.setCursor(0, 46);
        oled.print("Hold BOOT to approve");
        oled.drawRect(0, 56, 128, 8, SSD1306_WHITE);
        oled.fillRect(0, 56, (int)(128 * r.hold), 8, SSD1306_WHITE);
        oled.display();
    }

    void words(const String &mnemonic, int page)
    {
        if (!ready) return;
        head("Backup words");
        int idx = 0, start = 0, shown = 0;
        String m = mnemonic + " ";
        for (int i = 0; i < (int)m.length(); i++)
        {
            if (m[i] != ' ') continue;
            if (idx >= page * 6 && idx < page * 6 + 6)
            {
                oled.setCursor((shown % 2) * 64, 14 + (shown / 2) * 12);
                oled.print(idx + 1);
                oled.print(".");
                oled.print(m.substring(start, i));
                shown++;
            }
            idx++;
            start = i + 1;
        }
        oled.display();
    }

    void text(const String &line1, const String &line2)
    {
        if (!ready) return;
        head("Solder");
        oled.setCursor(0, 20);
        oled.print(line1);
        oled.setCursor(0, 34);
        oled.print(line2);
        oled.display();
    }
}

#else

namespace display
{
    void begin() {}
    void boot(const String &, const String &) {}
    void idle(const String &, bool) {}
    void request(const Request &) {}
    void words(const String &, int) {}
    void text(const String &, const String &) {}
}

#endif
