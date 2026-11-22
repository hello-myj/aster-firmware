Upstream: https://github.com/nayuki/QR-Code-generator
Revision: bcfe1b018cec5f1f91b5705eca5bb4cdd34e9f8d
Files: c/qrcodegen.c, c/qrcodegen.h (unmodified)
License: MIT; full notice retained in both source files.
Used only for the local Wi-Fi setup QR code. No network or image dependency.

The [shared first-boot UI](../../systems/shared/setup/setup_ui.c) encodes the setup hotspot SSID and password into a Wi-Fi QR code. After joining the hotspot, the phone submits the home network settings through the local web page. See [first-boot setup](../../docs/first-boot-setup.md).
