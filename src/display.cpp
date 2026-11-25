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

    bool available()
    {
        if (!ready) return false;
        Wire.beginTransmission(0x3C);
        return Wire.endTransmission() == 0;
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
        for (unsigned i = 0; i < apPass.length(); ++i)
        {
            if (i == 21) oled.setCursor(0, 36);
            oled.write(apPass[i]);
        }
        oled.setCursor(0, 50);
        oled.print("Open 192.168.4.1");
        oled.display();
    }

    static constexpr unsigned columns = 21;

    static void field(const String &value)
    {
        for (unsigned i = 0; i < value.length(); ++i)
        {
            if (i % columns == 0) oled.setCursor(0, 14 + (i / columns) * 8);
            oled.write(value[i]);
        }
    }

    void idle(const String &address, bool locked)
    {
        if (!ready) return;
        head(locked ? "Solder - locked" : "Solder - unlocked");
        oled.setCursor(0, 14);
        if (address.length()) field(address);
        else oled.print("-");
        oled.setCursor(0, 40);
        oled.print("WiFi ");
        oled.print(solder::settings.defaultPassword() ? "setup required" : "password set");
        oled.setCursor(0, 52);
        oled.print("http://192.168.4.1");
        oled.display();
    }

    // At text size 1 the 128px display fits 21 columns. Keep every signed
    // byte visible. Escape controls, UTF-8 bytes and the escape character.
    static constexpr unsigned rows = 4;

    static unsigned messagePages(const Request &r, int drawPage)
    {
        static const char hex[] = "0123456789ABCDEF";
        unsigned line = 0, col = 0;
        for (uint8_t b : r.message)
        {
            char escaped[] = {'\\', 'x', hex[b >> 4], hex[b & 15]};
            bool plain = b >= 0x20 && b <= 0x7e && b != '\\';
            unsigned width = plain ? 1 : 4;
            if (col + width > columns)
            {
                ++line;
                col = 0;
            }
            if ((int)(line / rows) == drawPage)
            {
                oled.setCursor(col * 6, 12 + (line % rows) * 8);
                if (plain) oled.write(b);
                else for (char c : escaped) oled.write(c);
            }
            col += width;
        }
        return 1 + line / rows;
    }

    unsigned reviewPages(const Request &r)
    {
        switch (r.kind)
        {
        case ReqKind::SendSol: return 2;
        case ReqKind::SendToken: return 3;
        case ReqKind::SignMessage: return messagePages(r, -1);
        case ReqKind::Wipe: return 1;
        }
        return 0;
    }

    void request(const Request &r, unsigned page)
    {
        if (!ready) return;
        unsigned pages = reviewPages(r);
        switch (r.kind)
        {
        case ReqKind::SendSol:
        case ReqKind::SendToken:
            if (page == 0)
            {
                const char *title = r.kind == ReqKind::SendSol ? "Send SOL"
                    : r.tokenProgram == INFRATIC_TOKEN_2022_PROGRAM_ID ? "Send Token-2022" : "Send SPL token";
                head(title);
                field(r.amountText);
                // An offline signer cannot verify the cluster of a blockhash.
                oled.setCursor(0, 36);
                oled.print("Can spend real funds");
            }
            else if (page == 1 && r.kind == ReqKind::SendToken)
            {
                head("Token mint");
                field(r.mint);
            }
            else
            {
                head("Recipient");
                field(r.to);
            }
            break;
        case ReqKind::SignMessage:
            head("Sign message");
            messagePages(r, page);
            break;
        case ReqKind::Wipe:
            head("ERASE WALLET");
            oled.setCursor(0, 16);
            oled.print("Keys are deleted.");
            oled.setCursor(0, 26);
            oled.print("Restore with backup.");
            break;
        }
        if (pages > 1)
        {
            oled.setCursor(96, 0);
            oled.printf("%u/%u", page + 1, pages);
        }
        oled.setCursor(0, 46);
        oled.print(page + 1 < pages ? "Tap BOOT for next" : "Hold BOOT to approve");
        oled.drawRect(0, 56, 128, 8, SSD1306_WHITE);
        oled.fillRect(0, 56, (int)(128 * r.hold), 8, SSD1306_WHITE);
        oled.display();
    }

    void words(const String &mnemonic, int page)
    {
        if (!ready) return;
        head("Backup words");
        int index = 0;
        for (unsigned i = 0; i < mnemonic.length(); ++i)
        {
            if (mnemonic[i] == ' ')
            {
                ++index;
                continue;
            }
            if (index / 6 != page) continue;
            if (i == 0 || mnemonic[i - 1] == ' ')
            {
                oled.setCursor(0, 14 + (index % 6) * 8);
                oled.printf("%d. ", index + 1);
            }
            oled.write(mnemonic[i]);
        }
        oled.display();
    }

    void backupQuestion(unsigned question, unsigned position, const char *const choices[3], unsigned selected)
    {
        if (!ready) return;
        head("Backup check");
        oled.setCursor(96, 0);
        oled.printf("%u/3", question);
        oled.setCursor(0, 13);
        oled.printf("Word #%u?", position);
        for (unsigned i = 0; i < 3; ++i)
        {
            oled.setCursor(0, 24 + i * 10);
            oled.print(i == selected ? "> " : "  ");
            for (const char *p = choices[i]; *p && *p != ' '; ++p) oled.write(*p);
        }
        oled.setCursor(0, 56);
        oled.print("Tap:next  Hold:pick");
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
    bool available() { return true; }
    unsigned reviewPages(const Request &) { return 1; }
    void request(const Request &, unsigned) {}
    void words(const String &, int) {}
    void text(const String &, const String &) {}
}

#endif
