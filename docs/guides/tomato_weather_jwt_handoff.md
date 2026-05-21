# Tomato Glow QWeather JWT handoff

This branch continues the Tomato Glow Clock weather integration.

## What changed

- Weather requests can now use the account-specific QWeather API Host instead of the legacy `devapi.qweather.com` domain.
- JWT remains the preferred authentication path when `main/APP/qweather_jwt_secret.h` exists.
- API key auth is kept only as a fallback/diagnostic path when no JWT secret header is present.
- QWeather gzip responses are decoded before JSON parsing.
- JWT `iat` is backdated by 30 seconds to tolerate small device time-sync skew.
- `tools/generate_qweather_jwt_secret.py` converts an OpenSSL Ed25519 PKCS#8 private key into the private header expected by the firmware.

## Still needed

Do not commit real credentials.

- Set `CONFIG_TOMATO_QWEATHER_API_HOST` to the user's QWeather API Host from Console Settings, for example `abc1234xyz.def.qweatherapi.com`.
- Generate `main/APP/qweather_jwt_secret.h` from the user's Ed25519 private key.
- Rebuild, flash, and watch serial logs while entering the Tomato page.

## Generate the private JWT header

```bash
python3 tools/generate_qweather_jwt_secret.py \
  --private-key /path/to/ed25519-private.pem \
  --credential-id YOUR_QWEATHER_CREDENTIAL_ID \
  --project-id YOUR_QWEATHER_PROJECT_ID
```

The generated `main/APP/qweather_jwt_secret.h` is ignored by git.

## Build and flash

```bash
source /Users/tupi/esp/esp-idf-v5.5/export.sh >/dev/null
idf.py build
idf.py -p /dev/cu.usbserial-21230 flash
```

Use `zsh`/the default shell for ESP-IDF export on this machine. A previous `bash` invocation selected the wrong Python environment.

## Expected statuses

- `Set QWeather JWT`: private JWT header is missing or incomplete.
- `Time sync failed`: Wi-Fi/SNTP did not provide a valid time yet.
- `QWeather host denied`: API Host is wrong for the credential/account.
- `QWeather auth failed`: JWT was rejected; validate `kid`, `sub`, private key, uploaded public key, and device time.
- `QWeather updated (JWT)`: weather update succeeded through JWT auth.
