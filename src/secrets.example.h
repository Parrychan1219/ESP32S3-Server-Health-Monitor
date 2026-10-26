// secrets.example.h
//
// Copy this file to secrets.h and fill in your own values.
// secrets.h is gitignored and must never be committed.
//
//     cp src/secrets.example.h src/secrets.h

#ifndef SECRETS_H
#define SECRETS_H

// Talk to @BotFather on Telegram to create a bot and get the token.
#define TELEGRAM_BOT_TOKEN "123456789:AAxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"

// Your numeric chat ID. Message @userinfobot to find it.
#define TELEGRAM_CHAT_ID   "123456789"

#define WIFI_SSID          "your-ssid"
#define WIFI_PASSWORD      "your-wifi-password"

// Optional. If defined, OTA uploads require this password and ota.sh
// picks it up automatically.
// #define OTA_PASSWORD    "choose-something"

// HomeKit pairing code (the Home app asks for it): exactly 8 digits, no dashes.
// MUST be a random, non-trivial code. HomeKit rejects 12345678, 87654321 and
// all-same digits, and the firmware refuses to start HomeKit at all while this
// is the 00000000 placeholder or missing (it never falls back to HomeSpan's
// well-known default code). Anyone on the LAN who knows it can pair.
#define HOMEKIT_SETUP_CODE "00000000"

#endif
