/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Platform data for the NXP PCA9468 battery charger driver.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#ifndef PCA9468_REGS_H__
#define PCA9468_REGS_H__

#define BITS(_end, _start) ((BIT(_end) - BIT(_start)) + BIT(_end))
#define MASK2SHIFT(_mask)	__ffs(_mask)

/*
 * Register Map
 */
#define PCA9468_REG_DEVICE_INFO 	0x00	/* Device ID, revision */
#define PCA9468_BIT_DEV_REV		BITS(7, 4)
#define PCA9468_BIT_DEV_ID		BITS(3, 0)
#define PCA9468_DEVICE_ID		0x18	/* Default ID */

#define PCA9468_REG_INT1		0x01
#define PCA9468_BIT_V_OK_INT		BIT(7)
#define PCA9468_BIT_NTC_TEMP_INT	BIT(6)
#define PCA9468_BIT_CHG_PHASE_INT	BIT(5)
#define PCA9468_BIT_CTRL_LIMIT_INT	BIT(3)
#define PCA9468_BIT_TEMP_REG_INT	BIT(2)
#define PCA9468_BIT_ADC_DONE_INT	BIT(1)
#define PCA9468_BIT_TIMER_INT		BIT(0)

#define PCA9468_REG_INT1_MSK		0x02
#define PCA9468_BIT_V_OK_M		BIT(7)
#define PCA9468_BIT_NTC_TEMP_M		BIT(6)
#define PCA9468_BIT_CHG_PHASE_M		BIT(5)
#define PCA9468_BIT_RESERVED_M		BIT(4)
#define PCA9468_BIT_CTRL_LIMIT_M	BIT(3)
#define PCA9468_BIT_TEMP_REG_M		BIT(2)
#define PCA9468_BIT_ADC_DONE_M		BIT(1)
#define PCA9468_BIT_TIMER_M		BIT(0)

#define PCA9468_REG_INT1_STS		0x03
#define PCA9468_BIT_V_OK_STS		BIT(7)
#define PCA9468_BIT_NTC_TEMP_STS	BIT(6)
#define PCA9468_BIT_CHG_PHASE_STS	BIT(5)
#define PCA9468_BIT_CTRL_LIMIT_STS	BIT(3)
#define PCA9468_BIT_TEMP_REG_STS	BIT(2)
#define PCA9468_BIT_ADC_DONE_STS	BIT(1)
#define PCA9468_BIT_TIMER_STS		BIT(0)

#define PCA9468_REG_STS_A		0x04
#define PCA9468_BIT_IIN_LOOP_STS	BIT(7)
#define PCA9468_BIT_CHG_LOOP_STS	BIT(6)	/* not in pca9468 */
#define PCA9468_BIT_VFLT_LOOP_STS	BIT(5)
#define PCA9468_BIT_CFLY_SHORT_STS	BIT(4)
#define PCA9468_BIT_VOUT_UV_STS		BIT(3)
#define PCA9468_BIT_VBAT_OV_STS		BIT(2)
#define PCA9468_BIT_VIN_OV_STS		BIT(1)
#define PCA9468_BIT_VIN_UV_STS		BIT(0)

#define PCA9468_REG_STS_B		0x05
#define PCA9468_BIT_BATT_MISS_STS	BIT(7)
#define PCA9468_BIT_OCP_FAST_STS	BIT(6)
#define PCA9468_BIT_OCP_AVG_STS		BIT(5)
#define PCA9468_BIT_ACTIVE_STATE_STS	BIT(4)
#define PCA9468_BIT_SHUTDOWN_STATE_STS	BIT(3)
#define PCA9468_BIT_STANDBY_STATE_STS	BIT(2)
#define PCA9468_BIT_CHARGE_TIMER_STS	BIT(1)
#define PCA9468_BIT_WATCHDOG_TIMER_STS	BIT(0)

#define PCA9468_REG_STS_C		0x06	/* IIN status */
#define PCA9468_BIT_IIN_STS		BITS(7, 2)

#define PCA9468_REG_STS_D		0x07	/* ICHG status */
#define PCA9468_BIT_ICHG_STS		BITS(7, 1)

#define PCA9468_REG_STS_ADC_1		0x08
#define PCA9468_BIT_ADC_IIN7_0		BITS(7, 0)

#define PCA9468_REG_STS_ADC_2		0x09
#define PCA9468_BIT_ADC_IOUT5_0		BITS(7, 2)
#define PCA9468_BIT_ADC_IIN9_8		BITS(1, 0)

#define PCA9468_REG_STS_ADC_3		0x0A
#define PCA9468_BIT_ADC_VIN3_0		BITS(7, 4)
#define PCA9468_BIT_ADC_IOUT9_6		BITS(3, 0)

#define PCA9468_REG_STS_ADC_4		0x0B
#define PCA9468_BIT_ADC_VOUT1_0		BITS(7, 6)
#define PCA9468_BIT_ADC_VIN9_4		BITS(5, 0)

#define PCA9468_REG_STS_ADC_5		0x0C
#define PCA9468_BIT_ADC_VOUT9_2		BITS(7, 0)

#define PCA9468_REG_STS_ADC_6		0x0D
#define PCA9468_BIT_ADC_VBAT7_0		BITS(7, 0)

#define PCA9468_REG_STS_ADC_7		0x0E
#define PCA9468_BIT_ADC_DIETEMP5_0	BITS(7, 2)
#define PCA9468_BIT_ADC_VBAT9_8		BITS(1, 0)

#define PCA9468_REG_STS_ADC_8		0x0F
#define PCA9468_BIT_ADC_NTCV3_0		BITS(7, 4)
#define PCA9468_BIT_ADC_DIETEMP9_6	BITS(3, 0)

#define PCA9468_REG_STS_ADC_9		0x10
#define PCA9468_BIT_ADC_NTCV9_4		BITS(5, 0)

/*
 * Charge current cannot be in PCA9468.
#define PCA9468_REG_ICHG_CTRL		0x20
#define PCA9468_BIT_ICHG_SS		BIT(7)
#define PCA9468_BIT_ICHG_CFG		BITS(6, 0)
 */

#define PCA9468_REG_IIN_CTRL		0x21	/* Input current */
#define PCA9468_BIT_LIMIT_INCREMENT_EN	BIT(7)
#define PCA9468_BIT_IIN_SS		BIT(6)
#define PCA9468_BIT_IIN_CFG		BITS(5, 0)

#define PCA9468_REG_START_CTRL		0x22	/* device init and config */
#define PCA9468_BIT_SNSRES		BIT(7)
#define PCA9468_BIT_EN_CFG		BIT(6)
#define PCA9468_BIT_STANDBY_EN		BIT(5)
#define PCA9468_BIT_REV_IIN_DET		BIT(4)
#define PCA9468_BIT_FSW_CFG		BITS(3, 0)

#define PCA9468_REG_ADC_CTRL		0x23	/* ADC configuration */
#define PCA9468_BIT_FORCE_ADC_MODE	BITS(7, 6)
#define PCA9468_BIT_ADC_SHUTDOWN_CFG	BIT(5)
#define PCA9468_BIT_HIBERNATE_DELAY	BITS(4, 3)

#define PCA9468_REG_ADC_CFG		0x24	/* ADC channel configuration */
#define PCA9468_BIT_CH7_EN		BIT(7)
#define PCA9468_BIT_CH6_EN		BIT(6)
#define PCA9468_BIT_CH5_EN		BIT(5)
#define PCA9468_BIT_CH4_EN		BIT(4)
#define PCA9468_BIT_CH3_EN		BIT(3)
#define PCA9468_BIT_CH2_EN		BIT(2)
#define PCA9468_BIT_CH1_EN		BIT(1)

#define PCA9468_REG_TEMP_CTRL		0x25	/* Temperature configuration */
#define PCA9468_BIT_TEMP_REG		BITS(7, 6)
#define PCA9468_BIT_TEMP_DELTA		BITS(5, 4)
#define PCA9468_BIT_TEMP_REG_EN		BIT(3)
#define PCA9468_BIT_NTC_PROTECTION_EN	BIT(2)
#define PCA9468_BIT_TEMP_MAX_EN		BIT(1)

#define PCA9468_REG_PWR_COLLAPSE	0x26	/* Power collapse cfg */
#define PCA9468_BIT_UV_DELTA		BITS(7, 6)
#define PCA9468_BIT_IIN_FORCE_COUNT	BIT(4)
#define PCA9468_BIT_BAT_MISS_DET_EN	BIT(3)

#define PCA9468_REG_V_FLOAT		0x27	/* Voltage regulation */
#define PCA9468_BIT_V_FLOAT		BITS(7, 0)

#define PCA9468_REG_SAFETY_CTRL		0x28	/* Safety configuration */
#define PCA9468_BIT_WATCHDOG_EN		BIT(7)
#define PCA9468_BIT_WATCHDOG_CFG	BITS(6, 5)
#define PCA9468_BIT_CHG_TIMER_EN	BIT(4)
#define PCA9468_BIT_CHG_TIMER_CFG	BITS(3, 2)
#define PCA9468_BIT_OV_DELTA		BITS(1, 0)

#define PCA9468_REG_NTC_TH_1		0x29	/* Thermistor threshold  */
#define PCA9468_BIT_NTC_THRESHOLD7_0	BITS(7, 0)

#define PCA9468_REG_NTC_TH_2		0x2A	/* Thermistor threshold  */
#define PCA9468_BIT_NTC_THRESHOLD9_8	BITS(1, 0)

#define PCA9468_REG_ADC_ACCESS		0x30

#define PCA9468_REG_ADC_ADJUST		0x31
#define PCA9468_BIT_ADC_GAIN		BITS(7, 4)

#define PCA9468_REG_ADC_IMPROVE		0x3D
#define PCA9468_BIT_ADC_IIN_IMP		BIT(3)

#define PCA9468_REG_ADC_MODE		0x3F
#define PCA9468_BIT_ADC_MODE		BIT(4)

#define PCA9468_MAX_REGISTER		0x4F


#define PCA9468_IIN_CFG_MIN		500000
/* input current step, unit - uA */
#define PCA9468_IIN_CFG_STEP		100000
/* input current, unit - uA */
#define PCA9468_IIN_CFG(input_curr)	((input_curr) / PCA9468_IIN_CFG_STEP)
/* charging current, uint - uA  */
#define PCA9468_ICHG_CFG(_chg_current)	((_chg_current) / 100000)
/* v_float voltage, unit - uV */
#define PCA9468_V_FLOAT(_v_float)	(((_v_float) / 1000 - 3725) / 5)

#define PCA9468_SNSRES_5mOhm		0x00
#define PCA9468_SNSRES_10mOhm		PCA9468_BIT_SNSRES

#define PCA9468_NTC_TH_STEP		2346	/* 2.346mV, unit - uV */

/* VIN over voltage setting from 2*VOUT */
enum {
	OV_DELTA_10P,
	OV_DELTA_30P,
	OV_DELTA_20P,
	OV_DELTA_40P,
};

/* Switching frequency */
enum {
	FSW_CFG_833KHZ,
	FSW_CFG_893KHZ,
	FSW_CFG_935KHZ,
	FSW_CFG_980KHZ,
	FSW_CFG_1020KHZ,
	FSW_CFG_1080KHZ,
	FSW_CFG_1120KHZ,
	FSW_CFG_1160KHZ,
	FSW_CFG_440KHZ,
	FSW_CFG_490KHZ,
	FSW_CFG_540KHZ,
	FSW_CFG_590KHZ,
	FSW_CFG_630KHZ,
	FSW_CFG_680KHZ,
	FSW_CFG_730KHZ,
	FSW_CFG_780KHZ
};

/* Enable pin polarity selection */
#define PCA9468_EN_ACTIVE_H	0x00
#define PCA9468_EN_ACTIVE_L	PCA9468_BIT_EN_CFG
#define PCA9468_STANDBY_FORCED	PCA9468_BIT_STANDBY_EN
#define PCA9468_STANDBY_DONOT	0

/* ADC Channel */
enum {
	ADCCH_VOUT = 1,
	ADCCH_VIN,
	ADCCH_VBAT,	/* 3 */
	ADCCH_ICHG,
	ADCCH_IIN,	/* 5 */
	ADCCH_DIETEMP,	/* 6 */
	ADCCH_NTC,
	ADCCH_MAX
};

/* ADC step */
#define VIN_STEP	16000	/* 16mV(16000uV) LSB, Range(0V ~ 16.368V) */
#define VBAT_STEP	5000	/* 5mV(5000uV) LSB, Range(0V ~ 5.115V) */
#define IIN_STEP	4890 	/* 4.89mA(4890uA) LSB, Range(0A ~ 5A) */
#define ICHG_STEP	9780 	/* 9.78mA(9780uA) LSB, Range(0A ~ 10A) */
#define DIETEMP_STEP  	435	/* 0.435C LSB, Range(-25C ~ 160C) */
#define DIETEMP_DENOM	1000	/* 1000, denominator */
#define DIETEMP_MIN 	-25  	/* -25C */
#define DIETEMP_MAX	160	/* 160C */
#define VOUT_STEP	5000 	/* 5mV(5000uV) LSB, Range(0V ~ 5.115V) */
#define NTCV_STEP	2346 	/* 2.346mV(2346uV) LSB, Range(0V ~ 2.4V) */
#define ADC_IIN_OFFSET	900000	/* 900mA */
#define NTC_CURVE_THRESHOLD	185
#define NTC_CURVE_1_BASE	960
#define NTC_CURVE_1_SHIFT	2
#define NTC_CURVE_2_BASE	730
#define NTC_CURVE_2_SHIFT	3


#endif