/* BionXtool.c */
/* ====================================================================
 * Copyright (c) 2026 by Ben Buxton <bbuxton@gmail.com>
 * Based on BigXionFlasher:
 * Copyright (c) 2011-2017 by Thomas König <info@bigxionflasher.org>.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 *
 * 3. All advertising materials mentioning features or use of this
 *    software must display the following acknowledgment:
 *    "This product includes software developed by the
 *    BigXionFlasher Project. (http://www.bigxionflasher.org/)"
 *
 * 4. The name "BigXionFlasher" must not be used to
 *    endorse or promote products derived from this software without
 *    prior written permission. For written permission, please contact
 *    info@bigxionflasher.org.
 *
 * 5. Products derived from this software may not be called "BigXionFlasher"
 *    nor may "BigXionFlasher" appear in their names without prior written
 *    permission of the BigXionFlasher Project.
 *
 * 6. Redistributions of any form whatsoever must retain the following
 *    acknowledgment:
 *    "This product includes software developed by the
 *    BigXionFlasher Project. (http://www.bigxionflasher.org/)"
 *
 * THIS SOFTWARE IS PROVIDED BY THE BigXionFlasher PROJECT ``AS IS'' AND ANY
 * EXPRESSED OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE BigXionFlasher PROJECT OR
 * ITS CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
 * STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED
 * OF THE POSSIBILITY OF SUCH DAMAGE.
 * ====================================================================
 *
 */
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <net/if.h>
#include <linux/can.h>
#include <linux/can/raw.h>
#include <poll.h>
#include <sys/time.h>
#include <ncurses.h>

#define _NL "\n"
#define _DEGREE_SIGN "°"

#define __DOSTR(v) #v
#define __STR(v) __DOSTR(v)

#define __BXF_VERSION__ "V 0.1 (BionXtool)"

#define UNLIMITED_SPEED_VALUE			70 /* Km/h */
#define UNLIMITED_MIN_SPEED_VALUE		30 /* Km/h */
#define MAX_THROTTLE_SPEED_VALUE		70 /* Km/h */

#include "registers.h"

#define TIMEOUT_VALUE					80
#define TIMEOUT_MS						10 // 10ms

#define doSleep(x) usleep(x*1000)

int gAssistInitLevel = -1, gPrintSystemSettings = 0, gSkipShutdown = 0, gPowerOff = 0, gConsoleSetSlaveMode = 1, gNoSerialNumbers = 0, gSetMountainCap = -1, gSetWheelCircumference = 0, gSniffMode = 0, gForceSlaveMode = 0;
int gReadSpecificReg = 0, gWriteSpecificReg = 0, gSetAccessoryPower = -1, gMonitorMode = 0, gSortLatestFirst = 0, gSniffOnlyChanges = 0, gSetRTC = 0;
char *gPcapFile = NULL;

/* State for 'only changed' sniffing */
unsigned char gSniffValueMap[256][256];
unsigned char gSniffSeenMap[256][32]; // bitmask for 256 registers per node

struct decoded_frame {
    char timestamp[64];
    unsigned int can_id;
    char src[16];
    char dst[16];
    const char *type;
    const char *regName;
    unsigned char reg;
    unsigned char val;
    int hasReg;
    int hasVal;
    unsigned char targetNode;
    unsigned char dlc;
    unsigned char data[8];
    const struct reg_metadata *meta;
    char val_str[128];
};

struct monitored_reg {
    unsigned char node;
    unsigned char reg;
    unsigned int last_val;
    struct timeval last_seen;
    long first_seen_order;
    char val_str[128];
    const char *reg_name;
};

#define MAX_MONITORED 2048
struct monitored_reg gMonitoredRegs[MAX_MONITORED];
short gMonitorMap[256][256]; // -1 if not seen
int gTotalMonitored = 0;
long gFirstSeenCounter = 0;

unsigned char gTargetNode = 0;
unsigned char gTargetReg = 0;
unsigned char gWriteNode = 0, gWriteReg = 0, gWriteVal = 0;
double gSetSpeedLimit = -1, gSetMinSpeedLimit = -1, gSetThrottleSpeedLimit = -1, gSetAccessoryVoltage = -1;
char *gCanInterface = "can0";
int can_sock = -1;

unsigned char gLastRequestTo[256] = {0};



const char *getNodeName(unsigned char id)
{
	static char nodeNames[4][16];
	static int idx = 0;
	char *nodeName = nodeNames[idx++ % 4];
	if (id == 0x08 || id == 0x48)
		return "console";
	if (id == 0x10 || id == 0x50)
		return "battery";
	if (id == 0x20 || id == 0x60)
		return "motor";
	if (id == BIB)
		return "bib";
	snprintf(nodeName, 16, "0x%02X", id);
	return nodeName;
}

unsigned char getBaseNodeId(unsigned char id)
{
	if (id == 0x08 || id == 0x48)
		return CONSOLE;
	if (id == 0x10 || id == 0x50)
		return BATTERY;
	if (id == 0x20 || id == 0x60)
		return MOTOR;
	return id;
}

unsigned char getNodeIdByName(const char *name)
{
	if (strcmp(name, "console") == 0) return 0x48; // CONSOLE
	if (strcmp(name, "battery") == 0) return 0x50; // BATTERY
	if (strcmp(name, "motor") == 0) return 0x60;   // MOTOR
	if (strcmp(name, "bib") == 0) return 0x58;     // BIB
	if (strncmp(name, "0x", 2) == 0) return (unsigned char)strtol(name, NULL, 16);
	return 0;
}

enum val_format {
	F_RAW,
	F_BOOL,
	F_VERSION,
	F_KPH,       /* 1x */
	F_KPH_D1,    /* scaled by 0.1 */
	F_VOLT_D1,   /* 0.1 */
	F_VOLT_D3,   /* 0.001 */
	F_VOLT_NORM, /* 0.416667x + 20.8333, output as % */
	F_PCT_ASSIST, /* 1.5625 */
	F_PCT_BATT,  /* 6.6667 */
	F_TEMP_C,
	F_TEMP_C_S,  /* Signed temperature */
	F_AH_D3,     /* 0.001 */
	F_AH_LMD,    /* 0.002142 */
	F_KM_D1,     /* 0.1 */
	F_SN,        /* 5 digits */
	F_DATE,      /* DD/MM/YYYY */
	F_RTC,       /* DDd HH:MM:SS */
	F_UINT16_BE,
	F_UINT32_BE
};

struct reg_metadata {
	unsigned char node;
	unsigned char reg;
	const char *name;
	enum val_format format;
};

static struct reg_metadata bionx_registers[] = {
	/* Console Registers */
	{CONSOLE, REG_CONSOLE_STATISTIC_DIST_HI, "REG_CONSOLE_STATISTIC_DIST_HI", F_KM_D1},
	{CONSOLE, REG_CONSOLE_STATISTIC_DIST_LO, "REG_CONSOLE_STATISTIC_DIST_LO", F_RAW},
	{CONSOLE, REG_CONSOLE_STATISTIC_AVGSPEED_HI, "REG_CONSOLE_STATISTIC_AVGSPEED_HI", F_KPH_D1},
	{CONSOLE, REG_CONSOLE_STATISTIC_AVGSPEED_LO, "REG_CONSOLE_STATISTIC_AVGSPEED_LO", F_RAW},
	{CONSOLE, REG_CONSOLE_STATISTIC_ODOMETER_HIHI, "REG_CONSOLE_STATISTIC_ODOMETER_HIHI", F_KM_D1},
	{CONSOLE, REG_CONSOLE_STATISTIC_ODOMETER_HILO, "REG_CONSOLE_STATISTIC_ODOMETER_HILO", F_RAW},
	{CONSOLE, REG_CONSOLE_STATISTIC_ODOMOTER_LOHI, "REG_CONSOLE_STATISTIC_ODOMOTER_LOHI", F_RAW},
	{CONSOLE, REG_CONSOLE_STATISTIC_ODOMETER_LOLO, "REG_CONSOLE_STATISTIC_ODOMETER_LOLO", F_RAW},
	{CONSOLE, REG_CONSOLE_THROTTLE_CALIBRATED, "REG_CONSOLE_THROTTLE_CALIBRATED", F_BOOL},
	{CONSOLE, REG_CONSOLE_STATISTIC_CHRONO_SECOND, "REG_CONSOLE_STATISTIC_CHRONO_SECOND", F_RAW},
	{CONSOLE, REG_CONSOLE_STATISTIC_CHRONO_MINUTE, "REG_CONSOLE_STATISTIC_CHRONO_MINUTE", F_RAW},
	{CONSOLE, REG_CONSOLE_STATISTIC_CHRONO_HOUR, "REG_CONSOLE_STATISTIC_CHRONO_HOUR", F_RAW},
	{CONSOLE, REG_CONSOLE_PREFERENCE_LCD_CONTRAST, "REG_CONSOLE_PREFERENCE_LCD_CONTRAST", F_RAW},
	{CONSOLE, REG_CONSOLE_SN_LOCATION, "REG_CONSOLE_SN_LOCATION", F_RAW},
	{CONSOLE, REG_CONSOLE_SN_YEAR, "REG_CONSOLE_SN_YEAR", F_RAW},
	{CONSOLE, REG_CONSOLE_SN_MONTH, "REG_CONSOLE_SN_MONTH", F_RAW},
	{CONSOLE, REG_CONSOLE_SN_DAY, "REG_CONSOLE_SN_DAY", F_RAW},
	{CONSOLE, REG_CONSOLE_SN_PN_HI, "REG_CONSOLE_SN_PN_HI", F_SN},
	{CONSOLE, REG_CONSOLE_SN_PN_LO, "REG_CONSOLE_SN_PN_LO", F_RAW},
	{CONSOLE, REG_CONSOLE_SN_ITEM_HI, "REG_CONSOLE_SN_ITEM_HI", F_SN},
	{CONSOLE, REG_CONSOLE_SN_ITEM_LO, "REG_CONSOLE_SN_ITEM_LO", F_RAW},
	{CONSOLE, REG_CONSOLE_SN_TYPE, "REG_CONSOLE_SN_TYPE", F_RAW},
	{CONSOLE, REG_CONSOLE_SN_OEM_HI, "REG_CONSOLE_SN_OEM_HI", F_RAW},
	{CONSOLE, REG_CONSOLE_SN_OEM_LO, "REG_CONSOLE_SN_OEM_LO", F_RAW},
	{CONSOLE, REG_CONSOLE_SN_PRODUCT_HI, "REG_CONSOLE_SN_PRODUCT_HI", F_RAW},
	{CONSOLE, REG_CONSOLE_SN_PRODUCT_LO, "REG_CONSOLE_SN_PRODUCT_LO", F_RAW},
	{CONSOLE, REG_CONSOLE_GEOMETRY_CIRC_HI, "REG_CONSOLE_GEOMETRY_CIRC_HI", F_UINT16_BE},
	{CONSOLE, REG_CONSOLE_GEOMETRY_CIRC_LO, "REG_CONSOLE_GEOMETRY_CIRC_LO", F_RAW},
	{CONSOLE, REG_CONSOLE_ASSIST_MAXSPEED_FLAG, "REG_CONSOLE_ASSIST_MAXSPEED_FLAG", F_BOOL},
	{CONSOLE, REG_CONSOLE_ASSIST_MAXSPEED_HI, "REG_CONSOLE_ASSIST_MAXSPEED_HI", F_KPH_D1},
	{CONSOLE, REG_CONSOLE_ASSIST_MAXSPEED_LO, "REG_CONSOLE_ASSIST_MAXSPEED_LO", F_RAW},
	{CONSOLE, REG_CONSOLE_THROTTLE_MAXSPEED_FLAG, "REG_CONSOLE_THROTTLE_MAXSPEED_FLAG", F_BOOL},
	{CONSOLE, REG_CONSOLE_THROTTLE_MAXSPEED_HI, "REG_CONSOLE_THROTTLE_MAXSPEED_HI", F_KPH_D1},
	{CONSOLE, REG_CONSOLE_THROTTLE_MAXSPEED_LO, "REG_CONSOLE_THROTTLE_MAXSPEED_LO", F_RAW},
	{CONSOLE, REG_CONSOLE_ASSIST_MINSPEED_FLAG, "REG_CONSOLE_ASSIST_MINSPEED_FLAG", F_BOOL},
	{CONSOLE, REG_CONSOLE_ASSIST_MINSPEED, "REG_CONSOLE_ASSIST_MINSPEED", F_KPH_D1},
	{CONSOLE, REG_CONSOLE_REV_SW, "REG_CONSOLE_REV_SW", F_VERSION},
	{CONSOLE, REG_CONSOLE_REV_HW, "REG_CONSOLE_REV_HW", F_VERSION},
	{CONSOLE, REG_CONSOLE_ASSIST_INITLEVEL, "REG_CONSOLE_ASSIST_INITLEVEL", F_RAW},
	{CONSOLE, REG_CONSOLE_REV_SUB, "REG_CONSOLE_REV_SUB", F_RAW},
	{CONSOLE, REG_CONSOLE_PREFERENCE_LIGHT_ON_AT_START, "REG_CONSOLE_PREFERENCE_LIGHT_ON_AT_START", F_BOOL},
	{CONSOLE, REG_CONSOLE_PREFERENCE_CODES_HIHI, "REG_CONSOLE_PREFERENCE_CODES_HIHI", F_RAW},
	{CONSOLE, REG_CONSOLE_PREFERENCE_CODES_HILO, "REG_CONSOLE_PREFERENCE_CODES_HILO", F_RAW},
	{CONSOLE, REG_CONSOLE_PREFERENCE_CODES_LOHI, "REG_CONSOLE_PREFERENCE_CODES_LOHI", F_RAW},
	{CONSOLE, REG_CONSOLE_PREFERENCE_CODES_LOLO, "REG_CONSOLE_PREFERENCE_CODES_LOLO", F_RAW},
	{CONSOLE, REG_CONSOLE_ASSIST_MOUNTAIN_CAP, "REG_CONSOLE_ASSIST_MOUNTAIN_CAP", F_PCT_ASSIST},
	{CONSOLE, REG_CONSOLE_STATUS_SLAVE, "REG_CONSOLE_STATUS_SLAVE", F_BOOL},

	/* Battery Registers */
	{BATTERY, REG_BATTERY_STATUS_FLAGS_HI, "REG_BATTERY_STATUS_FLAGS_HI", F_RAW},
	{BATTERY, REG_BATTERY_STATUS_FLAGS_LO, "REG_BATTERY_STATUS_FLAGS_LO", F_RAW},
	{BATTERY, REG_BATTERY_STATUS_CELLPACK_CURRENT_HI, "REG_BATTERY_STATUS_CELLPACK_CURRENT_HI", F_AH_D3},
	{BATTERY, REG_BATTERY_STATUS_CELLPACK_CURRENT_LO, "REG_BATTERY_STATUS_CELLPACK_CURRENT_LO", F_RAW},
	{BATTERY, REG_BATTERY_STATUS_BATTERY_VOLTAGE_NORMALIZED, "REG_BATTERY_STATUS_BATTERY_VOLTAGE_NORMALIZED", F_VOLT_NORM},
	{BATTERY, REG_BATTERY_REV_SW, "REG_BATTERY_REV_SW", F_VERSION},
	{BATTERY, REG_BATTERY_REV_HW, "REG_BATTERY_REV_HW", F_VERSION},
	{BATTERY, REG_BATTERY_REV_SUB, "REG_BATTERY_REV_SUB", F_RAW},
	{BATTERY, REG_BATTERY_REV_BOM, "REG_BATTERY_REV_BOM", F_RAW},
	{BATTERY, REG_BATTERY_SN_PN_HI, "REG_BATTERY_SN_PN_HI", F_SN},
	{BATTERY, REG_BATTERY_SN_PN_LO, "REG_BATTERY_SN_PN_LO", F_RAW},
	{BATTERY, REG_BATTERY_SN_ITEM_HI, "REG_BATTERY_SN_ITEM_HI", F_SN},
	{BATTERY, REG_BATTERY_SN_ITEM_LO, "REG_BATTERY_SN_ITEM_LO", F_RAW},
	{BATTERY, REG_BATTERY_SN_YEAR, "REG_BATTERY_SN_YEAR", F_RAW},
	{BATTERY, REG_BATTERY_SN_MONTH, "REG_BATTERY_SN_MONTH", F_RAW},
	{BATTERY, REG_BATTERY_SN_DAY, "REG_BATTERY_SN_DAY", F_RAW},
	{BATTERY, REG_BATTERY_SN_LOCATION, "REG_BATTERY_SN_LOCATION", F_RAW},
	{BATTERY, REG_BATTERY_SN_CELLPACK_HI, "REG_BATTERY_SN_CELLPACK_HI", F_RAW},
	{BATTERY, REG_BATTERY_SN_CELLPACK_LO, "REG_BATTERY_SN_CELLPACK_LO", F_RAW},
	{BATTERY, REG_BATTERY_STATUS_BATTERY_VOLTAGE_HI, "REG_BATTERY_STATUS_BATTERY_VOLTAGE_HI", F_VOLT_D3},
	{BATTERY, REG_BATTERY_STATUS_BATTERY_VOLTAGE_LO, "REG_BATTERY_STATUS_BATTERY_VOLTAGE_LO", F_RAW},
	{BATTERY, REG_BATTERY_STATUS_INTERNAL_BATTERY_VOLTAGE_HI, "REG_BATTERY_STATUS_INTERNAL_BATTERY_VOLTAGE_HI", F_VOLT_D3},
	{BATTERY, REG_BATTERY_STATUS_INTERNAL_BATTERY_VOLTAGE_LO, "REG_BATTERY_STATUS_INTERNAL_BATTERY_VOLTAGE_LO", F_RAW},
	{BATTERY, REG_BATTERY_STATUS_CONSOLE_VOLTAGE_HI, "REG_BATTERY_STATUS_CONSOLE_VOLTAGE_HI", F_VOLT_D3},
	{BATTERY, REG_BATTERY_STATUS_CONSOLE_VOLTAGE_LO, "REG_BATTERY_STATUS_CONSOLE_VOLTAGE_LO", F_RAW},
	{BATTERY, REG_BATTERY_STATUS_12V_VOLTAGE_HI, "REG_BATTERY_STATUS_12V_VOLTAGE_HI", F_VOLT_D3},
	{BATTERY, REG_BATTERY_STATUS_12V_VOLTAGE_LO, "REG_BATTERY_STATUS_12V_VOLTAGE_LO", F_RAW},
	{BATTERY, REG_BATTERY_STATUS_ACCESSORY_VOLTAGE_HI, "REG_BATTERY_STATUS_ACCESSORY_VOLTAGE_HI", F_VOLT_D3},
	{BATTERY, REG_BATTERY_STATUS_ACCESSORY_VOLTAGE_LO, "REG_BATTERY_STATUS_ACCESSORY_VOLTAGE_LO", F_RAW},
	{BATTERY, REG_BATTERY_STATUS_DCIN_VOLTAGE_HI, "REG_BATTERY_STATUS_DCIN_VOLTAGE_HI", F_VOLT_D3},
	{BATTERY, REG_BATTERY_STATUS_DCIN_VOLTAGE_LO, "REG_BATTERY_STATUS_DCIN_VOLTAGE_LO", F_RAW},
	{BATTERY, REG_BATTERY_STATUS_POWER_VOLTAGE_HI, "REG_BATTERY_STATUS_POWER_VOLTAGE_HI", F_VOLT_D3},
	{BATTERY, REG_BATTERY_STATUS_POWER_VOLTAGE_LO, "REG_BATTERY_STATUS_POWER_VOLTAGE_LO", F_RAW},
	{BATTERY, REG_BATTERY_STATUS_CONTROL_VOLTAGE_HI, "REG_BATTERY_STATUS_CONTROL_VOLTAGE_HI", F_VOLT_D3},
	{BATTERY, REG_BATTERY_STATUS_CONTROL_VOLTAGE_LO, "REG_BATTERY_STATUS_CONTROL_VOLTAGE_LO", F_RAW},
	{BATTERY, REG_BATTERY_CONFIG_ACCESSORY_VOLTAGE, "REG_BATTERY_CONFIG_ACCESSORY_VOLTAGE", F_VOLT_D1},
	{BATTERY, REG_BATTERY_STATUS_REMAINING, "REG_BATTERY_STATUS_REMAINING", F_RAW},
	{BATTERY, REG_BATTERY_STATUS_CHARGE_PLUG, "REG_BATTERY_STATUS_CHARGE_PLUG", F_BOOL},
	{BATTERY, REG_BATTERY_STATUS_CHARGE_LEVEL, "REG_BATTERY_STATUS_CHARGE_LEVEL", F_PCT_BATT},
	{BATTERY, REG_BATTERY_STATUS_TEMPERATURE_SENSOR_1, "REG_BATTERY_STATUS_TEMPERATURE_SENSOR_1", F_TEMP_C},
	{BATTERY, REG_BATTERY_STATUS_TEMPERATURE_SENSOR_2, "REG_BATTERY_STATUS_TEMPERATURE_SENSOR_2", F_TEMP_C},
	{BATTERY, REG_BATTERY_STATUS_TEMPERATURE_SENSOR_3, "REG_BATTERY_STATUS_TEMPERATURE_SENSOR_3", F_TEMP_C},
	{BATTERY, REG_BATTERY_STATUS_TEMPERATURE_SENSOR_4, "REG_BATTERY_STATUS_TEMPERATURE_SENSOR_4", F_TEMP_C},
	{BATTERY, REG_BATTERY_STATUS_ESTIMATED_SOC, "REG_BATTERY_STATUS_ESTIMATED_SOC", F_RAW},
	{BATTERY, REG_BATTERY_RTC_TIME_HIHI, "REG_BATTERY_RTC_TIME_HIHI", F_RTC},
	{BATTERY, REG_BATTERY_RTC_TIME_HILO, "REG_BATTERY_RTC_TIME_HILO", F_RAW},
	{BATTERY, REG_BATTERY_RTC_TIME_LOHI, "REG_BATTERY_RTC_TIME_LOHI", F_RAW},
	{BATTERY, REG_BATTERY_RTC_TIME_LOLO, "REG_BATTERY_RTC_TIME_LOLO", F_RAW},
	{BATTERY, REG_BATTERY_RTC_STATUS, "REG_BATTERY_RTC_STATUS", F_RAW},
	{BATTERY, REG_BATTERY_STATISTIC_WATCHDOG_RESET_COUNT, "REG_BATTERY_STATISTIC_WATCHDOG_RESET_COUNT", F_RAW},
	{BATTERY, REG_BATTERY_STATISTIC_BATTERY_MAX_VOLTAGE, "REG_BATTERY_STATISTIC_BATTERY_MAX_VOLTAGE", F_VOLT_NORM},
	{BATTERY, REG_BATTERY_STATISTIC_BATTERY_MIN_VOLTAGE, "REG_BATTERY_STATISTIC_BATTERY_MIN_VOLTAGE", F_VOLT_NORM},
	{BATTERY, REG_BATTERY_STATISTIC_BATTERY_AVGVOLTAGE_NORMALIZED, "REG_BATTERY_STATISTIC_BATTERY_AVGVOLTAGE_NORMALIZED", F_VOLT_NORM},
	{BATTERY, REG_BATTERY_STATISTIC_RESETS_HI, "REG_BATTERY_STATISTIC_RESETS_HI", F_UINT16_BE},
	{BATTERY, REG_BATTERY_STATISTIC_RESETS_LO, "REG_BATTERY_STATISTIC_RESETS_LO", F_RAW},
	{BATTERY, REG_BATTERY_STATISTIC_LMD_HI, "REG_BATTERY_STATISTIC_LMD_HI", F_AH_LMD},
	{BATTERY, REG_BATTERY_STATISTIC_LMD_LO, "REG_BATTERY_STATISTIC_LMD_LO", F_RAW},
	{BATTERY, REG_BATTERY_CONFIG_CELLCAPACITY_HI, "REG_BATTERY_CONFIG_CELLCAPACITY_HI", F_AH_D3},
	{BATTERY, REG_BATTERY_CONFIG_CELLCAPACITY_LO, "REG_BATTERY_CONFIG_CELLCAPACITY_LO", F_RAW},
	{BATTERY, REG_BATTERY_STATISTIC_CHARGETIME_WORST_HI, "REG_BATTERY_STATISTIC_CHARGETIME_WORST_HI", F_UINT16_BE},
	{BATTERY, REG_BATTERY_STATISTIC_CHARGETIME_WORST_LO, "REG_BATTERY_STATISTIC_CHARGETIME_WORST_LO", F_RAW},
	{BATTERY, REG_BATTERY_STATISTIC_CHARGETIME_MEAN_HI, "REG_BATTERY_STATISTIC_CHARGETIME_MEAN_HI", F_UINT16_BE},
	{BATTERY, REG_BATTERY_STATISTIC_CHARGETIME_MEAN_LO, "REG_BATTERY_STATISTIC_CHARGETIME_MEAN_LO", F_RAW},
	{BATTERY, REG_BATTERY_STATISTIC_BATTERY_CYCLES_HI, "REG_BATTERY_STATISTIC_BATTERY_CYCLES_HI", F_UINT16_BE},
	{BATTERY, REG_BATTERY_STATISTIC_BATTERY_CYCLES_LO, "REG_BATTERY_STATISTIC_BATTERY_CYCLES_LO", F_RAW},
	{BATTERY, REG_BATTERY_STATISTIC_BATTERY_FULL_CYCLES_HI, "REG_BATTERY_STATISTIC_BATTERY_FULL_CYCLES_HI", F_UINT16_BE},
	{BATTERY, REG_BATTERY_STATISTIC_BATTERY_FULL_CYCLES_LO, "REG_BATTERY_STATISTIC_BATTERY_FULL_CYCLES_LO", F_RAW},
	{BATTERY, REG_BATTERY_STATISTIC_POWER_CYCLES_HI, "REG_BATTERY_STATISTIC_POWER_CYCLES_HI", F_UINT16_BE},
	{BATTERY, REG_BATTERY_STATISTIC_POWER_CYCLES_LO, "REG_BATTERY_STATISTIC_POWER_CYCLES_LO", F_RAW},
	{BATTERY, REG_BATTERY_STATISTIC_TEMPERATURE_MAX, "REG_BATTERY_STATISTIC_TEMPERATURE_MAX", F_TEMP_C_S},
	{BATTERY, REG_BATTERY_STATISTIC_TEMPERATURE_MIN, "REG_BATTERY_STATISTIC_TEMPERATURE_MIN", F_TEMP_C_S},
	{BATTERY, REG_BATTERY_CELLMON_BALANCER_ENABLED, "REG_BATTERY_CELLMON_BALANCER_ENABLED", F_BOOL},
	{BATTERY, REG_BATTERY_ALARM_ENABLE, "REG_BATTERY_ALARM_ENABLE", F_BOOL},
	{BATTERY, REG_BATTERY_CONFIG_POWER_VOLTAGE_ENABLE, "REG_BATTERY_CONFIG_POWER_VOLTAGE_ENABLE", F_BOOL},
	{BATTERY, REG_BATTERY_CONFIG_ACCESSORY_ENABLED, "REG_BATTERY_CONFIG_ACCESSORY_ENABLED", F_BOOL},
	{BATTERY, REG_BATTERY_CONFIG_SHUTDOWN, "REG_BATTERY_CONFIG_SHUTDOWN", F_RAW},
	{BATTERY, REG_BATTERY_CONFIG_CONTROL_VOLTAGE_ENABLE, "REG_BATTERY_CONFIG_CONTROL_VOLTAGE_ENABLE", F_BOOL},
	{BATTERY, REG_BATTERY_CONFIG_CAP_SENSE_MODE, "REG_BATTERY_CONFIG_CAP_SENSE_MODE", F_RAW},
	{BATTERY, REG_BATTERY_CONFIG_COMMUNICATION_MODE, "REG_BATTERY_CONFIG_COMMUNICATION_MODE", F_RAW},
	{BATTERY, REG_BATTERY_CONFIG_SHIPMODE, "REG_BATTERY_CONFIG_SHIPMODE", F_RAW},
	{BATTERY, REG_BATTERY_CONFIG_TYPE, "REG_BATTERY_CONFIG_TYPE", F_RAW},
	{BATTERY, REG_BATTERY_CONFIG_TAILLAMP_INTENSITY, "REG_BATTERY_CONFIG_TAILLAMP_INTENSITY", F_RAW},
	{BATTERY, REG_BATTERY_CONFIG_ACCESSORY_MOUNTED, "REG_BATTERY_CONFIG_ACCESSORY_MOUNTED", F_BOOL},
	{BATTERY, REG_BATTERY_CONFIG_BATTINT_VOLTAGE_ENABLE, "REG_BATTERY_CONFIG_BATTINT_VOLTAGE_ENABLE", F_BOOL},
	{BATTERY, REG_BATTERY_CONFIG_FORCE_DONE, "REG_BATTERY_CONFIG_FORCE_DONE", F_RAW},
	{BATTERY, REG_BATTERY_CONFIG_PACK_SERIAL, "REG_BATTERY_CONFIG_PACK_SERIAL", F_RAW},
	{BATTERY, REG_BATTERY_CONFIG_PACK_PARALLEL, "REG_BATTERY_CONFIG_PACK_PARALLEL", F_RAW},
	{BATTERY, REG_BATTERY_CONFIG_MAX_CHARGE_HI, "REG_BATTERY_CONFIG_MAX_CHARGE_HI", F_UINT16_BE},
	{BATTERY, REG_BATTERY_CONFIG_MAX_CHARGE_LO, "REG_BATTERY_CONFIG_MAX_CHARGE_LO", F_RAW},
	{BATTERY, REG_BATTERY_CONFIG_MAX_DISCHARGE_HI, "REG_BATTERY_CONFIG_MAX_DISCHARGE_HI", F_UINT16_BE},
	{BATTERY, REG_BATTERY_CONFIG_MAX_DISCHARGE_LO, "REG_BATTERY_CONFIG_MAX_DISCHARGE_LO", F_RAW},
	{BATTERY, REG_BATTERY_CONFIG_CELLCAPACITY_HI, "REG_BATTERY_CONFIG_CELLCAPACITY_HI", F_AH_D3},
	{BATTERY, REG_BATTERY_CONFIG_CELLCAPACITY_LO, "REG_BATTERY_CONFIG_CELLCAPACITY_LO", F_RAW},
	{BATTERY, REG_BATTERY_GASGAGE_TEMPERATUR_HI, "REG_BATTERY_GASGAGE_TEMPERATUR_HI", F_TEMP_C},
	{BATTERY, REG_BATTERY_GASGAGE_TEMPERATUR_LO, "REG_BATTERY_GASGAGE_TEMPERATUR_LO", F_RAW},
	{BATTERY, REG_BATTERY_PROTECT_UNLOCK, "REG_BATTERY_PROTECT_UNLOCK", F_RAW},

	/* Motor Registers */
	{MOTOR, REG_MOTOR_SET_IDLE, "REG_MOTOR_SET_IDLE", F_RAW},
	{MOTOR, REG_MOTOR_SET_WAKEUP, "REG_MOTOR_SET_WAKEUP", F_RAW},
	{MOTOR, REG_MOTOR_SET_3KMH, "REG_MOTOR_SET_3KMH", F_BOOL},
	{MOTOR, REG_MOTOR_REV_SW, "REG_MOTOR_REV_SW", F_VERSION},
	{MOTOR, REG_MOTOR_REV_HW, "REG_MOTOR_REV_HW", F_VERSION},
	{MOTOR, REG_MOTOR_REV_SUB, "REG_MOTOR_REV_SUB", F_RAW},
	{MOTOR, REG_MOTOR_STATUS_MAIN, "REG_MOTOR_STATUS_MAIN", F_RAW},
	{MOTOR, REG_MOTOR_STATUS_TEMPERATURE, "REG_MOTOR_STATUS_TEMPERATURE", F_TEMP_C},
	{MOTOR, REG_MOTOR_ASSIST_MAXSPEED, "REG_MOTOR_ASSIST_MAXSPEED", F_KPH},
	{MOTOR, REG_MOTOR_GEOMETRY_CIRC_HI, "REG_MOTOR_GEOMETRY_CIRC_HI", F_UINT16_BE},
	{MOTOR, REG_MOTOR_GEOMETRY_CIRC_LO, "REG_MOTOR_GEOMETRY_CIRC_LO", F_RAW},
	{MOTOR, REG_MOTOR_SN_PN_HI, "REG_MOTOR_SN_PN_HI", F_SN},
	{MOTOR, REG_MOTOR_SN_PN_LO, "REG_MOTOR_SN_PN_LO", F_RAW},
	{MOTOR, REG_MOTOR_SN_ITEM_HI, "REG_MOTOR_SN_ITEM_HI", F_SN},
	{MOTOR, REG_MOTOR_SN_ITEM_LO, "REG_MOTOR_SN_ITEM_LO", F_RAW},
	{MOTOR, REG_MOTOR_SN_YEAR, "REG_MOTOR_SN_YEAR", F_RAW},
	{MOTOR, REG_MOTOR_SN_MONTH, "REG_MOTOR_SN_MONTH", F_RAW},
	{MOTOR, REG_MOTOR_SN_DAY, "REG_MOTOR_SN_DAY", F_RAW},
	{MOTOR, REG_MOTOR_SN_LOCATION, "REG_MOTOR_SN_LOCATION", F_RAW},
	{MOTOR, REG_MOTOR_SN_PRODUCT_HI, "REG_MOTOR_SN_PRODUCT_HI", F_RAW},
	{MOTOR, REG_MOTOR_SN_PRODUCT_LO, "REG_MOTOR_SN_PRODUCT_LO", F_RAW},
	{MOTOR, REG_MOTOR_SN_OEM_HI, "REG_MOTOR_SN_OEM_HI", F_RAW},
	{MOTOR, REG_MOTOR_SN_OEM_LO, "REG_MOTOR_SN_OEM_LO", F_RAW},
	{MOTOR, REG_MOTOR_STATISTIC_ODOMETER_HI, "REG_MOTOR_STATISTIC_ODOMETER_HI", F_KM_D1},
	{MOTOR, REG_MOTOR_STATISTIC_ODOMETER_LO, "REG_MOTOR_STATISTIC_ODOMETER_LO", F_RAW},
	{MOTOR, REG_MOTOR_STATUS_POWER_VOLTAGE_HI, "REG_MOTOR_STATUS_POWER_VOLTAGE_HI", F_VOLT_D3},
	{MOTOR, REG_MOTOR_STATUS_POWER_VOLTAGE_LO, "REG_MOTOR_STATUS_POWER_VOLTAGE_LO", F_RAW},
	{MOTOR, REG_MOTOR_STATUS_12V_VOLTAGE_HI, "REG_MOTOR_STATUS_12V_VOLTAGE_HI", F_VOLT_D3},
	{MOTOR, REG_MOTOR_STATUS_12V_VOLTAGE_LO, "REG_MOTOR_STATUS_12V_VOLTAGE_LO", F_RAW},
	{MOTOR, REG_MOTOR_STATUS_5V_VOLTAGE_HI, "REG_MOTOR_STATUS_5V_VOLTAGE_HI", F_VOLT_D3},
	{MOTOR, REG_MOTOR_STATUS_5V_VOLTAGE_LO, "REG_MOTOR_STATUS_5V_VOLTAGE_LO", F_RAW},
	{MOTOR, REG_MOTOR_ASSIST_LEVEL, "REG_MOTOR_ASSIST_LEVEL", F_PCT_ASSIST},
	{MOTOR, REG_MOTOR_ASSIST_WALK_LEVEL, "REG_MOTOR_ASSIST_WALK_LEVEL", F_PCT_ASSIST},
	{MOTOR, REG_MOTOR_STATUS_SPEED, "REG_MOTOR_STATUS_SPEED", F_RAW},
	{MOTOR, REG_MOTOR_STATUS_DISTANCE, "REG_MOTOR_STATUS_DISTANCE", F_RAW},
	{MOTOR, REG_MOTOR_STATUS_POWER_METER, "REG_MOTOR_STATUS_POWER_METER", F_PCT_ASSIST},
	{MOTOR, REG_MOTOR_STATUS_CODES, "REG_MOTOR_STATUS_CODES", F_RAW},
	{MOTOR, REG_MOTOR_CONFIG_MAX_DISCHARGE_HI, "REG_MOTOR_CONFIG_MAX_DISCHARGE_HI", F_UINT16_BE},
	{MOTOR, REG_MOTOR_CONFIG_MAX_DISCHARGE_LO, "REG_MOTOR_CONFIG_MAX_DISCHARGE_LO", F_RAW},
	{MOTOR, REG_MOTOR_CONFIG_MAX_CHARGE_HI, "REG_MOTOR_CONFIG_MAX_CHARGE_HI", F_UINT16_BE},
	{MOTOR, REG_MOTOR_CONFIG_MAX_CHARGE_LO, "REG_MOTOR_CONFIG_MAX_CHARGE_LO", F_RAW},
	{MOTOR, REG_MOTOR_ASSIST_LOWSPEED_RAMP_FLAG, "REG_MOTOR_ASSIST_LOWSPEED_RAMP_FLAG", F_BOOL},
	{MOTOR, REG_MOTOR_ASSIST_DIRECTION, "REG_MOTOR_ASSIST_DIRECTION", F_RAW},
	{MOTOR, REG_MOTOR_TORQUE_GAUGE_VALUE, "REG_MOTOR_TORQUE_GAUGE_VALUE", F_PCT_ASSIST},
	{MOTOR, REG_MOTOR_TORQUE_GAUGE_TYPE, "REG_MOTOR_TORQUE_GAUGE_TYPE", F_RAW},
	{MOTOR, REG_MOTOR_PROTECT_UNLOCK, "REG_MOTOR_PROTECT_UNLOCK", F_RAW},

	{0, 0, NULL, 0}
};

struct system_setting {
	const char *section;
	const char *label;
	unsigned char node;
	unsigned char reg;
	int bytes;
	enum val_format format;
	int hidden;
	int min_sw;
};

static struct system_setting system_overview_table[] = {
	{"Console information:", "hardware version ........:", CONSOLE, REG_CONSOLE_REV_HW, 1, F_VERSION, 0, 0},
	{NULL,                    "software version ........:", CONSOLE, REG_CONSOLE_REV_SW, 1, F_VERSION, 0, 0},
	{NULL,                    "manufacture date ........:", CONSOLE, REG_CONSOLE_SN_YEAR, 3, F_DATE, 0, 0},
	{NULL,                    "assistance level ........:", CONSOLE, REG_CONSOLE_ASSIST_INITLEVEL, 1, F_RAW, 0, 0},
	{NULL,                    "part number .............:", CONSOLE, REG_CONSOLE_SN_PN_HI, 2, F_SN, 1, 0},
	{NULL,                    "item number .............:", CONSOLE, REG_CONSOLE_SN_ITEM_HI, 2, F_SN, 1, 0},
	{NULL,                    "max limit enabled .......:", CONSOLE, REG_CONSOLE_ASSIST_MAXSPEED_FLAG, 1, F_BOOL, 0, 0},
	{NULL,                    "speed limit .............:", CONSOLE, REG_CONSOLE_ASSIST_MAXSPEED_HI, 2, F_KPH_D1, 0, 0},
	{NULL,                    "min limit enabled .......:", CONSOLE, REG_CONSOLE_ASSIST_MINSPEED_FLAG, 1, F_BOOL, 0, 0},
	{NULL,                    "min speed limit .........:", CONSOLE, REG_CONSOLE_ASSIST_MINSPEED, 1, F_KPH_D1, 0, 0},
	{NULL,                    "throttle limit enabled ..:", CONSOLE, REG_CONSOLE_THROTTLE_MAXSPEED_FLAG, 1, F_BOOL, 0, 0},
	{NULL,                    "throttle speed limit ....:", CONSOLE, REG_CONSOLE_THROTTLE_MAXSPEED_HI, 2, F_KPH_D1, 0, 0},
	{NULL,                    "wheel circumference .....:", CONSOLE, REG_CONSOLE_GEOMETRY_CIRC_HI, 2, F_UINT16_BE, 0, 0},
	{NULL,                    "mountain cap ............:", CONSOLE, REG_CONSOLE_ASSIST_MOUNTAIN_CAP, 1, F_PCT_ASSIST, 0, 59},
	{NULL,                    "odo .....................:", CONSOLE, REG_CONSOLE_STATISTIC_ODOMETER_HIHI, 4, F_KM_D1, 0, 0},

	{"Battery information:", "hardware version ........:", BATTERY, REG_BATTERY_REV_HW, 1, F_VERSION, 0, 0},
	{NULL,                   "software version ........:", BATTERY, REG_BATTERY_REV_SW, 1, F_VERSION, 0, 0},
	{NULL,                   "manufacture date ........:", BATTERY, REG_BATTERY_SN_YEAR, 3, F_DATE, 0, 0},
	{NULL,                   "part number .............:", BATTERY, REG_BATTERY_SN_PN_HI, 2, F_SN, 1, 0},
	{NULL,                   "item number .............:", BATTERY, REG_BATTERY_SN_ITEM_HI, 2, F_SN, 1, 0},
	{NULL,                   "voltage .................:", BATTERY, REG_BATTERY_STATUS_BATTERY_VOLTAGE_HI, 2, F_VOLT_D3, 0, 0},
	{NULL,                   "battery level ...........:", BATTERY, REG_BATTERY_STATUS_CHARGE_LEVEL, 1, F_PCT_BATT, 0, 0},
	{NULL,                   "maximum voltage .........:", BATTERY, REG_BATTERY_STATISTIC_BATTERY_MAX_VOLTAGE, 1, F_VOLT_NORM, 0, 0},
	{NULL,                   "minimum voltage .........:", BATTERY, REG_BATTERY_STATISTIC_BATTERY_MIN_VOLTAGE, 1, F_VOLT_NORM, 0, 0},
	{NULL,                   "mean voltage ............:", BATTERY, REG_BATTERY_STATISTIC_BATTERY_AVGVOLTAGE_NORMALIZED, 1, F_VOLT_NORM, 0, 0},
	{NULL,                   "resets ..................:", BATTERY, REG_BATTERY_STATISTIC_RESETS_HI, 2, F_UINT16_BE, 0, 0},
	{NULL,                   "lmd .....................:", BATTERY, REG_BATTERY_STATISTIC_LMD_HI, 2, F_AH_LMD, 0, 0},
	{NULL,                   "cell capacity ...........:", BATTERY, REG_BATTERY_CONFIG_CELLCAPACITY_HI, 2, F_AH_D3, 0, 0},
	{NULL,                   "charge time worst .......:", BATTERY, REG_BATTERY_STATISTIC_CHARGETIME_WORST_HI, 2, F_UINT16_BE, 0, 0},
	{NULL,                   "charge time mean ........:", BATTERY, REG_BATTERY_STATISTIC_CHARGETIME_MEAN_HI, 2, F_UINT16_BE, 0, 0},
	{NULL,                   "charge cycles ...........:", BATTERY, REG_BATTERY_STATISTIC_BATTERY_CYCLES_HI, 2, F_UINT16_BE, 0, 0},
	{NULL,                   "full charge cycles ......:", BATTERY, REG_BATTERY_STATISTIC_BATTERY_FULL_CYCLES_HI, 2, F_UINT16_BE, 0, 0},
	{NULL,                   "power cycles ............:", BATTERY, REG_BATTERY_STATISTIC_POWER_CYCLES_HI, 2, F_UINT16_BE, 0, 0},
	{NULL,                   "battery temp max ........:", BATTERY, REG_BATTERY_STATISTIC_TEMPERATURE_MAX, 1, F_TEMP_C_S, 0, 0},
	{NULL,                   "battery temp min ........:", BATTERY, REG_BATTERY_STATISTIC_TEMPERATURE_MIN, 1, F_TEMP_C_S, 0, 0},
	{NULL,                   "accessory voltage config.:", BATTERY, REG_BATTERY_CONFIG_ACCESSORY_VOLTAGE, 1, F_VOLT_D1, 0, 0},
	{NULL,                   "RTC timestamp............:", BATTERY, REG_BATTERY_RTC_TIME_HIHI, 4, F_RTC, 0, 0},

	{"Motor information:", "hardware version ........:", MOTOR, REG_MOTOR_REV_HW, 1, F_VERSION, 0, 0},
	{NULL,                 "software version ........:", MOTOR, REG_MOTOR_REV_SW, 1, F_VERSION, 0, 0},
	{NULL,                 "manufacture date ........:", MOTOR, REG_MOTOR_SN_YEAR, 3, F_DATE, 0, 0},
	{NULL,                 "temperature .............:", MOTOR, REG_MOTOR_STATUS_TEMPERATURE, 1, F_TEMP_C, 0, 0},
	{NULL,                 "speed limit .............:", MOTOR, REG_MOTOR_ASSIST_MAXSPEED, 1, F_KPH, 0, 0},
	{NULL,                 "wheel circumference .....:", MOTOR, REG_MOTOR_GEOMETRY_CIRC_HI, 2, F_UINT16_BE, 0, 0},
	{NULL,                 "part number .............:", MOTOR, REG_MOTOR_SN_PN_HI, 2, F_SN, 1, 0},
	{NULL,                 "item number .............:", MOTOR, REG_MOTOR_SN_ITEM_HI, 2, F_SN, 1, 0},

	{NULL, NULL, 0, 0, 0, 0, 0, 0}
};

const struct reg_metadata* getRegMetadata(unsigned char node, unsigned char reg)
{
	unsigned char baseNode = getBaseNodeId(node);
	for (int i = 0; bionx_registers[i].name != NULL; i++) {
		if (bionx_registers[i].node == baseNode && bionx_registers[i].reg == reg)
			return &bionx_registers[i];
	}
	return NULL;
}

const char* getRegisterName(unsigned char node, unsigned char reg)
{
	const struct reg_metadata *meta = getRegMetadata(node, reg);
	return meta ? meta->name : "UNKNOWN";
}

void formatValue(char *buf, size_t len, const struct reg_metadata *meta, enum val_format format, unsigned int raw_val)
{
	enum val_format f = (format != F_RAW) ? format : (meta ? meta->format : F_RAW);

	switch (f) {
		case F_RAW: snprintf(buf, len, "%u (0x%02X)", raw_val, raw_val); break;
		case F_BOOL: snprintf(buf, len, "%s", raw_val ? "yes" : "no"); break;
		case F_VERSION: snprintf(buf, len, "0x%02u", raw_val); break;
		case F_KPH: snprintf(buf, len, "%u km/h", raw_val); break;
		case F_KPH_D1: snprintf(buf, len, "%0.1f km/h", raw_val * 0.1); break;
		case F_VOLT_D1: snprintf(buf, len, "%0.2f V", raw_val * 0.1); break;
		case F_VOLT_D3: snprintf(buf, len, "%0.2f V", raw_val * 0.001); break;
		case F_VOLT_NORM: snprintf(buf, len, "%0.2f%%", raw_val * 0.416667 + 20.8333); break;
		case F_PCT_ASSIST: snprintf(buf, len, "%0.2f%%", raw_val * 1.5625); break;
		case F_PCT_BATT: snprintf(buf, len, "%0.2f%%", raw_val * 6.6667); break;
		case F_TEMP_C: snprintf(buf, len, "%d" _DEGREE_SIGN "C", (int)raw_val); break;
		case F_TEMP_C_S: snprintf(buf, len, "%hhd" _DEGREE_SIGN "C", (int)raw_val); break;
		case F_AH_D3: snprintf(buf, len, "%0.2f Ah", raw_val * 0.001); break;
		case F_AH_LMD: snprintf(buf, len, "%0.2f Ah", raw_val * 0.002142); break;
		case F_KM_D1: snprintf(buf, len, "%0.2f km", raw_val * 0.1); break;
		case F_SN: snprintf(buf, len, "%05u", raw_val); break;
		case F_DATE: {
			unsigned int y = (raw_val >> 16) & 0xFF;
			unsigned int m = (raw_val >> 8) & 0xFF;
			unsigned int d = raw_val & 0xFF;
			snprintf(buf, len, "%02u/%02u/20%02u", d, m, y);
			break;
		}
		case F_RTC: {
			unsigned int day = (raw_val / 86400);
			raw_val %= 86400;
			unsigned int hour = (raw_val / 3600);
			raw_val %= 3600;
			unsigned int min = (raw_val / 60);
			raw_val %= 60;
			snprintf(buf, len, "%dd %02u:%02u:%02u", day, hour, min, raw_val);
			break;
		}
		case F_UINT16_BE: snprintf(buf, len, "%u", raw_val); break;
		case F_UINT32_BE: snprintf(buf, len, "%u", raw_val); break;
		default: snprintf(buf, len, "%u (0x%02X)", raw_val, raw_val); break;
	}
}



void decodeFrame(struct can_frame *frame, struct timeval *tv, struct decoded_frame *df)
{
	struct tm *tm_info;

	memset(df, 0, sizeof(struct decoded_frame));
	df->can_id = frame->can_id;
	df->dlc = frame->can_dlc;
	memcpy(df->data, frame->data, 8);

	tm_info = localtime(&tv->tv_sec);
	strftime(df->timestamp, sizeof(df->timestamp), "%H:%M:%S", tm_info);
	snprintf(df->timestamp + strlen(df->timestamp), sizeof(df->timestamp) - strlen(df->timestamp), ".%03ld", tv->tv_usec / 1000);

	snprintf(df->src, 16, "unknown");
	snprintf(df->dst, 16, "unknown");
	df->type = "DATA";
	df->regName = "UNKNOWN";

	if (frame->can_dlc == 2) {
		/* Query: can_id is the target device. */
		df->type = "QUERY";
		df->targetNode = frame->can_id;
		snprintf(df->dst, 16, "%s", getNodeName(df->targetNode));
		/* Per protocol, the sender is usually the "master" (Console or BIB) */
		snprintf(df->src, 16, "master");
		df->reg = frame->data[1];
		df->hasReg = 1;
		
		gLastRequestTo[df->reg] = df->targetNode;
		df->regName = getRegisterName(df->targetNode, df->reg);

	} else if (frame->can_dlc == 4) {
		df->reg = frame->data[1];
		df->val = frame->data[3];
		df->hasReg = 1;
		df->hasVal = 1;

		/* Per PROTOCOL.md: 
		   - Reply: can_id is the requester (0x08 for Console Master, 0x58 for BIB).
		   - Set: can_id is the target device (0x10/0x50 battery, 0x20/0x60 motor, 0x48 console slave).
		*/
		if (frame->can_id == 0x08 || frame->can_id == 0x58) {
			df->type = "REPLY";
			snprintf(df->dst, 16, "%s", getNodeName(frame->can_id));
			df->targetNode = gLastRequestTo[df->reg];
			if (df->targetNode) {
				snprintf(df->src, 16, "%s", getNodeName(df->targetNode));
			} else {
				/* Heuristic: find which node has this register. */
				if (getRegMetadata(BATTERY, df->reg)) { snprintf(df->src, 16, "battery"); df->targetNode = BATTERY; }
				else if (getRegMetadata(MOTOR, df->reg)) { snprintf(df->src, 16, "motor"); df->targetNode = MOTOR; }
				else if (getRegMetadata(CONSOLE, df->reg)) { snprintf(df->src, 16, "console"); df->targetNode = CONSOLE; }
			}
			df->regName = getRegisterName(df->targetNode, df->reg);
		} else {
			df->type = "SET";
			df->targetNode = frame->can_id;
			snprintf(df->dst, 16, "%s", getNodeName(df->targetNode));
			snprintf(df->src, 16, "master");
			df->regName = getRegisterName(df->targetNode, df->reg);
		}
	}

	if (df->hasReg) {
		df->meta = getRegMetadata(df->targetNode, df->reg);
		if (df->hasVal) {
			unsigned int displayVal = df->val;
			if (df->meta && df->meta->name) {
				size_t nlen = strlen(df->meta->name);
				if (nlen > 5 && strcmp(df->meta->name + nlen - 5, "_HIHI") == 0) displayVal <<= 24;
				else if (nlen > 5 && strcmp(df->meta->name + nlen - 5, "_HILO") == 0) displayVal <<= 16;
				else if (nlen > 5 && strcmp(df->meta->name + nlen - 5, "_LOHI") == 0) displayVal <<= 8;
				else if (nlen > 3 && strcmp(df->meta->name + nlen - 3, "_HI") == 0) displayVal <<= 8;
			}
			formatValue(df->val_str, sizeof(df->val_str), df->meta, F_RAW, displayVal);
		}
	}
}

void processFrame(struct can_frame *frame, struct timeval *tv)
{
	struct decoded_frame df;
	decodeFrame(frame, tv, &df);

	if (gSniffOnlyChanges) {
		if (df.dlc != 4 || !df.hasReg || !df.hasVal) {
			return;
		}
		unsigned char node = df.targetNode;
		unsigned char reg = df.reg;
		unsigned char val = df.val;
		int seen = (gSniffSeenMap[node][reg / 8] >> (reg % 8)) & 1;
		if (seen && gSniffValueMap[node][reg] == val) {
			return;
		}
		gSniffValueMap[node][reg] = val;
		gSniffSeenMap[node][reg / 8] |= (1 << (reg % 8));
	}

	printf("[%s] [0x%03X] %7s -> %-7s: %-5s ", df.timestamp, df.can_id, df.src, df.dst, df.type);
	
	if (df.hasReg) {
		if (df.hasVal) {
			if (df.meta && df.meta->format != F_RAW) {
				printf("%s (0x%02X) = %s (0x%02X)" _NL, df.regName, df.reg, df.val_str, df.val);
			} else {
				printf("%s (0x%02X) = %s" _NL, df.regName, df.reg, df.val_str);
			}
		} else {
			printf("%s (0x%02X)" _NL, df.regName, df.reg);
		}
	} else {
		printf("DLC=%d DATA:", df.dlc);
		for (int i = 0; i < df.dlc; i++) printf(" %02X", df.data[i]);
		printf(_NL);
	}
}

void updateMonitor(struct decoded_frame *df, struct timeval *tv)
{
	if (!df->hasReg) return;

	unsigned char node = df->targetNode;
	unsigned char reg = df->reg;

	if (gMonitorMap[node][reg] == -1) {
		if (gTotalMonitored >= MAX_MONITORED) return;
		int idx = gTotalMonitored++;
		gMonitorMap[node][reg] = idx;
		gMonitoredRegs[idx].node = node;
		gMonitoredRegs[idx].reg = reg;
		gMonitoredRegs[idx].first_seen_order = gFirstSeenCounter++;
		gMonitoredRegs[idx].reg_name = df->regName;
	}

	int idx = gMonitorMap[node][reg];
	gMonitoredRegs[idx].last_val = df->val;
	gMonitoredRegs[idx].last_seen = *tv;
	snprintf(gMonitoredRegs[idx].val_str, sizeof(gMonitoredRegs[idx].val_str), "%s", df->val_str);
}

int compareMonitored(const void *a, const void *b)
{
	const struct monitored_reg *ma = (const struct monitored_reg *)a;
	const struct monitored_reg *mb = (const struct monitored_reg *)b;
	if (gSortLatestFirst) {
		if (ma->last_seen.tv_sec != mb->last_seen.tv_sec)
			return (int)(mb->last_seen.tv_sec - ma->last_seen.tv_sec);
		return (int)(mb->last_seen.tv_usec - ma->last_seen.tv_usec);
	} else {
		return (int)(ma->first_seen_order - mb->first_seen_order);
	}
}

void renderMonitor()
{
	erase();
	attron(A_REVERSE);
	mvprintw(0, 0, "BionXtool Monitor - Device: %s | Total Regs: %d | Sort: %s", 
	         gCanInterface, gTotalMonitored, gSortLatestFirst ? "Latest Seen" : "Earliest Seen");
	mvprintw(1, 0, "Press 't' to toggle sort, 'q' to quit");
	attroff(A_REVERSE);

	struct monitored_reg sorted[MAX_MONITORED];
	memcpy(sorted, gMonitoredRegs, gTotalMonitored * sizeof(struct monitored_reg));
	qsort(sorted, gTotalMonitored, sizeof(struct monitored_reg), compareMonitored);

	int max_y, max_x;
	getmaxyx(stdscr, max_y, max_x);
	(void)max_x; // Silence unused variable warning

	for (int i = 0; i < gTotalMonitored && i < max_y - 3; i++) {
		struct tm *tm_info = localtime(&sorted[i].last_seen.tv_sec);
		char ts[32];
		strftime(ts, sizeof(ts), "%H:%M:%S", tm_info);
    time_t past_time = mktime(tm_info);
    time_t current_time = time(NULL);
    double age = difftime(current_time, past_time);
    if (age < 1) {
      attron(COLOR_PAIR(1));
    } else if (age < 5) {
      attron(COLOR_PAIR(2));
    }

		mvprintw(i + 3, 0, "[%s.%03ld] %-7s: %-35s (0x%02X) = %s", 
		         ts, sorted[i].last_seen.tv_usec / 1000,
		         getNodeName(sorted[i].node),
		         sorted[i].reg_name,
		         sorted[i].reg,
		         sorted[i].val_str);

    if (age < 1) {
      attroff(COLOR_PAIR(1));
    } else if (age < 5) {
      attroff(COLOR_PAIR(2));
    }

	}
	refresh();
}

void monitorBus()
{
	struct can_frame frame;
	int nbytes;
	struct timeval tv;

	initscr();
  if (has_colors()) {
    start_color();
  }
  init_pair(1, COLOR_GREEN, COLOR_BLACK);
  init_pair(2, COLOR_YELLOW, COLOR_BLACK);
	raw();
	keypad(stdscr, TRUE);
	noecho();
	curs_set(0);
	timeout(0); // non-blocking getch

	for (int i = 0; i < 256; i++) for (int j = 0; j < 256; j++) gMonitorMap[i][j] = -1;

	while (1) {
		int ch = getch();
		if (ch == 'q') break;
		if (ch == 't') gSortLatestFirst = !gSortLatestFirst;

		struct pollfd pfd[1];
		pfd[0].fd = can_sock;
		pfd[0].events = POLLIN;

		int ret = poll(pfd, 1, 100);
		if (ret > 0 && (pfd[0].revents & POLLIN)) {
			nbytes = read(can_sock, &frame, sizeof(struct can_frame));
			if (nbytes > 0) {
				gettimeofday(&tv, NULL);
				struct decoded_frame df;
				decodeFrame(&frame, &tv, &df);
				updateMonitor(&df, &tv);
			}
		}
		renderMonitor();
	}
	endwin();
}

void sniffBus()
{
	struct can_frame frame;
	int nbytes;
	struct timeval tv;

	printf("Starting CAN bus sniffer on %s..." _NL, gCanInterface);
	printf("Press Ctrl+C to stop." _NL _NL);

	while (1) {
		nbytes = read(can_sock, &frame, sizeof(struct can_frame));
		if (nbytes < 0) {
			if (errno == EINTR) continue;
			perror("can raw read");
			break;
		}
		gettimeofday(&tv, NULL);
		processFrame(&frame, &tv);
	}
}

typedef struct pcap_hdr_s {
        unsigned int magic_number;   /* magic number */
        unsigned short version_major;  /* major version number */
        unsigned short version_minor;  /* minor version number */
        int  thiszone;       /* GMT to local correction */
        unsigned int sigfigs;        /* accuracy of timestamps */
        unsigned int snaplen;        /* max length of captured packets, in octets */
        unsigned int network;        /* data link type */
} pcap_hdr_t;

typedef struct pcaprec_hdr_s {
        unsigned int ts_sec;         /* timestamp seconds */
        unsigned int ts_usec;        /* timestamp microseconds */
        unsigned int incl_len;       /* number of octets of packet saved in file */
        unsigned int orig_len;       /* actual length of packet */
} pcaprec_hdr_t;

void sniffPcap(const char *filename)
{
	FILE *fp = fopen(filename, "rb");
	if (!fp) {
		perror("fopen");
		return;
	}

	pcap_hdr_t head;
	if (fread(&head, 1, sizeof(head), fp) != sizeof(head)) {
		fprintf(stderr, "error: could not read pcap header" _NL);
		fclose(fp);
		return;
	}

	if (head.magic_number != 0xa1b2c3d4 && head.magic_number != 0xd4c3b2a1) {
		fprintf(stderr, "error: not a pcap file (magic 0x%08x)" _NL, head.magic_number);
		fclose(fp);
		return;
	}

	if (head.network != 227) { // LINKTYPE_CAN_SOCKETCAN
		fprintf(stderr, "error: not a SocketCAN pcap file (network %d, expected 227)" _NL, head.network);
		fclose(fp);
		return;
	}

	printf("Reading from pcap file: %s" _NL _NL, filename);

	pcaprec_hdr_t rec;
	while (fread(&rec, 1, sizeof(rec), fp) == sizeof(rec)) {
		unsigned char data[rec.incl_len];
		if (fread(data, 1, rec.incl_len, fp) != rec.incl_len) break;

		if (rec.incl_len < 8) continue;

		struct can_frame frame;
		struct timeval tv;

		/* SocketCAN in PCAP: 4 bytes CAN ID (Big Endian), 1 byte DLC, 3 bytes padding, up to 8 bytes data. */
		unsigned int id = (data[0] << 24) | (data[1] << 16) | (data[2] << 8) | data[3];
		frame.can_id = id & 0x1FFFFFFF;
		frame.can_dlc = data[4];
		memcpy(frame.data, &data[8], (frame.can_dlc > 8) ? 8 : frame.can_dlc);

		tv.tv_sec = rec.ts_sec;
		tv.tv_usec = rec.ts_usec;

		processFrame(&frame, &tv);
	}

	fclose(fp);
}

int can_open(const char *ifname)
{
	struct sockaddr_can addr;
	struct ifreq ifr;
	int s;

	if ((s = socket(PF_CAN, SOCK_RAW, CAN_RAW)) < 0) {
		perror("Error while opening socket");
		return -1;
	}

	strncpy(ifr.ifr_name, ifname, IFNAMSIZ - 1);
	if (ioctl(s, SIOCGIFINDEX, &ifr) < 0) {
		perror("ioctl (SIOCGIFINDEX)");
		close(s);
		return -1;
	}

	addr.can_family  = AF_CAN;
	addr.can_ifindex = ifr.ifr_ifindex;

	if (bind(s, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		perror("Error in socket bind");
		return -1;
	}

	return s;
}

void setValue(unsigned char receipient, unsigned char reg, unsigned char value)
{
	struct can_frame frame;
	int nbytes;

	frame.can_id = receipient;
	frame.can_dlc = 4;
	frame.data[0] = 0x00;
	frame.data[1] = reg;
	frame.data[2] = 0x00;
	frame.data[3] = value;

	nbytes = write(can_sock, &frame, sizeof(struct can_frame));

	if (nbytes < 0)
		perror("can raw write");
	
	if (nbytes < sizeof(struct can_frame)) {
		fprintf(stderr, "error: could not send value to %s" _NL, getNodeName(receipient));
	}
//	printf("set dst 0x%02x register 0x%02x to 0x%02x" _NL, receipient, reg, value);
}

unsigned int getValue(unsigned char receipient, unsigned char reg)
{
	struct can_frame frame;
	int nbytes, retry = 20;
	struct pollfd pfd;
	
	frame.can_id = receipient;
	frame.can_dlc = 2;
	frame.data[0] = 0x00;
	frame.data[1] = reg;

	nbytes = write(can_sock, &frame, sizeof(struct can_frame));
	if (nbytes < 0) {
		perror("can raw write");
		return 0;
	}

retry:
	pfd.fd = can_sock;
	pfd.events = POLLIN;
	
	if (poll(&pfd, 1, TIMEOUT_VALUE * TIMEOUT_MS) > 0) {
		nbytes = read(can_sock, &frame, sizeof(struct can_frame));
		if (nbytes < 0) {
			perror("can raw read");
			return 0;
		}

		if (--retry && (frame.can_id != BIB || frame.can_dlc != 4 || frame.data[1] != reg))
			goto retry;

		if (!retry) {
			printf("error: no response from node %s to %s" _NL, getNodeName(receipient), getNodeName(BIB));
			return 0;
		}
//		printf("src: 0x%02x reg: 0x%02x val: 0x%02x" _NL, receipient, reg, frame.data[3]);

		return (unsigned int) frame.data[3];
	} else {
		printf("error: no response from node %s" _NL, getNodeName(receipient));
	}

	return 0;
}

void setSpeedLimit(double speed)
{
	int limit = (speed != 0);

	if (!speed)
		speed = UNLIMITED_SPEED_VALUE;
	setValue(CONSOLE, CONSOLE_ASSIST_MAXSPEEDFLAG, limit);
	setValue(CONSOLE, CONSOLE_ASSIST_MAXSPEED_HI, ((int)(speed * 10)) >> 8);
	setValue(CONSOLE, CONSOLE_ASSIST_MAXSPEED_LO, ((int)(speed * 10)) & 0xff);
	setValue(MOTOR, MOTOR_PROTECT_UNLOCK, MOTOR_PROTECT_UNLOCK_KEY);
	setValue(MOTOR, MOTOR_ASSIST_MAXSPEED, (int)speed);
}

void setWheelCircumference(unsigned short circumference)
{
	if (!circumference)
		return;

	setValue(CONSOLE, CONSOLE_GEOMETRY_CIRC_HI, (int) (circumference >> 8));
	setValue(CONSOLE, CONSOLE_GEOMETRY_CIRC_LO, (int) (circumference & 0xff));
	setValue(MOTOR, MOTOR_PROTECT_UNLOCK, MOTOR_PROTECT_UNLOCK_KEY);
	setValue(MOTOR, MOTOR_GEOMETRY_CIRC_HI, (int) (circumference >> 8));
	setValue(MOTOR, MOTOR_GEOMETRY_CIRC_LO, (int) (circumference & 0xff));
}

void setMinSpeedLimit(double speed)
{
	char limit = (speed != 0);

	setValue(CONSOLE, CONSOLE_ASSIST_MINSPEEDFLAG, limit);
	setValue(CONSOLE, CONSOLE_ASSIST_MINSPEED, (int)(speed * 10));
}

void setThrottleSpeedLimit(double speed)
{
	int limit = (speed != 0);

	if (!speed)
		speed = MAX_THROTTLE_SPEED_VALUE;

	setValue(CONSOLE, CONSOLE_THROTTLE_MAXSPEEDFLAG, limit);
	setValue(CONSOLE, CONSOLE_THROTTLE_MAXSPEED_HI, ((int)(speed * 10)) >> 8);
	setValue(CONSOLE, CONSOLE_THROTTLE_MAXSPEED_LO, ((int)(speed * 10)) & 0xff);
}

void setAccessoryVoltage(double voltage)
{
	int hwVersion = getValue(BATTERY, BATTERY_REF_HW);
	int value;

	if (hwVersion == 0) {
		printf("error: battery not responding, cannot set accessory voltage" _NL);
		return;
	}

	if (hwVersion >= 60) {
		value = (int)(voltage * 10);
	} else {
		value = (int)(voltage / 6);
	}

	setValue(BATTERY, REG_BATTERY_PROTECT_UNLOCK, 0x10);
	doSleep(500);
	setValue(BATTERY, BATTERY_CONFIG_ACCESSORY_VOLTAGE, value);
}

void setBatteryRTC()
{
    time_t raw_time;
    struct tm *tm_info;
    time(&raw_time);
    tm_info = localtime(&raw_time);

    unsigned int rtcValue = (getValue(BATTERY, REG_BATTERY_RTC_TIME_HIHI) << 24) +
                            (getValue(BATTERY, REG_BATTERY_RTC_TIME_HILO) << 16) +
                            (getValue(BATTERY, REG_BATTERY_RTC_TIME_LOHI) << 8) +
                            (getValue(BATTERY, REG_BATTERY_RTC_TIME_LOLO));
    char rtcStr[32];
    formatValue(rtcStr, sizeof(rtcStr), NULL, F_RTC, rtcValue);
    printf(" RTC Value was: %s (0x%x)" _NL, rtcStr, rtcValue);

    unsigned int days = rtcValue / 86400; // Convert to integer.
    rtcValue = days*86400 + tm_info->tm_hour*3600 + tm_info->tm_min*60 + tm_info->tm_sec;

    setValue(BATTERY, REG_BATTERY_PROTECT_UNLOCK, 0x10);
    doSleep(50);
    setValue(BATTERY, REG_BATTERY_RTC_TIME_HIHI, (rtcValue >>24)&0xFF);
    doSleep(50);
    setValue(BATTERY, REG_BATTERY_RTC_TIME_HILO, (rtcValue >>16)&0xFF);
    doSleep(50);
    setValue(BATTERY, REG_BATTERY_RTC_TIME_LOHI, (rtcValue >>8)&0xFF);
    doSleep(50);
    setValue(BATTERY, REG_BATTERY_RTC_TIME_LOLO, rtcValue&0xFF);
    doSleep(50);
    printf(" Set RTC to current time (%02d:%02d:%02d) (%08x)" _NL, tm_info->tm_hour, tm_info->tm_min, tm_info->tm_sec, rtcValue);
}

void printBatteryStats()
{
	int channel = 1, packSerial, packParallel;
	
	printf( " balancer enabled ...: %s" _NL _NL, (getValue(BATTERY, BATTERY_CELLMON_BALANCERENABLED) != 0) ? "yes" : "no");

	packSerial = getValue(BATTERY, BATTERY_CONFIG_PACKSERIAL);
	packParallel = getValue(BATTERY, BATTERY_CONFIG_PACKPARALLEL);
	
	packSerial = (packSerial > 20) ? 0 : packSerial;
	packParallel = (packParallel > 20) ? 0 : packParallel;

	for (;channel <= packSerial; channel++) {
		setValue(BATTERY, BATTERY_CELLMON_CHANNELADDR, (int)0x80 + channel);
		printf(" voltage cell #%02d ...: %.3fV" _NL, channel,
			((getValue(BATTERY, BATTERY_CELLMON_CHANNELDATA_HI) << 8) + getValue(BATTERY,BATTERY_CELLMON_CHANNELDATA_LO)) * 0.001);
	}

	for (channel = 0 ; channel < packParallel ; channel ++)
		printf(" temperature pack #%02d: %hhd" _DEGREE_SIGN "C" _NL, channel + 1,
				getValue(BATTERY, BATTERY_STATUS_PACKTEMPERATURE1 + channel));
	
	printf(_NL);
}

void printChargeStats() {
	int channel = 1, totalChagres = 0, c;

	for (channel = 1 ; channel <= 10; channel++) {
		setValue(BATTERY, 0xf6, channel);
		c = (getValue(BATTERY, 0xf7) << 8) + getValue(BATTERY,0xf8);
		totalChagres += c;
		printf(" charge level @ %03d%% : %04d" _NL, channel*10, c);
	}

	printf(" total # of charges .: %04d" _NL _NL, totalChagres);
}

double getVoltageValue(unsigned char in, unsigned char reg)
{
	return (getValue(BATTERY, reg) + 20.8333) * 0.416667;
}

void usage(void) {
	printf( "usage: BionXtool [-d <device>] ..." _NL
			" -d <device> .............. CAN device (default: can0)" _NL
			" -l <speedLimit> .......... set the speed limit to <speedLimit> (1 - " __STR(UNLIMITED_SPEED_VALUE) "), 0 = remove the limit" _NL
			" -m <minSpeedLimit> ....... set the minimum speed limit to <minSpeedLimit> (0 - " __STR(UNLIMITED_MIN_SPEED_VALUE) "), 0 = remove the limit" _NL
			" -t <throttleSpeedLimit> .. set the throttle speed limit to <throttleSpeedLimit> (0 - " __STR(MAX_THROTTLE_SPEED_VALUE) "), 0 = remove the limit" _NL
			" -a <assistLevel> ......... set the initial assist level after power on (0 - 4)" _NL
			" -v <voltage> ............. set the battery accessory voltage (e.g. 6.0 or 12.0)" _NL
			" -A <0|1> ................. set the battery accessory power (0=off, 1=on)" _NL
			" -o <level> ............... set the mountain cap level (0%% - 100%%), use 55%%" _NL

			" -c <wheel circumference> . set the wheel circumference (in mm)" _NL
			" -R <node|id> <reg> ....... read a specific register from a node (e.g. -R battery 0x3B)" _NL
			" -W <node|id> <reg> <val> . write a specific register value to a node (e.g. -W motor 0xA5 0xAA)" _NL
			" -T ....................... write current time to RTC" _NL
			" -s ....................... print system settings overview" _NL
			" -S ....................... sniff CAN bus" _NL
			" -C ....................... sniff CAN bus, only display changes" _NL
			" -M ....................... monitor CAN bus (real-time TUI)" _NL
			" -f <pcapfile> ............ sniff from a pcap file" _NL
			" -p ....................... power off system" _NL
			" -n ....................... don't try to put console in slave mode" _NL
			" -z ....................... put console in slave mode" _NL
			" -x ....................... skip automatic system shutdown when setting new speed limit. (should not be used)" _NL
			" -i ....................... don't display private serial and part numbers" _NL
			" -h ....................... print this help screen" _NL _NL);
}

int parseOptions(int argc, char **argv)
{
	int oc;
	char odef[] = "d:l:t:m:sa:pnxio:c:v:h?SCR:zW:A:f:MT";

	while((oc = getopt(argc,argv,odef)) != -1) {
		switch(oc) {
			case 'M':
				gMonitorMode = 1;
				break;
			case 'C':
				gSniffOnlyChanges = 1;
				gSniffMode = 1;
				break;
			case 'f':
				gPcapFile = optarg;
				break;
			case 'A':
				gSetAccessoryPower = atoi(optarg);
				if (gSetAccessoryPower != 0 && gSetAccessoryPower != 1) {
					printf("error: accessory power must be 0 or 1. exiting..." _NL);
					return -1;
				}
				break;
      case 'T':
        gSetRTC = 1;
        break;
			case 'W':
				gWriteSpecificReg = 1;
				gWriteNode = getNodeIdByName(optarg);
				if (optind + 1 < argc && argv[optind][0] != '-' && argv[optind+1][0] != '-') {
					gWriteReg = (unsigned char)strtol(argv[optind], NULL, 16);
					gWriteVal = (unsigned char)strtol(argv[optind+1], NULL, 0);
					optind += 2;
				} else {
					printf("error: -W requires <node>, <reg>, and <val> arguments" _NL);
					return -1;
				}
				break;
			case 'z':
				gForceSlaveMode = 1;
				break;
			case 'R':
				gReadSpecificReg = 1;
				gTargetNode = getNodeIdByName(optarg);
				if (optind < argc && argv[optind][0] != '-') {
					gTargetReg = (unsigned char)strtol(argv[optind], NULL, 16);
					optind++;
				} else {
					printf("error: -R requires both <node> and <reg> arguments" _NL);
					return -1;
				}
				break;
			case 'd':
				gCanInterface = optarg;
				break;
			case 'S':
				gSniffMode = 1;
				break;
			case 'p':
				gPowerOff = 1;
				break;
			case 'x':
				gSkipShutdown = 1;
				break;
			case 'v':
				gSetAccessoryVoltage = atof(optarg);
				if (gSetAccessoryVoltage > 14 || gSetAccessoryVoltage < 6) {
					printf("error: accessory voltage %.2f is out of range (6V - 14V). exiting..." _NL, gSetAccessoryVoltage);
					return -1;
				}
				break;
			case 'l':
				gSetSpeedLimit = atof(optarg);
				if (gSetSpeedLimit > UNLIMITED_SPEED_VALUE || gSetSpeedLimit < 0) {
					printf("error: speed limit %.2f is out of range. exiting..." _NL, gSetSpeedLimit);
					return -1;
				}
				break;
			case 't':
				gSetThrottleSpeedLimit = atof(optarg);
				if (gSetThrottleSpeedLimit > MAX_THROTTLE_SPEED_VALUE || gSetThrottleSpeedLimit < 0) {
					printf("error: throttle speed limit %.2f is out of range. exiting..." _NL, gSetThrottleSpeedLimit);
					return -1;
				}
				break;
			case 'm':
				gSetMinSpeedLimit = atof(optarg);
				if (gSetMinSpeedLimit > UNLIMITED_MIN_SPEED_VALUE || gSetMinSpeedLimit < 0) {
					printf("error: min speed limit %.2f is out of range. exiting..." _NL, gSetMinSpeedLimit);
					return -1;
				}
				break;
			case 'a':
				gAssistInitLevel = atoi(optarg);
				if (gAssistInitLevel > 4 || gAssistInitLevel < 0) {
					printf("error: initial assist level %d is out of range. exiting..." _NL, gAssistInitLevel);
					return -1;
				}
				break;
			case 'o':
				gSetMountainCap = atoi(optarg);
				if (gSetMountainCap > 100 || gSetMountainCap < 0) {
					printf("error: mountain cap level %d is out of range. exiting..." _NL, gSetMountainCap);
					return -1;
				}
				break;
			case 'c':
				gSetWheelCircumference = atoi(optarg);
				if (gSetWheelCircumference > 3000 || gSetWheelCircumference < 1000) {
					printf("error: wheel circumference %d is out of range. exiting..." _NL, gSetWheelCircumference);
					return -1;
				}
				break;
			case 'n':
				gConsoleSetSlaveMode = 0;
				break;
			case 'i':
				gNoSerialNumbers = 1;
				break;
			case 's':
				gPrintSystemSettings = 1;
				break;
			case 'h':
			case '?':
			default:
				usage();
				return -1;
		}
	}

	return 0;
}

void printSystemSettings()
{
	int i;
	unsigned int swVersion = 0;
	unsigned int hwVersion = 0;
	unsigned char currentNode = 0;

	printf(_NL _NL);

	for (i = 0; system_overview_table[i].label != NULL; i++) {
		struct system_setting *s = &system_overview_table[i];

		if (s->section) {
			currentNode = s->node;
			hwVersion = getValue(currentNode, (currentNode == CONSOLE) ? REG_CONSOLE_REV_HW : 
			                          (currentNode == BATTERY) ? REG_BATTERY_REV_HW : 
			                          REG_MOTOR_REV_HW);
			if (hwVersion == 0) {
				printf("%s" _NL, s->section);
				printf(" %s not responding" _NL _NL, getNodeName(currentNode));
				/* Skip this section */
				while (system_overview_table[i+1].label != NULL && system_overview_table[i+1].section == NULL) {
					i++;
				}
				continue;
			}
			printf("%s" _NL, s->section);
			swVersion = getValue(currentNode, (currentNode == CONSOLE) ? REG_CONSOLE_REV_SW : 
			                                (currentNode == BATTERY) ? REG_BATTERY_REV_SW : 
			                                REG_MOTOR_REV_SW);
		}

		if (s->hidden && gNoSerialNumbers) continue;
		if (s->min_sw > 0 && swVersion < s->min_sw) continue;

		unsigned int raw_val = 0;
		if (s->bytes == 1) {
			raw_val = getValue(s->node, s->reg);
		} else if (s->bytes == 2) {
			raw_val = (getValue(s->node, s->reg) << 8) | getValue(s->node, s->reg + 1);
		} else if (s->bytes == 3) {
			raw_val = (getValue(s->node, s->reg) << 16) | (getValue(s->node, s->reg + 1) << 8) | getValue(s->node, s->reg + 2);
		} else if (s->bytes == 4) {
			raw_val = (getValue(s->node, s->reg) << 24) | (getValue(s->node, s->reg + 1) << 16) |
			          (getValue(s->node, s->reg + 2) << 8) | getValue(s->node, s->reg + 3);
		}

		char valBuf[128];
		const struct reg_metadata *meta = getRegMetadata(s->node, s->reg);
		formatValue(valBuf, sizeof(valBuf), meta, s->format, raw_val);
		
		printf(" %-25s %s" _NL, s->label, valBuf);

		if (system_overview_table[i+1].section != NULL || system_overview_table[i+1].label == NULL) {
			printf(_NL);
			/* Extra logic for special sections */
			if (currentNode == BATTERY) {
				printChargeStats();
				if (hwVersion >= 60)
					printBatteryStats();
				else
					printf(" no battery details supported by battery hardware #%d" _NL _NL, hwVersion);
			}
		}
	}
}

int enterSlaveMode(void)
{
	int consoleInSlaveMode = getValue(CONSOLE, CONSOLE_STATUS_SLAVE);
	if (consoleInSlaveMode) {
		printf("console already in slave mode. good!" _NL _NL);
		return 1;
	}

	int retry = 20;

	printf("putting console in slave mode ... ");
	do {
		setValue(CONSOLE, CONSOLE_STATUS_SLAVE, 1);
		consoleInSlaveMode = getValue(CONSOLE, CONSOLE_STATUS_SLAVE);
		usleep(200000);
	} while(retry-- && !consoleInSlaveMode);

	doSleep(750); // give the console some time to settle
	printf("%s" _NL _NL, consoleInSlaveMode ? "done" : "failed");
	return consoleInSlaveMode;
}

int main(int argc, char **argv)
{
	int err, doShutdown = 0;

	printf("BionXtool " __BXF_VERSION__ _NL " (c) 2026 by Ben Buxton <bbuxton@gmail.com>"_NL _NL);

	if ((err=parseOptions(argc, argv) < 0))
		exit(1);

	if (gPcapFile) {
		sniffPcap(gPcapFile);
		return 0;
	}

	if ((can_sock = can_open(gCanInterface)) < 0) {
		exit(1);
	}

	int commandsRequested = (gReadSpecificReg || gWriteSpecificReg || gSniffMode || gMonitorMode || gPrintSystemSettings || gPowerOff ||
	                         gAssistInitLevel != -1 || gSetSpeedLimit != -1 || gSetMinSpeedLimit != -1 || gSetRTC ||
	                         gSetThrottleSpeedLimit != -1 || gSetMountainCap != -1 ||
	                         gSetWheelCircumference > 0 || gSetAccessoryVoltage > 0 || gSetAccessoryPower != -1);

	if (gForceSlaveMode || (!gReadSpecificReg && !gWriteSpecificReg && !gSniffMode && !gMonitorMode && gConsoleSetSlaveMode)) {
		enterSlaveMode();
	}

	if (gForceSlaveMode && !commandsRequested) {
		close(can_sock);
		return 0;
	}

	if (gWriteSpecificReg) {
		setValue(gWriteNode, gWriteReg, gWriteVal);
		const char *regName = getRegisterName(getBaseNodeId(gWriteNode), gWriteReg);
		printf("Writing to %s register %s (0x%02X) = %u (0x%02X)" _NL,
			getNodeName(gWriteNode), regName, gWriteReg, gWriteVal, gWriteVal);
		close(can_sock);
		return 0;
	}

	if (gReadSpecificReg) {
		unsigned int val = getValue(gTargetNode, gTargetReg);
		const char *regName = getRegisterName(getBaseNodeId(gTargetNode), gTargetReg);
		const struct reg_metadata *meta = getRegMetadata(gTargetNode, gTargetReg);
		char valBuf[128];
		formatValue(valBuf, sizeof(valBuf), meta, F_RAW, val);
		printf("Reading %s register %s (0x%02X) = %s" _NL,
			getNodeName(gTargetNode), regName, gTargetReg, valBuf);
		close(can_sock);
		return 0;
	}

	if (gSniffMode) {
		sniffBus();
		close(can_sock);
		return 0;
	}

	if (gMonitorMode) {
		monitorBus();
		close(can_sock);
		return 0;
	}
	if (gAssistInitLevel != -1) {
		printf("setting initial assistance level to %d" _NL, gAssistInitLevel);
		setValue(CONSOLE, CONSOLE_ASSIST_INITLEVEL, gAssistInitLevel);
	}

	if (gSetSpeedLimit > 0) {
		printf("set speed limit to %0.2f km/h" _NL, gSetSpeedLimit);
		setSpeedLimit(gSetSpeedLimit);
		doShutdown = 1;
	} else if (gSetSpeedLimit == 0) {
		printf("disable speed limit, drive carefully" _NL);
		setSpeedLimit(0);
		doShutdown = 1;
	}

	if (gSetMinSpeedLimit > 0) {
		printf("set minimal speed limit to %0.2f km/h" _NL, gSetMinSpeedLimit);
		setMinSpeedLimit(gSetMinSpeedLimit);
		doShutdown = 1;
	} else if (gSetMinSpeedLimit == 0) {
		printf("disable minimal speed limit, drive carefully" _NL);
		setMinSpeedLimit(0);
		doShutdown = 1;
	}

	if (gSetThrottleSpeedLimit > 0) {
		printf("set throttle speed limit to %0.2f km/h" _NL, gSetThrottleSpeedLimit);
		setThrottleSpeedLimit(gSetThrottleSpeedLimit);
		doShutdown = 1;
	} else if (gSetThrottleSpeedLimit == 0) {
		printf("disable throttle speed limit, drive carefully" _NL);
		setThrottleSpeedLimit(0);
		doShutdown = 1;
	}

	if (gSetMountainCap > 0) {
		printf("set mountain cap level to %0.2f%%" _NL, ((int)gSetMountainCap / 1.5625) * 1.5625);
		setValue(CONSOLE, CONSOLE_ASSIST_MOUNTAINCAP, gSetMountainCap / 1.5625);
	}

	if (gSetWheelCircumference > 0) {
		printf("set wheel circumference to %d" _NL, gSetWheelCircumference);
		setWheelCircumference(gSetWheelCircumference);
	}

	if (gSetAccessoryVoltage > 0) {
		printf("set battery accessory voltage to %0.1fV" _NL, gSetAccessoryVoltage);
		setAccessoryVoltage(gSetAccessoryVoltage);
	}

  if (gSetRTC > 0) {
    printf("setting RTC (stored in battery)" _NL);
    setBatteryRTC();
  }

	if (gSetAccessoryPower != -1) {
		printf("setting battery accessory power to %s" _NL, gSetAccessoryPower ? "ON" : "OFF");
		setValue(BATTERY, REG_BATTERY_CONFIG_ACCESSORY_ENABLED, gSetAccessoryPower);
	}

	if (gPrintSystemSettings)
		printSystemSettings();

	if ((doShutdown && !gSkipShutdown) || gPowerOff) {
		doSleep(1000);
		printf("shutting down system." _NL);
		setValue(BATTERY, BATTERY_CONFIG_SHUTDOWN, 1);
	}

	close(can_sock);

	return 0;
}
