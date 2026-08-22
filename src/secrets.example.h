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

#endif
