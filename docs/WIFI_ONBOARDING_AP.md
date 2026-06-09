# Wi-Fi Onboarding AP

ESP-Claw starts a provisioning AP when STA Wi-Fi is not configured, when STA
connection cannot be used for the current boot, or when BOOT is held for the
configured long-press duration.

## Default Portal

```text
SSID: esp-claw-XXXXXX
URL:  http://192.168.4.1/
Auth: open unless ap_password is configured
```

The captive DNS service points clients at the board, so phones and laptops often
open the setup page automatically after joining the AP.

## STA Behavior

The SuperMini profile defaults to:

```text
ap_behavior = close_on_sta
```

After the board connects to the configured STA network, the AP is closed. Web
Admin remains available at the STA IP address.

## BOOT Long Press

Hold BOOT for 5 seconds to force provisioning on the next boot. This stores a
one-shot RTC flag and restarts the board.

The saved SSID and password are not erased. The next boot simply skips STA,
starts the provisioning AP, and lets you edit settings. A later reboot can use
the existing STA credentials again.

## Web Admin Security

During first-time or forced AP provisioning, Web Admin is open so the device can
be recovered easily.

On the STA network, Web Admin requires HTTP Basic Auth. Credentials are resolved
in this order:

```text
NVS admin_username/admin_password
build-time Kconfig defaults
generated fallback
```

Default username:

```text
admin
```

Fallback password:

```text
esp-claw-XXXXXX
```

`XXXXXX` matches the AP suffix. The firmware logs the active login hint on the
serial console.

## Secret Fields

`/api/config` does not return secret values. The Web Admin shows a placeholder
when a secret exists. Leaving a secret field blank or leaving the placeholder in
place preserves the previous value.

Secret fields include:

- Wi-Fi password
- AP password
- LLM API key
- Telegram bot token
- QQ/Feishu/WeChat secrets
- Brave/Tavily search keys
- Admin password
