/*
 * Copyright 2012-2013 Andrew Smith
 * Copyright 2012 Xiangfu <xiangfu@openmobilefree.com>
 * Copyright 2013 Con Kolivas <kernel@kolivas.org>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 3 of the License, or (at your option)
 * any later version.  See COPYING for more details.
 */

/*
 * Those code should be works fine with V2 and V3 bitstream of Icarus.
 * Operation:
 *   No detection implement.
 *   Input: 64B = 32B midstate + 20B fill bytes + last 12 bytes of block head.
 *   Return: send back 32bits immediately when Icarus found a valid nonce.
 *           no query protocol implemented here, if no data send back in ~11.3
 *           seconds (full cover time on 32bit nonce range by 380MH/s speed)
 *           just send another work.
 * Notice:
 *   1. Icarus will start calculate when you push a work to them, even they
 *      are busy.
 *   2. The 2 FPGAs on Icarus will distribute the job, one will calculate the
 *      0 ~ 7FFFFFFF, another one will cover the 80000000 ~ FFFFFFFF.
 *   3. It's possible for 2 FPGAs both find valid nonce in the meantime, the 2
 *      valid nonce will all be send back.
 *   4. Icarus will stop work when: a valid nonce has been found or 32 bits
 *      nonce range is completely calculated.
 */


#include <float.h>
#include <limits.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <strings.h>
#include <sys/time.h>
#include <unistd.h>

#include "config.h"

#ifdef WIN32
// #include <windows.h>		// KRAMBLE use winsock2.h instead
#include <winsock2.h>
#endif

#include "compat.h"
#include "miner.h"
#include "usbutils.h"

// The serial I/O speed - Linux uses a define 'B115200' in bits/termios.h
#define ICARUS_IO_SPEED 115200

// The size of a successful nonce read
#define ICARUS_READ_SIZE 4

#define AMU_PREF_PACKET 256
#define BLT_PREF_PACKET 512
#define ICA_PREF_PACKET 256

// Ensure the sizes are correct for the Serial read
#if (ICARUS_READ_SIZE != 4)
#error ICARUS_READ_SIZE must be 4
#endif
#define ASSERT1(condition) __maybe_unused static char sizeof_uint32_t_must_be_4[(condition)?1:-1]
ASSERT1(sizeof(uint32_t) == 4);

// TODO: USB? Different calculation? - see usbstats to work it out e.g. 1/2 of normal send time
//  or even use that number? 1/2
// #define ICARUS_READ_TIME(baud) ((double)ICARUS_READ_SIZE * (double)8.0 / (double)(baud))
// maybe 1ms?
#define ICARUS_READ_TIME(baud) (0.001)

// USB ms timeout to wait - user specified timeouts are multiples of this
#define ICARUS_WAIT_TIMEOUT 100

// Defined in multiples of ICARUS_WAIT_TIMEOUT
// Must of course be greater than ICARUS_READ_COUNT_TIMING/ICARUS_WAIT_TIMEOUT
// There's no need to have this bigger, since the overhead/latency of extra work
// is pretty small once you get beyond a 10s nonce range time and 10s also
// means that nothing slower than 429MH/s can go idle so most icarus devices
// will always mine without idling
#define ICARUS_READ_TIME_LIMIT_MAX 100

// In timing mode: Default starting value until an estimate can be obtained
// 5000 ms allows for up to a ~840MH/s device
#define ICARUS_READ_COUNT_TIMING	(5000) /* *4 */
#define ICARUS_READ_COUNT_MIN		ICARUS_WAIT_TIMEOUT
#define SECTOMS(s)	((int)((s) * 1000))
// How many ms below the expected completion time to abort work
// extra in case the last read is delayed
#define ICARUS_READ_REDUCE	((int)(ICARUS_WAIT_TIMEOUT * 1.5))

// For a standard Icarus REV3 (to 5 places)
// Since this rounds up a the last digit - it is a slight overestimate
// Thus the hash rate will be a VERY slight underestimate
// (by a lot less than the displayed accuracy)
// Minor inaccuracy of these numbers doesn't affect the work done,
// only the displayed MH/s
#define ICARUS_REV3_HASH_TIME 0.0000000026316
#define LANCELOT_HASH_TIME 0.0000000025000
#define ASICMINERUSB_HASH_TIME 0.0000000029761
// TODO: What is it?
#define CAIRNSMORE1_HASH_TIME 0.0000000026316
#define NANOSEC 1000000000.0

// Icarus Rev3 doesn't send a completion message when it finishes
// the full nonce range, so to avoid being idle we must abort the
// work (by starting a new work item) shortly before it finishes
//
// Thus we need to estimate 2 things:
//	1) How many hashes were done if the work was aborted
//	2) How high can the timeout be before the Icarus is idle,
//		to minimise the number of work items started
//	We set 2) to 'the calculated estimate' - ICARUS_READ_REDUCE
//	to ensure the estimate ends before idle
//
// The simple calculation used is:
//	Tn = Total time in seconds to calculate n hashes
//	Hs = seconds per hash
//	Xn = number of hashes
//	W  = code/usb overhead per work
//
// Rough but reasonable estimate:
//	Tn = Hs * Xn + W	(of the form y = mx + b)
//
// Thus:
//	Line of best fit (using least squares)
//
//	Hs = (n*Sum(XiTi)-Sum(Xi)*Sum(Ti))/(n*Sum(Xi^2)-Sum(Xi)^2)
//	W = Sum(Ti)/n - (Hs*Sum(Xi))/n
//
// N.B. W is less when aborting work since we aren't waiting for the reply
//	to be transferred back (ICARUS_READ_TIME)
//	Calculating the hashes aborted at n seconds is thus just n/Hs
//	(though this is still a slight overestimate due to code delays)
//

// Both below must be exceeded to complete a set of data
// Minimum how long after the first, the last data point must be
#define HISTORY_SEC 60
// Minimum how many points a single ICARUS_HISTORY should have
#define MIN_DATA_COUNT 5
// The value MIN_DATA_COUNT used is doubled each history until it exceeds:
#define MAX_MIN_DATA_COUNT 100

static struct timeval history_sec = { HISTORY_SEC, 0 };

// Store the last INFO_HISTORY data sets
// [0] = current data, not yet ready to be included as an estimate
// Each new data set throws the last old set off the end thus
// keeping a ongoing average of recent data
#define INFO_HISTORY 10

struct ICARUS_HISTORY {
	struct timeval finish;
	double sumXiTi;
	double sumXi;
	double sumTi;
	double sumXi2;
	uint32_t values;
	uint32_t hash_count_min;
	uint32_t hash_count_max;
};

enum timing_mode { MODE_DEFAULT, MODE_SHORT, MODE_LONG, MODE_VALUE };

static const char *MODE_DEFAULT_STR = "default";
static const char *MODE_SHORT_STR = "short";
static const char *MODE_SHORT_STREQ = "short=";
static const char *MODE_LONG_STR = "long";
static const char *MODE_LONG_STREQ = "long=";
static const char *MODE_VALUE_STR = "value";
static const char *MODE_UNKNOWN_STR = "unknown";

struct ICARUS_INFO {
	// time to calculate the golden_ob
	uint64_t golden_hashes;
	struct timeval golden_tv;

	struct ICARUS_HISTORY history[INFO_HISTORY+1];
	uint32_t min_data_count;

	// seconds per Hash
	double Hs;
	// ms til we abort
	int read_time;
	// ms limit for (short=/long=) read_time
	int read_time_limit;

	enum timing_mode timing_mode;
	bool do_icarus_timing;

	double fullnonce;
	int count;
	double W;
	uint32_t values;
	uint64_t hash_count_range;

	// Determine the cost of history processing
	// (which will only affect W)
	uint64_t history_count;
	struct timeval history_time;

	// icarus-options
	int baud;
	int work_division;
	int fpga_count;
	uint32_t nonce_mask;
};

#define END_CONDITION 0x0000ffff

// Looking for options in --icarus-timing and --icarus-options:
//
// Code increments this each time we start to look at a device
// However, this means that if other devices are checked by
// the Icarus code (e.g. Avalon only as at 20130517)
// they will count in the option offset
//
// This, however, is deterministic so that's OK
//
// If we were to increment after successfully finding an Icarus
// that would be random since an Icarus may fail and thus we'd
// not be able to predict the option order
//
// Devices are checked in the order libusb finds them which is ?
//
static int option_offset = -1;

struct device_drv icarus_drv;

/*
#define ICA_BUFSIZ (0x200)

static void transfer_read(struct cgpu_info *icarus, uint8_t request_type, uint8_t bRequest, uint16_t wValue, uint16_t wIndex, char *buf, int bufsiz, int *amount, enum usb_cmds cmd)
{
	int err;

	err = usb_transfer_read(icarus, request_type, bRequest, wValue, wIndex, buf, bufsiz, amount, cmd);

	applog(LOG_DEBUG, "%s: cgid %d %s got err %d",
			icarus->drv->name, icarus->cgminer_id,
			usb_cmdname(cmd), err);
}
*/

static void _transfer(struct cgpu_info *icarus, uint8_t request_type, uint8_t bRequest, uint16_t wValue, uint16_t wIndex, uint32_t *data, int siz, enum usb_cmds cmd)
{
	int err;

	err = usb_transfer_data(icarus, request_type, bRequest, wValue, wIndex, data, siz, cmd);

	applog(LOG_DEBUG, "%s: cgid %d %s got err %d",
			icarus->drv->name, icarus->cgminer_id,
			usb_cmdname(cmd), err);
}

#define transfer(icarus, request_type, bRequest, wValue, wIndex, cmd) \
		_transfer(icarus, request_type, bRequest, wValue, wIndex, NULL, 0, cmd)

static void icarus_initialise(struct cgpu_info *icarus, int baud)
{
	uint16_t wValue, wIndex;
	enum sub_ident ident;
	int interface;

	if (icarus->usbinfo.nodev)
		return;

	//applog(LOG_ERR, "Initialise baud %d",baud);

	usb_set_cps(icarus, baud / 10);
	usb_enable_cps(icarus);

	interface = usb_interface(icarus);
	ident = usb_ident(icarus);

	switch (ident) {
		case IDENT_BLT:
		case IDENT_LLT:
		case IDENT_CMR1:
		case IDENT_CMR2:
			usb_set_pps(icarus, BLT_PREF_PACKET);

			// Reset
			transfer(icarus, FTDI_TYPE_OUT, FTDI_REQUEST_RESET, FTDI_VALUE_RESET,
				 interface, C_RESET);

			if (icarus->usbinfo.nodev)
				return;

			// Latency
			usb_ftdi_set_latency(icarus);

			if (icarus->usbinfo.nodev)
				return;

			// Set data control
			transfer(icarus, FTDI_TYPE_OUT, FTDI_REQUEST_DATA, FTDI_VALUE_DATA_BLT,
				 interface, C_SETDATA);

			if (icarus->usbinfo.nodev)
				return;

			// default to BLT/LLT 115200
			wValue = FTDI_VALUE_BAUD_BLT;
			wIndex = FTDI_INDEX_BAUD_BLT;

			if (ident == IDENT_CMR1 || ident == IDENT_CMR2) {
				switch (baud) {
					case 115200:
						wValue = FTDI_VALUE_BAUD_CMR_115;
						wIndex = FTDI_INDEX_BAUD_CMR_115;
						break;
					case 57600:
						wValue = FTDI_VALUE_BAUD_CMR_57;
						wIndex = FTDI_INDEX_BAUD_CMR_57;
						break;
					default:
						quit(1, "icarus_intialise() invalid baud (%d) for Cairnsmore1", baud);
						break;
				}
			}

			// Set the baud
			transfer(icarus, FTDI_TYPE_OUT, FTDI_REQUEST_BAUD, wValue,
				 (wIndex & 0xff00) | interface, C_SETBAUD);

			if (icarus->usbinfo.nodev)
				return;

			// Set Modem Control
			transfer(icarus, FTDI_TYPE_OUT, FTDI_REQUEST_MODEM, FTDI_VALUE_MODEM,
				 interface, C_SETMODEM);

			if (icarus->usbinfo.nodev)
				return;

			// Set Flow Control
			transfer(icarus, FTDI_TYPE_OUT, FTDI_REQUEST_FLOW, FTDI_VALUE_FLOW,
				 interface, C_SETFLOW);

			if (icarus->usbinfo.nodev)
				return;

			// Clear any sent data
			transfer(icarus, FTDI_TYPE_OUT, FTDI_REQUEST_RESET, FTDI_VALUE_PURGE_TX,
				 interface, C_PURGETX);

			if (icarus->usbinfo.nodev)
				return;

			// Clear any received data
			transfer(icarus, FTDI_TYPE_OUT, FTDI_REQUEST_RESET, FTDI_VALUE_PURGE_RX,
				 interface, C_PURGERX);
			break;
		case IDENT_ICA:
			usb_set_pps(icarus, ICA_PREF_PACKET);

			// Set Data Control
			transfer(icarus, PL2303_CTRL_OUT, PL2303_REQUEST_CTRL, PL2303_VALUE_CTRL,
				 interface, C_SETDATA);

			if (icarus->usbinfo.nodev)
				return;

			// Set Line Control
			uint32_t ica_data[2] = { PL2303_VALUE_LINE0, PL2303_VALUE_LINE1 };
			_transfer(icarus, PL2303_CTRL_OUT, PL2303_REQUEST_LINE, PL2303_VALUE_LINE,
				 interface, &ica_data[0], PL2303_VALUE_LINE_SIZE, C_SETLINE);

			if (icarus->usbinfo.nodev)
				return;

			// Vendor
			transfer(icarus, PL2303_VENDOR_OUT, PL2303_REQUEST_VENDOR, PL2303_VALUE_VENDOR,
				 interface, C_VENDOR);
			break;
		case IDENT_AMU:
			usb_set_pps(icarus, AMU_PREF_PACKET);

			// Enable the UART
			transfer(icarus, CP210X_TYPE_OUT, CP210X_REQUEST_IFC_ENABLE,
				 CP210X_VALUE_UART_ENABLE,
				 interface, C_ENABLE_UART);

			if (icarus->usbinfo.nodev)
				return;

			// Set data control
			transfer(icarus, CP210X_TYPE_OUT, CP210X_REQUEST_DATA, CP210X_VALUE_DATA,
				 interface, C_SETDATA);

			if (icarus->usbinfo.nodev)
				return;

			// Set the baud
			uint32_t data = CP210X_DATA_BAUD;
			_transfer(icarus, CP210X_TYPE_OUT, CP210X_REQUEST_BAUD, 0,
				 interface, &data, sizeof(data), C_SETBAUD);
			break;
		default:
			quit(1, "icarus_intialise() called with invalid %s cgid %i ident=%d",
				icarus->drv->name, icarus->cgminer_id, ident);
	}
}

static void rev(unsigned char *s, size_t l)
{
	size_t i, j;
	unsigned char t;

	for (i = 0, j = l - 1; i < j; i++, j--) {
		t = s[i];
		s[i] = s[j];
		s[j] = t;
	}
}

#define ICA_NONCE_ERROR -1
#define ICA_NONCE_OK 0
#define ICA_NONCE_RESTART 1
#define ICA_NONCE_TIMEOUT 2

static int icarus_get_nonce(struct cgpu_info *icarus, unsigned char *buf, struct timeval *tv_start, struct timeval *tv_finish, struct thr_info *thr, int read_time)
{
	struct timeval read_start, read_finish;
	int err, amt;
	int rc = 0;
	int read_amount = ICARUS_READ_SIZE;
	bool first = true;

	cgtime(tv_start);
	while (true) {
		if (icarus->usbinfo.nodev)
			return ICA_NONCE_ERROR;

		cgtime(&read_start);
		err = usb_read_timeout(icarus, (char *)buf, read_amount, &amt, ICARUS_WAIT_TIMEOUT, C_GETRESULTS);
		cgtime(&read_finish);
		//applog(LOG_ERR, "USB read err=%d amt=%d",err,amt);
		if (err < 0 && err != LIBUSB_ERROR_TIMEOUT) {
			applog(LOG_ERR, "%s%i: Comms error (rerr=%d amt=%d)",
					icarus->drv->name, icarus->device_id, err, amt);
			dev_error(icarus, REASON_DEV_COMMS_ERROR);
			return ICA_NONCE_ERROR;
		}

		if (first)
			copy_time(tv_finish, &read_finish);

		if (amt >= read_amount)
			return ICA_NONCE_OK;

		rc = SECTOMS(tdiff(&read_finish, tv_start));
		if (rc >= read_time) {
			if (amt > 0)
				applog(LOG_DEBUG, "Icarus Read: Timeout reading for %d ms", rc);
			else
				applog(LOG_DEBUG, "Icarus Read: No data for %d ms", rc);
			return ICA_NONCE_TIMEOUT;
		}

		if (thr && thr->work_restart) {
			if (opt_debug) {
				applog(LOG_DEBUG,
					"Icarus Read: Work restart at %d ms", rc);
			}
			return ICA_NONCE_RESTART;
		}

		if (amt > 0) {
			buf += amt;
			read_amount -= amt;
			first = false;
		}
	}
}

static const char *timing_mode_str(enum timing_mode timing_mode)
{
	switch(timing_mode) {
	case MODE_DEFAULT:
		return MODE_DEFAULT_STR;
	case MODE_SHORT:
		return MODE_SHORT_STR;
	case MODE_LONG:
		return MODE_LONG_STR;
	case MODE_VALUE:
		return MODE_VALUE_STR;
	default:
		return MODE_UNKNOWN_STR;
	}
}

static void set_timing_mode(int this_option_offset, struct cgpu_info *icarus)
{
	struct ICARUS_INFO *info = (struct ICARUS_INFO *)(icarus->device_data);
	enum sub_ident ident;
	double Hs;
	char buf[BUFSIZ+1];
	char *ptr, *comma, *eq;
	size_t max;
	int i;

	if (opt_icarus_timing == NULL)
		buf[0] = '\0';
	else {
		ptr = opt_icarus_timing;
		for (i = 0; i < this_option_offset; i++) {
			comma = strchr(ptr, ',');
			if (comma == NULL)
				break;
			ptr = comma + 1;
		}

		comma = strchr(ptr, ',');
		if (comma == NULL)
			max = strlen(ptr);
		else
			max = comma - ptr;

		if (max > BUFSIZ)
			max = BUFSIZ;
		strncpy(buf, ptr, max);
		buf[max] = '\0';
	}

	ident = usb_ident(icarus);
	switch (ident) {
		case IDENT_ICA:
			info->Hs = ICARUS_REV3_HASH_TIME;
			break;
		case IDENT_BLT:
		case IDENT_LLT:
			info->Hs = LANCELOT_HASH_TIME;
			break;
		case IDENT_AMU:
			info->Hs = ASICMINERUSB_HASH_TIME;
			break;
		// TODO: ?
		case IDENT_CMR1:
		case IDENT_CMR2:
			info->Hs = CAIRNSMORE1_HASH_TIME;
			break;
		default:
			quit(1, "Icarus get_options() called with invalid %s ident=%d",
				icarus->drv->name, ident);
	}

	info->read_time = 0;
	info->read_time_limit = 0; // 0 = no limit

	if (strcasecmp(buf, MODE_SHORT_STR) == 0) {
		// short
		info->read_time = ICARUS_READ_COUNT_TIMING;

		info->timing_mode = MODE_SHORT;
		info->do_icarus_timing = true;
	} else if (strncasecmp(buf, MODE_SHORT_STREQ, strlen(MODE_SHORT_STREQ)) == 0) {
		// short=limit
		info->read_time = ICARUS_READ_COUNT_TIMING;

		info->timing_mode = MODE_SHORT;
		info->do_icarus_timing = true;

		info->read_time_limit = atoi(&buf[strlen(MODE_SHORT_STREQ)]);
		if (info->read_time_limit < 0)
			info->read_time_limit = 0;
		if (info->read_time_limit > ICARUS_READ_TIME_LIMIT_MAX)
			info->read_time_limit = ICARUS_READ_TIME_LIMIT_MAX;
	} else if (strcasecmp(buf, MODE_LONG_STR) == 0) {
		// long
		info->read_time = ICARUS_READ_COUNT_TIMING;

		info->timing_mode = MODE_LONG;
		info->do_icarus_timing = true;
	} else if (strncasecmp(buf, MODE_LONG_STREQ, strlen(MODE_LONG_STREQ)) == 0) {
		// long=limit
		info->read_time = ICARUS_READ_COUNT_TIMING;

		info->timing_mode = MODE_LONG;
		info->do_icarus_timing = true;

		info->read_time_limit = atoi(&buf[strlen(MODE_LONG_STREQ)]);
		if (info->read_time_limit < 0)
			info->read_time_limit = 0;
		if (info->read_time_limit > ICARUS_READ_TIME_LIMIT_MAX)
			info->read_time_limit = ICARUS_READ_TIME_LIMIT_MAX;
	} else if ((Hs = atof(buf)) != 0) {
		// ns[=read_time]
		info->Hs = Hs / NANOSEC;
		info->fullnonce = info->Hs * (((double)0xffffffff) + 1);

		if ((eq = strchr(buf, '=')) != NULL)
			info->read_time = atoi(eq+1) * ICARUS_WAIT_TIMEOUT;

		if (info->read_time < ICARUS_READ_COUNT_MIN)
			info->read_time = SECTOMS(info->fullnonce) - ICARUS_READ_REDUCE;

		if (unlikely(info->read_time < ICARUS_READ_COUNT_MIN))
			info->read_time = ICARUS_READ_COUNT_MIN;

		info->timing_mode = MODE_VALUE;
		info->do_icarus_timing = false;
	} else {
		// Anything else in buf just uses DEFAULT mode

		info->fullnonce = info->Hs * (((double)0xffffffff) + 1);

		if ((eq = strchr(buf, '=')) != NULL)
			info->read_time = atoi(eq+1) * ICARUS_WAIT_TIMEOUT;

		if (info->read_time < ICARUS_READ_COUNT_MIN)
			info->read_time = SECTOMS(info->fullnonce) - ICARUS_READ_REDUCE;

		if (unlikely(info->read_time < ICARUS_READ_COUNT_MIN))
			info->read_time = ICARUS_READ_COUNT_MIN;

		info->timing_mode = MODE_DEFAULT;
		info->do_icarus_timing = false;
	}

	info->min_data_count = MIN_DATA_COUNT;

	// All values are in multiples of ICARUS_WAIT_TIMEOUT
	info->read_time_limit *= ICARUS_WAIT_TIMEOUT;

	applog(LOG_DEBUG, "%s: cgid %d Init: mode=%s read_time=%dms limit=%dms Hs=%e",
			icarus->drv->name, icarus->cgminer_id,
			timing_mode_str(info->timing_mode),
			info->read_time, info->read_time_limit, info->Hs);
}

static uint32_t mask(int work_division)
{
	uint32_t nonce_mask = 0x7fffffff;

	// yes we can calculate these, but this way it's easy to see what they are
	switch (work_division) {
	case 1:
		nonce_mask = 0xffffffff;
		break;
	case 2:
		nonce_mask = 0x7fffffff;
		break;
	case 4:
		nonce_mask = 0x3fffffff;
		break;
	case 8:
		nonce_mask = 0x1fffffff;
		break;
	default:
		quit(1, "Invalid2 icarus-options for work_division (%d) must be 1, 2, 4 or 8", work_division);
	}

	return nonce_mask;
}

static void get_options(int this_option_offset, struct cgpu_info *icarus, int *baud, int *work_division, int *fpga_count)
{
	char buf[BUFSIZ+1];
	char *ptr, *comma, *colon, *colon2;
	enum sub_ident ident;
	size_t max;
	int i, tmp;

	if (opt_icarus_options == NULL)
		buf[0] = '\0';
	else {
		ptr = opt_icarus_options;
		for (i = 0; i < this_option_offset; i++) {
			comma = strchr(ptr, ',');
			if (comma == NULL)
				break;
			ptr = comma + 1;
		}

		comma = strchr(ptr, ',');
		if (comma == NULL)
			max = strlen(ptr);
		else
			max = comma - ptr;

		if (max > BUFSIZ)
			max = BUFSIZ;
		strncpy(buf, ptr, max);
		buf[max] = '\0';
	}

	ident = usb_ident(icarus);
	switch (ident) {
		case IDENT_ICA:
		case IDENT_BLT:
		case IDENT_LLT:
			*baud = ICARUS_IO_SPEED;
			*work_division = 2;
			*fpga_count = 2;
			break;
		case IDENT_AMU:
			*baud = ICARUS_IO_SPEED;
			*work_division = 1;
			*fpga_count = 1;
			break;
		// TODO: ?
		case IDENT_CMR1:
		case IDENT_CMR2:
			*baud = ICARUS_IO_SPEED;
			*work_division = 2;
			*fpga_count = 2;
			break;
		default:
			quit(1, "Icarus get_options() called with invalid %s ident=%d",
				icarus->drv->name, ident);
	}

	if (*buf) {
		colon = strchr(buf, ':');
		if (colon)
			*(colon++) = '\0';

		if (*buf) {
			tmp = atoi(buf);
			switch (tmp) {
			case 115200:
				*baud = 115200;
				break;
			case 57600:
				*baud = 57600;
				break;
			default:
				quit(1, "Invalid icarus-options for baud (%s) must be 115200 or 57600", buf);
			}
		}

		if (colon && *colon) {
			colon2 = strchr(colon, ':');
			if (colon2)
				*(colon2++) = '\0';

			if (*colon) {
				tmp = atoi(colon);
				if (tmp == 1 || tmp == 2 || tmp == 4 || tmp == 8) {
					*work_division = tmp;
					*fpga_count = tmp;	// default to the same
				} else {
					quit(1, "Invalid icarus-options for work_division (%s) must be 1, 2, 4 or 8", colon);
				}
			}

			if (colon2 && *colon2) {
				tmp = atoi(colon2);
				if (tmp > 0 && tmp <= *work_division)
					*fpga_count = tmp;
				else {
					quit(1, "Invalid icarus-options for fpga_count (%s) must be >0 and <=work_division (%d)", colon2, *work_division);
				}
			}
		}
	}
}

int usb_write_ica(struct cgpu_info *cgpu, char *buf, size_t bufsiz, int *processed, enum usb_cmds uc )
{
    int i;
    int err;
    int written = 0;
    int got;
    char tmp[52];
    char rbuf[16];
    tmp[0]=0;
    tmp[1]=0;
    tmp[2]=0x07;
    tmp[3]=0xFF;
    memcpy(&tmp[ 4],&buf[16*3],16);
    memcpy(&tmp[20],&buf[   0],32);
    tmp[7]=0x41; //BUG
    //clear
    for(i=0; i<512; i++) {
	got=0;
	err = usb_read_timeout(cgpu, rbuf, sizeof(rbuf), &got, 100, C_GETRESULTS);
	if(got<sizeof(rbuf))
	    break;
    }

    err = usb_write(cgpu, tmp, 52, &written, uc );
    if(written==52){
	*processed = bufsiz;
    }
    return err;
}

static bool icarus_detect_one(struct libusb_device *dev, struct usb_find_devices *found)
{
	int this_option_offset = ++option_offset;
	struct ICARUS_INFO *info;
	struct timeval tv_start, tv_finish;

	/* Blakecoin detection hash
	N.B. golden_ob MUST take less time to calculate than the timeout set in icarus_open()
    0000007002685447273026edebf62cf5e17454f35cc7b1f2da57caeb008cf4fb00000000dad683f2975c7e00a8088275099c69a3c589916aaa9c7c2501d136c1bf78422d5256fbaa1c01d9d1b48b4600000000800000000000000000000000000000000000000000000000000000000000000000000000000000000080020000
	{ midstate, data } = { 256'h553bf521cf6f816d21b2e3c660f29469f8b6ae935291176ef5dda6fe442ca6e4, 96'hd1d9011caafb56522d4278bf };
	*/
	const char golden_ob[] =
//		"553bf521cf6f816d21b2e3c660f29469"
//		"f8b6ae935291176ef5dda6fe442ca6e4"
//		"00000000000000000000000000000000"
//		"00000000d1d9011caafb56522d4278bf";

//-----------
		"a8c369073d7dc0a63168f5fcf0246e4f"
		"eb916bda12787ad1607d2303186ed8f1"
		"00000000000000000000000000000000"
		"0142b9a0e7b4001cf8b35852a3accab0";

	const char golden_nonce[] = "0142b9b1"; //"000187a2";
	const uint32_t golden_nonce_val = 0x0142b9b1; //0x000187a2;
	unsigned char ob_bin[64];
	unsigned char nonce_bin[ICARUS_READ_SIZE];
	char *nonce_hex;
	int baud, uninitialised_var(work_division), uninitialised_var(fpga_count);
	struct cgpu_info *icarus;
	int ret, err, amount, tries;
	bool ok;

	char tmpbuf[256];	//lancelot52
	unsigned char* wr_buf = ob_bin;
	int bufLen = sizeof(ob_bin);

	icarus = usb_alloc_cgpu(&icarus_drv, 1);

	if (!usb_init(icarus, dev, found))
		goto shin;

	usb_buffer_enable(icarus);

	get_options(this_option_offset, icarus, &baud, &work_division, &fpga_count);

	hex2bin(ob_bin, golden_ob, sizeof(ob_bin));

	tries = 2;
	ok = false;
	while (!ok && tries-- > 0) {
		icarus_initialise(icarus, baud);

		err = usb_write_ica(icarus, (char *)wr_buf, bufLen, &amount, C_SENDTESTWORK);

		if (err != LIBUSB_SUCCESS || amount != bufLen)
			continue;

		memset(nonce_bin, 0, sizeof(nonce_bin));
		ret = icarus_get_nonce(icarus, nonce_bin, &tv_start, &tv_finish, NULL, 500);
		applog(LOG_ERR, "???Get nonce ret=%d %02X%02X%02X%02X",ret,nonce_bin[0],nonce_bin[1],nonce_bin[2],nonce_bin[3]);

		if (ret != ICA_NONCE_OK)
			continue;

		nonce_hex = bin2hex(nonce_bin, sizeof(nonce_bin));
		if (strncmp(nonce_hex, golden_nonce, 8) == 0)
			ok = true;
		else {
			if (tries < 0) {
				applog(LOG_ERR,
					"Icarus Detect: "
					"Test failed at %s: get %s, should: %s",
					icarus->device_path, nonce_hex, golden_nonce);
			}
		}
		free(nonce_hex);
	}

	if (!ok)
		goto unshin;

	applog(LOG_DEBUG,
		"Icarus Detect: "
		"Test succeeded at %s: got %s",
			icarus->device_path, golden_nonce);

	/* We have a real Icarus! */
	if (!add_cgpu(icarus))
		goto unshin;

	update_usb_stats(icarus);

	applog(LOG_INFO, "%s%d: Found at %s",
		icarus->drv->name, icarus->device_id, icarus->device_path);

	applog(LOG_DEBUG, "%s%d: Init baud=%d work_division=%d fpga_count=%d",
		icarus->drv->name, icarus->device_id, baud, work_division, fpga_count);

	info = (struct ICARUS_INFO *)malloc(sizeof(struct ICARUS_INFO));
	if (unlikely(!info))
		quit(1, "Failed to malloc ICARUS_INFO");

	icarus->device_data = (void *)info;

	// Initialise everything to zero for a new device
	memset(info, 0, sizeof(struct ICARUS_INFO));

	info->baud = baud;
	info->work_division = work_division;
	info->fpga_count = fpga_count;
	info->nonce_mask = mask(work_division);

	info->golden_hashes = (golden_nonce_val & info->nonce_mask) * fpga_count;
	timersub(&tv_finish, &tv_start, &(info->golden_tv));

	set_timing_mode(this_option_offset, icarus);

	return true;

unshin:

	usb_uninit(icarus);

shin:

	icarus = usb_free_cgpu(icarus);

	return false;
}

static void icarus_detect()
{
	usb_detect(&icarus_drv, icarus_detect_one);
}

static bool icarus_prepare(__maybe_unused struct thr_info *thr)
{
//	struct cgpu_info *icarus = thr->cgpu;

	return true;
}


static int64_t icarus_scanhash(struct thr_info *thr, struct work *work,
				__maybe_unused int64_t max_nonce)
{
	struct cgpu_info *icarus = thr->cgpu;
	struct ICARUS_INFO *info = (struct ICARUS_INFO *)(icarus->device_data);
	int ret, err, amount;
	unsigned char ob_bin[64], nonce_bin[ICARUS_READ_SIZE];
	char *ob_hex;
	uint32_t nonce;
	int64_t hash_count;
	struct timeval tv_start, tv_finish, elapsed;
	struct timeval tv_history_start, tv_history_finish;
	double Ti, Xi;
	int curr_hw_errors, i;
	bool was_hw_error;

	struct ICARUS_HISTORY *history0, *history;
	int count;
	double Hs, W, fullnonce;
	int read_time;
	bool limited;
	int64_t estimate_hashes;
	uint32_t values;
	int64_t hash_count_range;

	// Device is gone
	if (icarus->usbinfo.nodev)
		return -1;

	elapsed.tv_sec = elapsed.tv_usec = 0;

	memset(ob_bin, 0, sizeof(ob_bin));
	memcpy(ob_bin, work->midstate, 32);
	memcpy(ob_bin + 52, work->data + 64, 12);
	rev(ob_bin, 32);
	rev(ob_bin + 52, 12);

	// We only want results for the work we are about to send
	usb_buffer_clear(icarus);

	err = usb_write_ica(icarus, (char *)ob_bin, sizeof(ob_bin), &amount, C_SENDWORK);
	if (err < 0 || amount != sizeof(ob_bin)) {
		applog(LOG_ERR, "%s%i: Comms error (werr=%d amt=%d)",
				icarus->drv->name, icarus->device_id, err, amount);
		dev_error(icarus, REASON_DEV_COMMS_ERROR);
		icarus_initialise(icarus, info->baud);
		return 0;
	}

	//if (opt_debug) 
	{
		ob_hex = bin2hex(ob_bin, sizeof(ob_bin));
		applog(LOG_ERR, "%s%d: sent %s (%d)",
			icarus->drv->name, icarus->device_id, ob_hex,info->read_time);
		free(ob_hex);
	}

wait_next:
	/* Icarus will return 4 bytes (ICARUS_READ_SIZE) nonces or nothing */
	memset(nonce_bin, 0, sizeof(nonce_bin));
	ret = icarus_get_nonce(icarus, nonce_bin, &tv_start, &tv_finish, thr, info->read_time*10);
	if (ret == ICA_NONCE_ERROR) {
		applog(LOG_ERR, "%s%d: nonce err",
			icarus->drv->name, icarus->device_id);
		return 0;
	}

	work->blk.nonce = 0xffffffff;

	// aborted before becoming idle, get new work
	if (ret == ICA_NONCE_TIMEOUT || ret == ICA_NONCE_RESTART) {
		timersub(&tv_finish, &tv_start, &elapsed);

		// ONLY up to just when it aborted
		// We didn't read a reply so we don't subtract ICARUS_READ_TIME
		estimate_hashes = ((double)(elapsed.tv_sec)
					+ ((double)(elapsed.tv_usec))/((double)1000000)) / info->Hs;

		// If some Serial-USB delay allowed the full nonce range to
		// complete it can't have done more than a full nonce
		if (unlikely(estimate_hashes > 0xffffffff))
			estimate_hashes = 0xffffffff;

		//if (opt_debug) 
		{
			applog(LOG_ERR, "%s%d: no nonce = 0x%08lX hashes (%ld.%06lds)",
					icarus->drv->name, icarus->device_id,
					(long unsigned int)estimate_hashes,
					elapsed.tv_sec, elapsed.tv_usec);
		}

		return estimate_hashes;
	}


	memcpy((char *)&nonce, nonce_bin, sizeof(nonce_bin));
	if(nonce==0xFFFFFFFF) {
	    applog(LOG_ERR, "End of nonce search = 0x%08X",nonce);
	    return hash_count;
	}
	applog(LOG_ERR, "submit nonce = 0x%08X",nonce);
	nonce = htobe32(nonce);
	curr_hw_errors = icarus->hw_errors;
	if ( submit_nonce(thr, work, nonce) ) {
	    applog(LOG_ERR, "submit nonce = 0x%08X SUCCESS/ACCEPTED!!!",nonce);
	    hash_count++;
	    return hash_count;
	}
	was_hw_error = (curr_hw_errors > icarus->hw_errors);

	hash_count = (nonce & info->nonce_mask);
	hash_count++;
	hash_count *= info->fpga_count;
	goto wait_next;

#if 0
	// This appears to only return zero nonce values
	if (usb_buffer_size(icarus) > 3) {
		memcpy((char *)&nonce, icarus->usbdev->buffer, sizeof(nonce_bin));
		nonce = htobe32(nonce);
		applog(LOG_WARNING, "%s%d: attempting to submit 2nd nonce = 0x%08lX",
				icarus->drv->name, icarus->device_id,
				(long unsigned int)nonce);
		curr_hw_errors = icarus->hw_errors;
		submit_nonce(thr, work, nonce);
		was_hw_error = (curr_hw_errors > icarus->hw_errors);
	}
#endif

	if (opt_debug || info->do_icarus_timing)
		timersub(&tv_finish, &tv_start, &elapsed);

	if (opt_debug) {
		applog(LOG_DEBUG, "%s%d: nonce = 0x%08x = 0x%08lX hashes (%ld.%06lds)",
				icarus->drv->name, icarus->device_id,
				nonce, (long unsigned int)hash_count,
				elapsed.tv_sec, elapsed.tv_usec);
	}

	// Ignore possible end condition values ... and hw errors
	// TODO: set limitations on calculated values depending on the device
	// to avoid crap values caused by CPU/Task Switching/Swapping/etc
	if (info->do_icarus_timing
	&&  !was_hw_error
	&&  ((nonce & info->nonce_mask) > END_CONDITION)
	&&  ((nonce & info->nonce_mask) < (info->nonce_mask & ~END_CONDITION))) {
		cgtime(&tv_history_start);

		history0 = &(info->history[0]);

		if (history0->values == 0)
			timeradd(&tv_start, &history_sec, &(history0->finish));

		Ti = (double)(elapsed.tv_sec)
			+ ((double)(elapsed.tv_usec))/((double)1000000)
			- ((double)ICARUS_READ_TIME(info->baud));
		Xi = (double)hash_count;
		history0->sumXiTi += Xi * Ti;
		history0->sumXi += Xi;
		history0->sumTi += Ti;
		history0->sumXi2 += Xi * Xi;

		history0->values++;

		if (history0->hash_count_max < hash_count)
			history0->hash_count_max = hash_count;
		if (history0->hash_count_min > hash_count || history0->hash_count_min == 0)
			history0->hash_count_min = hash_count;

		if (history0->values >= info->min_data_count
		&&  timercmp(&tv_start, &(history0->finish), >)) {
			for (i = INFO_HISTORY; i > 0; i--)
				memcpy(&(info->history[i]),
					&(info->history[i-1]),
					sizeof(struct ICARUS_HISTORY));

			// Initialise history0 to zero for summary calculation
			memset(history0, 0, sizeof(struct ICARUS_HISTORY));

			// We just completed a history data set
			// So now recalc read_time based on the whole history thus we will
			// initially get more accurate until it completes INFO_HISTORY
			// total data sets
			count = 0;
			for (i = 1 ; i <= INFO_HISTORY; i++) {
				history = &(info->history[i]);
				if (history->values >= MIN_DATA_COUNT) {
					count++;

					history0->sumXiTi += history->sumXiTi;
					history0->sumXi += history->sumXi;
					history0->sumTi += history->sumTi;
					history0->sumXi2 += history->sumXi2;
					history0->values += history->values;

					if (history0->hash_count_max < history->hash_count_max)
						history0->hash_count_max = history->hash_count_max;
					if (history0->hash_count_min > history->hash_count_min || history0->hash_count_min == 0)
						history0->hash_count_min = history->hash_count_min;
				}
			}

			// All history data
			Hs = (history0->values*history0->sumXiTi - history0->sumXi*history0->sumTi)
				/ (history0->values*history0->sumXi2 - history0->sumXi*history0->sumXi);
			W = history0->sumTi/history0->values - Hs*history0->sumXi/history0->values;
			hash_count_range = history0->hash_count_max - history0->hash_count_min;
			values = history0->values;
			
			// Initialise history0 to zero for next data set
			memset(history0, 0, sizeof(struct ICARUS_HISTORY));

			fullnonce = W + Hs * (((double)0xffffffff) + 1);
			read_time = SECTOMS(fullnonce) - ICARUS_READ_REDUCE;
			if (info->read_time_limit > 0 && read_time > info->read_time_limit) {
				read_time = info->read_time_limit;
				limited = true;
			} else
				limited = false;

			info->Hs = Hs;
			info->read_time = read_time;

			info->fullnonce = fullnonce;
			info->count = count;
			info->W = W;
			info->values = values;
			info->hash_count_range = hash_count_range;

			if (info->min_data_count < MAX_MIN_DATA_COUNT)
				info->min_data_count *= 2;
			else if (info->timing_mode == MODE_SHORT)
				info->do_icarus_timing = false;

			applog(LOG_WARNING, "%s%d Re-estimate: Hs=%e W=%e read_time=%dms%s fullnonce=%.3fs",
					icarus->drv->name, icarus->device_id, Hs, W, read_time,
					limited ? " (limited)" : "", fullnonce);
		}
		info->history_count++;
		cgtime(&tv_history_finish);

		timersub(&tv_history_finish, &tv_history_start, &tv_history_finish);
		timeradd(&tv_history_finish, &(info->history_time), &(info->history_time));
	}

	return hash_count;
}

static struct api_data *icarus_api_stats(struct cgpu_info *cgpu)
{
	struct api_data *root = NULL;
	struct ICARUS_INFO *info = (struct ICARUS_INFO *)(cgpu->device_data);

	// Warning, access to these is not locked - but we don't really
	// care since hashing performance is way more important than
	// locking access to displaying API debug 'stats'
	// If locking becomes an issue for any of them, use copy_data=true also
	root = api_add_int(root, "read_time", &(info->read_time), false);
	root = api_add_int(root, "read_time_limit", &(info->read_time_limit), false);
	root = api_add_double(root, "fullnonce", &(info->fullnonce), false);
	root = api_add_int(root, "count", &(info->count), false);
	root = api_add_hs(root, "Hs", &(info->Hs), false);
	root = api_add_double(root, "W", &(info->W), false);
	root = api_add_uint(root, "total_values", &(info->values), false);
	root = api_add_uint64(root, "range", &(info->hash_count_range), false);
	root = api_add_uint64(root, "history_count", &(info->history_count), false);
	root = api_add_timeval(root, "history_time", &(info->history_time), false);
	root = api_add_uint(root, "min_data_count", &(info->min_data_count), false);
	root = api_add_uint(root, "timing_values", &(info->history[0].values), false);
	root = api_add_const(root, "timing_mode", timing_mode_str(info->timing_mode), false);
	root = api_add_bool(root, "is_timing", &(info->do_icarus_timing), false);
	root = api_add_int(root, "baud", &(info->baud), false);
	root = api_add_int(root, "work_division", &(info->work_division), false);
	root = api_add_int(root, "fpga_count", &(info->fpga_count), false);

	return root;
}

static void icarus_shutdown(__maybe_unused struct thr_info *thr)
{
	// TODO: ?
}

struct device_drv icarus_drv = {
	.drv_id = DRIVER_ICARUS,
	.dname = "Icarus",
	.name = "ICA",
	.drv_detect = icarus_detect,
	.get_api_stats = icarus_api_stats,
	.thread_prepare = icarus_prepare,
	.scanhash = icarus_scanhash,
	.thread_shutdown = icarus_shutdown,
};
