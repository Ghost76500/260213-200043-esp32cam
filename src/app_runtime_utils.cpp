#include "app_runtime_utils.h"

#include "FS.h"
#include "SD_MMC.h"

String appNextRecordPath() {
  for (uint16_t i = 1; i <= 9999; ++i) {
    char path[32];
    snprintf(path, sizeof(path), "/record_%04u.avi", i);
    if (!SD_MMC.exists(path)) {
      return String(path);
    }
  }
  return String("/record_last.avi");
}

String appNormalPowerText(uint8_t powerValue) {
  if (powerValue > 100) {
    powerValue = 100;
  }
  return String(powerValue);
}

bool appInitSdCard1Bit() {
  SD_MMC.end();
  delay(20);

  Serial.println("[SD] init start (SD_MMC 1-bit mode)");

  const bool mode1bit = true;
  bool ok = SD_MMC.begin("/sdcard", mode1bit);
  if (!ok) {
    Serial.println("[SD] mount failed (1-bit)");
    Serial.println("[SD] check card insert, card format(FAT32/exFAT), and pull-up resistors.");
    return false;
  }

  uint8_t cardType = SD_MMC.cardType();
  if (cardType == CARD_NONE) {
    Serial.println("[SD] no card (1-bit)");
    SD_MMC.end();
    return false;
  }

  uint64_t cardSizeMB = SD_MMC.cardSize() / (1024ULL * 1024ULL);
  Serial.print("[SD] mount ok, mode=1-bit type=");
  if (cardType == CARD_MMC) {
    Serial.print("MMC");
  } else if (cardType == CARD_SD) {
    Serial.print("SDSC");
  } else if (cardType == CARD_SDHC) {
    Serial.print("SDHC/SDXC");
  } else {
    Serial.print("UNKNOWN");
  }
  Serial.print(", size=");
  Serial.print((unsigned long)cardSizeMB);
  Serial.println("MB");
  return true;
}

String appNowText() {
  uint32_t sec = millis() / 1000;
  uint32_t hh = (sec / 3600) % 24;
  uint32_t mm = (sec / 60) % 60;
  uint32_t ss = sec % 60;

  char buf[32];
  snprintf(buf, sizeof(buf), "2026-02-30 %02lu:%02lu:%02lu", (unsigned long)hh, (unsigned long)mm, (unsigned long)ss);
  return String(buf);
}
