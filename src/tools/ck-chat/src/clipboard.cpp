#include "clipboard.hpp"
#include <cstdlib>
#include <cstdio>
#include <unistd.h>
#include <string>

namespace clipboard
{
    namespace
    {
        std::string base64Encode(const std::string &input)
        {
            static const char base64_chars[] =
                "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
                "abcdefghijklmnopqrstuvwxyz"
                "0123456789+/";

            std::string encoded;
            int val = 0;
            int valb = -6;
            for (unsigned char c : input)
            {
                val = (val << 8) + c;
                valb += 8;
                while (valb >= 0)
                {
                    encoded.push_back(base64_chars[(val >> valb) & 0x3F]);
                    valb -= 6;
                }
            }
            if (valb > -6)
                encoded.push_back(base64_chars[((val << 8) >> (valb + 8)) & 0x3F]);
            while (encoded.size() % 4)
                encoded.push_back('=');
            return encoded;
        }

        bool osc52Likely()
        {
            const char *noOsc52 = std::getenv("NO_OSC52");
            if (noOsc52 && *noOsc52)
                return false;

            const char *term = std::getenv("TERM");
            if (!term)
                return false;

            std::string termStr(term);

            if (termStr == "dumb" || termStr == "linux")
                return false;

            return termStr.find("xterm") != std::string::npos ||
                   termStr.find("tmux") != std::string::npos ||
                   termStr.find("screen") != std::string::npos ||
                   termStr.find("rxvt") != std::string::npos ||
                   termStr.find("alacritty") != std::string::npos ||
                   termStr.find("foot") != std::string::npos ||
                   termStr.find("kitty") != std::string::npos ||
                   termStr.find("wezterm") != std::string::npos;
        }
    } // namespace

    std::string statusMessage()
    {
        if (osc52Likely())
            return "Response copied to clipboard!";
        if (std::getenv("TMUX") && !osc52Likely())
            return "Clipboard not supported - tmux needs OSC 52 configuration";
        return "Clipboard not supported by this terminal";
    }

    void copyToClipboard(const std::string &text)
    {
        if (!osc52Likely())
            return;

        constexpr std::size_t maxOsc52Payload = 100000;
        std::string encoded = base64Encode(text);

        if (encoded.size() > maxOsc52Payload)
            return;

        FILE *out = std::fopen("/dev/tty", "w");
        if (!out && isatty(fileno(stdout)))
            out = stdout;

        if (!out)
            return;

        std::fprintf(out, "\033]52;c;%s\a", encoded.c_str());
        std::fflush(out);
        if (out != stdout)
            std::fclose(out);
    }
} // namespace clipboard
