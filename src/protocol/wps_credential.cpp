// SPDX-License-Identifier: GPL-2.0-or-later
#include "protocol/wps_credential.h"

#include <algorithm>
#include <cctype>

namespace padlink {

namespace {

// One WPS TLV: 2-byte type, 2-byte length, then the value.
void put_tlv(Bytes& out, uint16_t type, ByteView value)
{
    put_be16(out, type);
    put_be16(out, uint16_t(value.size()));
    put_bytes(out, value);
}

ByteView view(const std::string& s) { return {reinterpret_cast<const uint8_t*>(s.data()), s.size()}; }

} // namespace

std::optional<Bytes> build_wps_credential(const WpsCredential& cred)
{
    const bool psk_ok = cred.psk_hex.size() == 64 &&
        std::all_of(cred.psk_hex.begin(), cred.psk_hex.end(),
                    [](unsigned char c) { return std::isxdigit(c) != 0; });
    if (cred.ssid.empty() || cred.ssid.size() > 32 || !psk_ok)
        return std::nullopt;

    const uint8_t network_index[] = {0x01};
    const uint8_t auth_wpa2_psk[] = {0x00, 0x20};
    const uint8_t encr_aes[] = {0x00, 0x08};

    Bytes inner;
    put_tlv(inner, wps_attr::NetworkIndex, network_index);
    put_tlv(inner, wps_attr::Ssid, view(cred.ssid));
    put_tlv(inner, wps_attr::AuthType, auth_wpa2_psk);
    put_tlv(inner, wps_attr::EncrType, encr_aes);
    put_tlv(inner, wps_attr::NetworkKey, view(cred.psk_hex));
    put_tlv(inner, wps_attr::MacAddress, cred.ap_mac.bytes);

    Bytes outer;
    put_tlv(outer, wps_attr::Credential, inner);
    return outer;
}

} // namespace padlink
