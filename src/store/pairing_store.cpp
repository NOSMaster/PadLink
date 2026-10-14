// SPDX-License-Identifier: GPL-2.0-or-later
#include "store/pairing_store.h"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <unistd.h>

namespace padlink {

std::string serialize(const PairingRecord& rec)
{
    std::ostringstream out;
    out << "# padlink GamePad pairing. Contains the Wi-Fi key: keep private.\n"
        << "ap_mac=" << rec.ap_mac.to_string() << "\n"
        << "ssid=" << rec.ssid << "\n"
        << "psk=" << rec.psk_hex << "\n"
        << "channel=" << rec.channel << "\n";
    if (rec.gamepad_mac)
        out << "gamepad_mac=" << rec.gamepad_mac->to_string() << "\n";
    return out.str();
}

std::optional<PairingRecord> deserialize(const std::string& text)
{
    std::map<std::string, std::string> kv;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#')
            continue;
        const size_t eq = line.find('=');
        if (eq == std::string::npos)
            return std::nullopt;
        kv[line.substr(0, eq)] = line.substr(eq + 1);
    }

    PairingRecord rec;
    const auto ap = MacAddress::parse(kv["ap_mac"]);
    if (!ap || kv["ssid"].empty() || kv["psk"].size() != 64)
        return std::nullopt;
    rec.ap_mac = *ap;
    rec.ssid = kv["ssid"];
    rec.psk_hex = kv["psk"];
    try {
        rec.channel = std::stoi(kv["channel"]);
    } catch (...) {  // std::stoi throws on bad input
        return std::nullopt;
    }
    if (kv.contains("gamepad_mac")) {
        rec.gamepad_mac = MacAddress::parse(kv["gamepad_mac"]);
        if (!rec.gamepad_mac)
            return std::nullopt;
    }
    return rec;
}

std::optional<PairingRecord> load_pairing(const std::string& path)
{
    std::ifstream in(path);
    if (!in)
        return std::nullopt;
    std::stringstream ss;
    ss << in.rdbuf();
    return deserialize(ss.str());
}

bool save_pairing(const std::string& path, const PairingRecord& rec, std::string& error)
{
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);
    // Write to a temporary file, then rename: a crash never leaves half a file.
    const std::string tmp = path + ".tmp";
    const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0) {
        error = tmp + ": " + std::strerror(errno);
        return false;
    }
    const std::string text = serialize(rec);
    const bool ok = ::write(fd, text.data(), text.size()) == ssize_t(text.size()) && ::fsync(fd) == 0;
    ::close(fd);
    if (!ok || ::rename(tmp.c_str(), path.c_str()) != 0) {
        error = path + ": " + std::strerror(errno);
        ::unlink(tmp.c_str());
        return false;
    }
    return true;
}

} // namespace padlink
