// SPDX-License-Identifier: GPL-2.0-or-later
#include "wifi/hostapd_config.h"

#include <sstream>

namespace padlink {

namespace {

// Settings shared by both modes.
void common(std::ostringstream& c, const ApSettings& ap)
{
    c << "interface=" << ap.interface << "\n"
      << "driver=nl80211\n"
      << "ctrl_interface=" << ap.ctrl_dir << "\n"
      << "bssid=" << ap.ap_mac.to_string() << "\n"
      << "hw_mode=a\n"
      << "channel=" << ap.channel << "\n"
      << "ieee80211n=1\n"
      << "beacon_int=100\n"
      << "dtim_period=3\n"
      << "wmm_enabled=1\n"
      << "logger_stdout=-1\n"
      << "logger_stdout_level=1\n";
}

} // namespace

std::string pairing_config(const ApSettings& ap, const std::string& credential_path)
{
    std::ostringstream c;
    c << "# padlink: GamePad pairing AP (open + WPS registrar)\n";
    common(c, ap);
    c << "ssid=" << pairing_ssid(ap.ap_mac) << "\n"
      << "auth_algs=1\n"
      << "ieee8021x=1\n"
      << "eapol_version=1\n"
      << "eap_server=1\n"
      << "wps_state=2\n"
      // Send our own credential (runtime SSID + PSK) instead of describing
      // this open pairing network.
      << "skip_cred_build=1\n"
      << "extra_cred=" << credential_path << "\n"
      // Registrar identity strings as advertised by a real console.
      << "uuid=22210203-0405-0607-0809-0a0b0c0d0e0f\n"
      << "device_name=WiiU" << ap.ap_mac.to_hex() << "\n"
      << "manufacturer=Broadcom\n"
      << "model_name=SoftAP\n"
      << "model_number=0\n"
      << "serial_number=0\n"
      << "device_type=6-a4c0e1f4-1\n"
      << "os_version=80000000\n"
      << "config_methods=label push_button\n"
      // Broadcom HT and compatibility vendor IEs seen in the console beacon;
      // our hostapd patch places the first before WPS and the rest before WMM.
      << "vendor_elements=dd0800904c0700513206dd09001018020100040000\n";
    return c.str();
}

std::string runtime_config(const ApSettings& ap, const std::string& ssid, const std::string& psk_hex)
{
    std::ostringstream c;
    c << "# padlink: GamePad runtime AP (WPA2-PSK, Nintendo variant)\n";
    common(c, ap);
    c << "ssid=" << ssid << "\n"
      << "ignore_broadcast_ssid=2\n"
      << "auth_algs=1\n"
      << "wpa=2\n"
      << "wpa_key_mgmt=WPA-PSK\n"
      << "rsn_pairwise=CCMP\n"
      << "wpa_psk=" << psk_hex << "\n";
    return c.str();
}

} // namespace padlink
