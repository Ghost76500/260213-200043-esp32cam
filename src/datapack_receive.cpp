#include "datapack_receive.h"

#include <HardwareSerial.h>
#include <string.h>

uint8_t kNormalPower_t = 100; // 定义正常电量百分比，供 MQTT 状态上报使用

namespace {

constexpr uint8_t kHeader1 = 0x55;
constexpr uint8_t kHeader2 = 0xAA;
constexpr uint8_t kTail = 0x6B;

constexpr uint8_t kWarningNone = 0x00;
constexpr uint8_t kWarningHongwai = 0xAA;
constexpr uint8_t kWarningQingxie = 0xBB;

HardwareSerial &gDataUart = Serial2;

uint8_t gRxFrame[DATAPACK_SIZE] = {0};
uint8_t gLatestFrame[DATAPACK_SIZE] = {0};

size_t gRxIndex = 0;
bool gHasValidPacket = false;
bool gUpdatedFlag = false;

uint8_t gLatestWarningCode = kWarningNone;
bool gLatestHongwaiWarning = false;
bool gLatestQingxieWarning = false;

}  // namespace

void dataPackInitUart() {
	gDataUart.begin(DATAPACK_UART_BAUD, SERIAL_8N1, DATAPACK_UART_RX_PIN, DATAPACK_UART_TX_PIN);
}

void dataPackBuild(uint8_t (&data_packet)[DATAPACK_SIZE], bool flag_hongwai_warning, bool flag_qingxie_warning) {
	uint8_t warningCode = kWarningNone;
	if (flag_hongwai_warning) {
		warningCode = kWarningHongwai;
	}
	if (flag_qingxie_warning) {
		warningCode = kWarningQingxie;
	}

	data_packet[0] = kHeader1;
	data_packet[1] = kHeader2;
	data_packet[2] = 0x00;
	data_packet[3] = warningCode;
	data_packet[4] = 0x00;
	data_packet[5] = kNormalPower_t;
	data_packet[6] = 0x00;
	data_packet[7] = 0x00;
	data_packet[8] = 0x00;
	data_packet[9] = 0x00;
	data_packet[10] = kTail;
}

void dataPackSend(bool flag_hongwai_warning, bool flag_qingxie_warning) {
	uint8_t data_packet[DATAPACK_SIZE];
	dataPackBuild(data_packet, flag_hongwai_warning, flag_qingxie_warning);
	gDataUart.write(data_packet, DATAPACK_SIZE);
}

void dataPackPoll() {
	while (gDataUart.available() > 0) {
		uint8_t b = static_cast<uint8_t>(gDataUart.read());

		if (gRxIndex == 0) {
			if (b == kHeader1) {
				gRxFrame[gRxIndex++] = b;
			}
			continue;
		}

		if (gRxIndex == 1) {
			if (b == kHeader2) {
				gRxFrame[gRxIndex++] = b;
			} else if (b == kHeader1) {
				gRxFrame[0] = kHeader1;
				gRxIndex = 1;
			} else {
				gRxIndex = 0;
			}
			continue;
		}

		gRxFrame[gRxIndex++] = b;
		if (gRxIndex < DATAPACK_SIZE) {
			continue;
		}

		if (gRxFrame[DATAPACK_SIZE - 1] == kTail) {
			memcpy(gLatestFrame, gRxFrame, DATAPACK_SIZE);
			kNormalPower_t = gLatestFrame[5];
			gLatestWarningCode = gLatestFrame[3];
			gLatestHongwaiWarning = (gLatestWarningCode == kWarningHongwai);
			gLatestQingxieWarning = (gLatestWarningCode == kWarningQingxie);
			gHasValidPacket = true;
			gUpdatedFlag = true;
		}
		gRxIndex = 0;
	}
}

bool dataPackTakeUpdatedFlag() {
	bool ret = gUpdatedFlag;
	gUpdatedFlag = false;
	return ret;
}

bool dataPackHasValidPacket() {
	return gHasValidPacket;
}

uint8_t dataPackLatestWarningCode() {
	return gLatestWarningCode;
}

bool dataPackLatestIsHongwaiWarning() {
	return gLatestHongwaiWarning;
}

bool dataPackLatestIsQingxieWarning() {
	return gLatestQingxieWarning;
}

bool dataPackCopyLatest(uint8_t (&out_packet)[DATAPACK_SIZE]) {
	if (!gHasValidPacket) {
		return false;
	}

	memcpy(out_packet, gLatestFrame, DATAPACK_SIZE);
	return true;
}
