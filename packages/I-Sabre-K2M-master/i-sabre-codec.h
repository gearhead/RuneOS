/*
 * Driver for I-Sabre K2M
 *
 * Author: Satoru Kawase
 *      Copyright 2017 Audiophonics
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 */

#ifndef _SND_SOC_ISABRECODEC
#define _SND_SOC_ISABRECODEC


/* ISABRECODEC Register Address */
#define ISABRECODEC_REG_01	0x01	/* Virtual Device ID  :  0x01 = I-Sabre K2M */
#define ISABRECODEC_REG_02	0x02	/* API revision       :  0x01 = Revision 01 */
#define ISABRECODEC_REG_10	0x10	/* 0x01 = 352.8kHz or 384kHz, 0x00 = otherwise */
#define ISABRECODEC_REG_20	0x20	/* 0 - 100 (decimal value, 0 = min., 100 = max.) */
#define ISABRECODEC_REG_21	0x21	/* 0x00 = Mute OFF, 0x01 = Mute ON */
#define ISABRECODEC_REG_22	0x22	/* 0x00 = Fast Roll-Off, 0x01 = Slow Roll-Off, 0x02 = Minimum Phase */
#define ISABRECODEC_REG_23	0x23	/* 0x00 = 47.44k, 0x01 = 50kHz, 0x02 = 60kHz, 0x03 = 70kHz */
#define ISABRECODEC_REG_24	0x24	/* 0x00 = I2S, 0x01 = SPDIF */
#define ISABRECODEC_MAX_REG	0x24	/* Maximum Register Number */

#endif /* _SND_SOC_ISABRECODEC */
