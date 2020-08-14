/*
 * Copyright 2018 Google, Inc
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#ifdef CONFIG_PM_SLEEP
#define SUPPORT_PM_SLEEP 1
#endif

#include <linux/kernel.h>
#include <linux/printk.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/pm_runtime.h>
#include <linux/platform_device.h>
#include <linux/power_supply.h>
#include <linux/pm_wakeup.h>
#include <linux/pmic-voter.h>
#include <linux/thermal.h>
#include <linux/slab.h>
#include "google_bms.h"
#include "google_psy.h"
#include "qmath.h"
#include "logbuffer.h"
#include <crypto/hash.h>

#include <linux/debugfs.h>

#define BATT_DELAY_INIT_MS		250
#define BATT_WORK_FAST_RETRY_CNT	30
#define BATT_WORK_FAST_RETRY_MS		1000
#define BATT_WORK_ERROR_RETRY_MS	1000

#define DEFAULT_BATT_FAKE_CAPACITY		50
#define DEFAULT_BATT_UPDATE_INTERVAL		30000
#define DEFAULT_BATT_DRV_RL_SOC_THRESHOLD	97
#define DEFAULT_HIGH_TEMP_UPDATE_THRESHOLD	550

#define MSC_ERROR_UPDATE_INTERVAL		5000
#define MSC_DEFAULT_UPDATE_INTERVAL		30000

/* qual time is 15 minutes of charge or 15% increase in SOC */
#define DEFAULT_CHG_STATS_MIN_QUAL_TIME		(15 * 60)
#define DEFAULT_CHG_STATS_MIN_DELTA_SOC		15

/* Voters */
#define MSC_LOGIC_VOTER	"msc_logic"
#define SW_JEITA_VOTER	"sw_jeita"
#define RL_STATE_VOTER	"rl_state"

#define UICURVE_MAX	3

/* Initial data of history cycle count */
#define HCC_DEFAULT_DELTA_CYCLE_CNT	25

#undef MODULE_PARAM_PREFIX
#define MODULE_PARAM_PREFIX	"androidboot."
#define DEV_SN_LENGTH		20
static char dev_sn[DEV_SN_LENGTH];
module_param_string(serialno, dev_sn, DEV_SN_LENGTH, 0000);

#if (GBMS_CCBIN_BUCKET_COUNT < 1) || (GBMS_CCBIN_BUCKET_COUNT > 100)
#error "GBMS_CCBIN_BUCKET_COUNT needs to be a value from 1-100"
#endif

#define get_boot_sec() div_u64(ktime_to_ns(ktime_get_boottime()), NSEC_PER_SEC)

struct ssoc_uicurve {
	qnum_t real;
	qnum_t ui;
};

enum batt_rl_status {
	BATT_RL_STATUS_NONE = 0,
	BATT_RL_STATUS_DISCHARGE = -1,
	BATT_RL_STATUS_RECHARGE = 1,
};

#define RL_DELTA_SOC_MAX	8

struct batt_ssoc_rl_state {
	/* rate limiter state */
	qnum_t rl_ssoc_target;
	time_t rl_ssoc_last_update;

	/* rate limiter flags */
	bool rl_no_zero;
	int rl_fast_track;
	int rl_track_target;
	/* rate limiter config */
	int rl_delta_max_time;
	qnum_t rl_delta_max_soc;

	int rl_delta_soc_ratio[RL_DELTA_SOC_MAX];
	qnum_t rl_delta_soc_limit[RL_DELTA_SOC_MAX];
	int rl_delta_soc_cnt;

	qnum_t rl_ft_low_limit;
	qnum_t rl_ft_delta_limit;
};

#define SSOC_STATE_BUF_SZ 128

struct batt_ssoc_state {
	/* output of gauge data filter */
	qnum_t ssoc_gdf;
	/*  UI Curves */
	int ssoc_curve_type;    /*<0 dsg, >0 chg, 0? */
	struct ssoc_uicurve ssoc_curve[UICURVE_MAX];
	qnum_t ssoc_uic;
	/* output of rate limiter */
	qnum_t ssoc_rl;
	struct batt_ssoc_rl_state ssoc_rl_state;

	/* output of rate limiter */
	int rl_rate;
	int rl_last_ssoc;
	time_t rl_last_update;

	/* connected or disconnected */
	int buck_enabled;

	/* recharge logic */
	int rl_soc_threshold;
	enum batt_rl_status rl_status;

	/* buff */
	char ssoc_state_cstr[SSOC_STATE_BUF_SZ];
};

struct gbatt_ccbin_data {
	u16 count[GBMS_CCBIN_BUCKET_COUNT];
	char cyc_ctr_cstr[GBMS_CCBIN_CSTR_SIZE];
	struct mutex lock;
	int prev_soc;
};

#define DEFAULT_RES_TEMP_HIGH	390
#define DEFAULT_RES_TEMP_LOW	350
#define DEFAULT_RES_SSOC_THR	75
#define DEFAULT_RES_FILT_LEN	10

struct batt_res {
	bool estimate_requested;

	/* samples */
	int sample_accumulator;
	int sample_count;

	/* registers */
	int filter_count;
	int resistance_avg;

	/* configuration */
	int estimate_filter;
	int ssoc_threshold;
	int res_temp_low;
	int res_temp_high;
};

enum batt_paired_state {
	BATT_PAIRING_WRITE_ERROR = -4,
	BATT_PAIRING_READ_ERROR = -3,
	BATT_PAIRING_MISMATCH = -2,
	BATT_PAIRING_DISABLED = -1,
	BATT_PAIRING_ENABLED = 0,
	BATT_PAIRING_PAIRED = 1,
	BATT_PAIRING_RESET = 2,
};

enum batt_lfcollect_status {
	BATT_LFCOLLECT_NOT_AVAILABLE = -1,
	BATT_LFCOLLECT_DISABLED = 0,
	BATT_LFCOLLECT_ENABLED = 1,
	BATT_LFCOLLECT_COLLECT = 2,
};

/* battery driver state */
struct batt_drv {
	struct device *device;
	struct power_supply *psy;

	const char *fg_psy_name;
	struct power_supply *fg_psy;
	struct notifier_block fg_nb;

	struct delayed_work init_work;
	struct delayed_work batt_work;

	struct wakeup_source *msc_ws;
	struct wakeup_source *batt_ws;
	struct wakeup_source *taper_ws;
	struct wakeup_source *poll_ws;
	bool hold_taper_ws;

	/* TODO: b/111407333, will likely need to adjust SOC% on wakeup */
	bool init_complete;
	bool resume_complete;
	bool batt_present;

	struct mutex batt_lock;
	struct mutex chg_lock;

	/* battery work */
	int fg_status;
	int batt_fast_update_cnt;
	u32 batt_update_interval;
	/* update high temperature in time */
	int batt_temp;
	u32 batt_update_high_temp_threshold;
	/* fake battery temp for thermal testing */
	int fake_temp;
	/* triger for recharge logic next update from charger */
	bool batt_full;
	struct batt_ssoc_state ssoc_state;
	/* bin count */
	struct gbatt_ccbin_data cc_data;
	/* fg cycle count */
	int cycle_count;

	/* props */
	int soh;
	int fake_capacity;
	bool dead_battery;
	int capacity_level;
	bool chg_done;

	/* temp outside the charge table */
	int jeita_stop_charging;

	/* MSC charging */
	u32 battery_capacity;
	struct gbms_chg_profile chg_profile;
	union gbms_charger_state chg_state;

	int temp_idx;
	int vbatt_idx;
	int checked_cv_cnt;
	int checked_ov_cnt;
	int checked_tier_switch_cnt;

	int fv_uv;
	int cc_max;
	int msc_update_interval;

	bool disable_votes;
	struct votable	*msc_interval_votable;
	struct votable	*fcc_votable;
	struct votable	*fv_votable;

	/* stats */
	int msc_state;
	int msc_irdrop_state;
	struct mutex stats_lock;
	struct gbms_charging_event ce_data;
	struct gbms_charging_event ce_qual;

	/* time to full */
	struct batt_ttf_stats ttf_stats;

	/* logging */
	struct logbuffer *ttf_log;
	struct logbuffer *ssoc_log;

	/* thermal */
	struct thermal_zone_device *tz_dev;

	/* Resistance */
	struct batt_res res_state;

	/* used to detect battery replacements and reset statistics */
	enum batt_paired_state pairing_state;

	/* collect battery history/lifetime data (history) */
	enum batt_lfcollect_status blf_state;
	int hist_delta_cycle_cnt;
	int hist_data_max_cnt;
	void *hist_data;

	/* Battery device info */
	u8 dev_info_check[GBMS_DINF_LEN];

	/* History Device */
	struct gbms_storage_device *history;
};

static inline void batt_update_cycle_count(struct batt_drv *batt_drv)
{
	batt_drv->cycle_count = GPSY_GET_PROP(batt_drv->fg_psy,
					      POWER_SUPPLY_PROP_CYCLE_COUNT);
}

static int google_battery_tz_get_cycle_count(void *data, int *cycle_count)
{
	struct batt_drv *batt_drv = (struct batt_drv *)data;

	if (!cycle_count) {
		pr_err("Cycle Count NULL");
		return -EINVAL;
	}

	if (batt_drv->cycle_count < 0)
		return batt_drv->cycle_count;

	*cycle_count = batt_drv->cycle_count;

	return 0;
}

static int psy_changed(struct notifier_block *nb,
		       unsigned long action, void *data)
{
	struct power_supply *psy = data;
	struct batt_drv *batt_drv = container_of(nb, struct batt_drv, fg_nb);

	pr_debug("name=%s evt=%lu\n", psy->desc->name, action);

	if ((action != PSY_EVENT_PROP_CHANGED) ||
	    (psy == NULL) || (psy->desc == NULL) || (psy->desc->name == NULL))
		return NOTIFY_OK;

	if (action == PSY_EVENT_PROP_CHANGED &&
	    (!strcmp(psy->desc->name, batt_drv->fg_psy_name))) {
		mod_delayed_work(system_wq, &batt_drv->batt_work, 0);
	}

	return NOTIFY_OK;
}

/* ------------------------------------------------------------------------- */

#define SSOC_TRUE 15
#define SSOC_SPOOF 95
#define SSOC_FULL 100
#define UICURVE_BUF_SZ	(UICURVE_MAX * 15 + 1)

enum ssoc_uic_type {
	SSOC_UIC_TYPE_DSG  = -1,
	SSOC_UIC_TYPE_NONE = 0,
	SSOC_UIC_TYPE_CHG  = 1,
};

const qnum_t ssoc_point_true = qnum_rconst(SSOC_TRUE);
const qnum_t ssoc_point_spoof = qnum_rconst(SSOC_SPOOF);
const qnum_t ssoc_point_full = qnum_rconst(SSOC_FULL);

static struct ssoc_uicurve chg_curve[UICURVE_MAX] = {
	{ ssoc_point_true, ssoc_point_true },
	{ ssoc_point_spoof, ssoc_point_spoof },
	{ ssoc_point_full, ssoc_point_full },
};

static struct ssoc_uicurve dsg_curve[UICURVE_MAX] = {
	{ ssoc_point_true, ssoc_point_true },
	{ ssoc_point_spoof, ssoc_point_full },
	{ ssoc_point_full, ssoc_point_full },
};

static char *ssoc_uicurve_cstr(char *buff, size_t size,
			       struct ssoc_uicurve *curve)
{
	int i, len = 0;

	for (i = 0; i < UICURVE_MAX ; i++) {
		len += scnprintf(&buff[len], size - len,
				"[" QNUM_CSTR_FMT " " QNUM_CSTR_FMT "]",
				qnum_toint(curve[i].real),
				qnum_fracdgt(curve[i].real),
				qnum_toint(curve[i].ui),
				qnum_fracdgt(curve[i].ui));
		if (len >= size)
			break;
	}

	buff[len] = 0;
	return buff;
}

/* NOTE: no bounds checks on this one */
static int ssoc_uicurve_find(qnum_t real, struct ssoc_uicurve *curve)
{
	int i;

	for (i = 1; i < UICURVE_MAX ; i++) {
		if (real == curve[i].real)
			return i;
		if (real > curve[i].real)
			continue;
		break;
	}

	return i-1;
}

static qnum_t ssoc_uicurve_map(qnum_t real, struct ssoc_uicurve *curve)
{
	qnum_t slope = 0, delta_ui, delta_re;
	int i;

	if (real < curve[0].real)
		return real;
	if (real >= curve[UICURVE_MAX - 1].ui)
		return curve[UICURVE_MAX - 1].ui;

	i = ssoc_uicurve_find(real, curve);
	if (curve[i].real == real)
		return curve[i].ui;

	delta_ui = curve[i + 1].ui - curve[i].ui;
	delta_re =  curve[i + 1].real - curve[i].real;
	if (delta_re)
		slope = qnum_div(delta_ui, delta_re);

	return curve[i].ui + qnum_mul(slope, (real - curve[i].real));
}

/* "optimized" to work on 3 element curves */
static void ssoc_uicurve_splice(struct ssoc_uicurve *curve, qnum_t real,
				qnum_t ui)
{
	if (real < curve[0].real || real > curve[2].real)
		return;

#if UICURVE_MAX != 3
#error ssoc_uicurve_splice() only support UICURVE_MAX == 3
#endif

	/* splice only when real is within the curve range */
	curve[1].real = real;
	curve[1].ui = ui;
}

static void ssoc_uicurve_dup(struct ssoc_uicurve *dst,
			     struct ssoc_uicurve *curve)
{
	if (dst != curve)
		memcpy(dst, curve, sizeof(*dst)*UICURVE_MAX);
}


/* ------------------------------------------------------------------------- */

/* could also use the rate of change for this */
static qnum_t ssoc_rl_max_delta(const struct batt_ssoc_rl_state *rls,
				int bucken, time_t delta_time)
{
	int i;
	const qnum_t max_delta = (rls->rl_delta_max_soc * delta_time) /
				  rls->rl_delta_max_time;

	if (rls->rl_fast_track)
		return max_delta;

	/* might have one table for charging and one for discharging */
	for (i = 0; i < rls->rl_delta_soc_cnt; i++) {
		if (rls->rl_delta_soc_limit[i] == 0)
			break;

		if (rls->rl_ssoc_target < rls->rl_delta_soc_limit[i])
			return (max_delta * 10) /
				rls->rl_delta_soc_ratio[i];
	}

	return max_delta;
}

static qnum_t ssoc_apply_rl(struct batt_ssoc_state *ssoc)
{
	const time_t now = get_boot_sec();
	struct batt_ssoc_rl_state *rls = &ssoc->ssoc_rl_state;
	qnum_t rl_val;

	/* track ssoc_uic when buck is enabled or the minimum value of uic */
	if (ssoc->buck_enabled ||
	    (!ssoc->buck_enabled && ssoc->ssoc_uic < rls->rl_ssoc_target))
		rls->rl_ssoc_target = ssoc->ssoc_uic;

	/* sanity on the target */
	if (rls->rl_ssoc_target > qnum_fromint(100))
		rls->rl_ssoc_target = qnum_fromint(100);
	if (rls->rl_ssoc_target < qnum_fromint(0))
		rls->rl_ssoc_target = qnum_fromint(0);

	/* closely track target */
	if (rls->rl_track_target) {
		rl_val = rls->rl_ssoc_target;
	} else {
		qnum_t step;
		const time_t delta_time = now - rls->rl_ssoc_last_update;
		const time_t max_delta = ssoc_rl_max_delta(rls,
							   ssoc->buck_enabled,
							   delta_time);

		/* apply the rate limiter, delta_soc to target */
		step = rls->rl_ssoc_target - ssoc->ssoc_rl;
		if (step < -max_delta)
			step = -max_delta;
		else if (step > max_delta)
			step = max_delta;

		rl_val = ssoc->ssoc_rl + step;
	}

	/* do not increase when not connected */
	if (!ssoc->buck_enabled && rl_val > ssoc->ssoc_rl)
		rl_val = ssoc->ssoc_rl;

	/* will report 0% when rl_no_zero clears */
	if (rls->rl_no_zero && rl_val <= qnum_fromint(1))
		rl_val = qnum_fromint(1);

	rls->rl_ssoc_last_update = now;
	return rl_val;
}

/* ------------------------------------------------------------------------- */

/* a statement :-) */
static int ssoc_get_real(const struct batt_ssoc_state *ssoc)
{
	return qnum_toint(ssoc->ssoc_gdf);
}

static qnum_t ssoc_get_capacity_raw(const struct batt_ssoc_state *ssoc)
{
	return ssoc->ssoc_rl;
}

/* reported to userspace: call while holding batt_lock */
static int ssoc_get_capacity(const struct batt_ssoc_state *ssoc)
{
	const qnum_t raw = ssoc_get_capacity_raw(ssoc);
	return qnum_roundint(raw, 0.005);
}

/* ------------------------------------------------------------------------- */

void dump_ssoc_state(struct batt_ssoc_state *ssoc_state, struct logbuffer *log)
{
	char buff[UICURVE_BUF_SZ] = { 0 };

	scnprintf(ssoc_state->ssoc_state_cstr,
		  sizeof(ssoc_state->ssoc_state_cstr),
		  "SSOC: l=%d%% gdf=%d.%02d uic=%d.%02d rl=%d.%02d ct=%d curve:%s rls=%d",
		  ssoc_get_capacity(ssoc_state),
		  qnum_toint(ssoc_state->ssoc_gdf),
		  qnum_fracdgt(ssoc_state->ssoc_gdf),
		  qnum_toint(ssoc_state->ssoc_uic),
		  qnum_fracdgt(ssoc_state->ssoc_uic),
		  qnum_toint(ssoc_state->ssoc_rl),
		  qnum_fracdgt(ssoc_state->ssoc_rl),
		  ssoc_state->ssoc_curve_type,
		  ssoc_uicurve_cstr(buff, sizeof(buff), ssoc_state->ssoc_curve),
		  ssoc_state->rl_status);

	if (log) {
		logbuffer_log(log, "%s", ssoc_state->ssoc_state_cstr);
	} else {
		pr_info("%s\n", ssoc_state->ssoc_state_cstr);
	}
}

/* ------------------------------------------------------------------------- */

/* call while holding batt_lock */
static void ssoc_update(struct batt_ssoc_state *ssoc, qnum_t soc)
{
	struct batt_ssoc_rl_state *rls =  &ssoc->ssoc_rl_state;
	qnum_t delta;

	/* low pass filter */
	ssoc->ssoc_gdf = soc;
	/* spoof UI @ EOC */
	ssoc->ssoc_uic = ssoc_uicurve_map(ssoc->ssoc_gdf, ssoc->ssoc_curve);

	/* first target is current UIC */
	if (rls->rl_ssoc_target == -1) {
		rls->rl_ssoc_target = ssoc->ssoc_uic;
		ssoc->ssoc_rl = ssoc->ssoc_uic;
	}

	/* enable fast track when target under configured limit */
	rls->rl_fast_track |= rls->rl_ssoc_target < rls->rl_ft_low_limit;

	/*
	 * delta fast tracking during charge
	 * NOTE: might use the stats from TTF to determine the maximum rate
	 */
	delta = rls->rl_ssoc_target - ssoc->ssoc_rl;
	if (rls->rl_ft_delta_limit && ssoc->buck_enabled && delta > 0) {
		/* only when SOC increase */
		rls->rl_fast_track |= delta > rls->rl_ft_delta_limit;
	} else if (rls->rl_ft_delta_limit && !ssoc->buck_enabled && delta < 0) {
		/* enable fast track when target under configured limit */
		rls->rl_fast_track |= -delta > rls->rl_ft_delta_limit;
	}

	/*
	 * Right now a simple test on target metric falling under 0.5%
	 * TODO: add a filter that decrements no_zero when a specific
	 * condition is met (ex rl_ssoc_target < 1%).
	 */
	if (rls->rl_no_zero)
		rls->rl_no_zero = rls->rl_ssoc_target > qnum_from_q8_8(128);

	/*  monotonicity and rate of change */
	ssoc->ssoc_rl = ssoc_apply_rl(ssoc);
}

/*
 * Maxim could need:
 *	1fh AvCap, 10h FullCap. 23h FullCapNom
 * QC could need:
 *	QG_CC_SOC, QG_Raw_SOC, QG_Bat_SOC, QG_Sys_SOC, QG_Mon_SOC
 */
static int ssoc_work(struct batt_ssoc_state *ssoc_state,
		     struct power_supply *fg_psy)
{
	int soc_q8_8;
	qnum_t soc_raw;

	/*
	 * TODO: POWER_SUPPLY_PROP_CAPACITY_RAW should return a qnum_t
	 * TODO: add an array here configured in DT with the properties
	 * to query and their weights, make soc_raw come from fusion.
	 */
	soc_q8_8 = GPSY_GET_PROP(fg_psy, POWER_SUPPLY_PROP_CAPACITY_RAW);
	if (soc_q8_8 < 0)
		return -EINVAL;

	/*
	 * soc_raw can come from fusion:
	 *    soc_raw = m1 * w1 + m2 * w2 + ...
	 *
	 * where m1, m2 are gauge metrics, w1,w1 are weights that change
	 * with temperature, state of charge, battery health etc.
	 */
	soc_raw = qnum_from_q8_8(soc_q8_8);

	ssoc_update(ssoc_state, soc_raw);
	return 0;
}

/*
 * Called on connect and disconnect to adjust the UI curve. Splice at GDF less
 * a fixed delta while UI is at 100% (i.e. in RL) to avoid showing 100% for
 * "too long" after disconnect.
 */
#define SSOC_DELTA 3
void ssoc_change_curve(struct batt_ssoc_state *ssoc_state,
		       enum ssoc_uic_type type)
{
	struct ssoc_uicurve *new_curve;
	qnum_t gdf = ssoc_state->ssoc_gdf; /* actual battery level */
	const qnum_t ssoc_level = ssoc_get_capacity(ssoc_state);

	/* force dsg curve when connect/disconnect with battery at 100% */
	if (ssoc_level >= SSOC_FULL) {
		const qnum_t rlt = qnum_fromint(ssoc_state->rl_soc_threshold);

		gdf -=  qnum_rconst(SSOC_DELTA);
		if (gdf > rlt)
			gdf = rlt;
		type = SSOC_UIC_TYPE_DSG;
	}

	new_curve = (type == SSOC_UIC_TYPE_DSG) ? dsg_curve : chg_curve;
	ssoc_uicurve_dup(ssoc_state->ssoc_curve, new_curve);
	ssoc_state->ssoc_curve_type = type;

	/* splice at (->ssoc_gdf,->ssoc_rl) because past spoof */
	ssoc_uicurve_splice(ssoc_state->ssoc_curve,
			    gdf,
			    ssoc_get_capacity_raw(ssoc_state));
}

/* ------------------------------------------------------------------------- */

/*
 * enter recharge logic in BATT_RL_STATUS_DISCHARGE on charger_DONE,
 * enter BATT_RL_STATUS_RECHARGE on Fuel Gauge FULL
 * NOTE: batt_rl_update_status() doesn't call this, it flip from DISCHARGE
 * to recharge on its own.
 * NOTE: call holding chg_lock
 * @pre rl_status != BATT_RL_STATUS_NONE
 */
static bool batt_rl_enter(struct batt_ssoc_state *ssoc_state,
			  enum batt_rl_status rl_status)
{
	const int rl_current = ssoc_state->rl_status;

	/*
	 * NOTE: NO_OP when RL=DISCHARGE since batt_rl_update_status() flip
	 * between BATT_RL_STATUS_DISCHARGE and BATT_RL_STATUS_RECHARGE
	 * directly.
	 */
	if (rl_current == rl_status || rl_current == BATT_RL_STATUS_DISCHARGE)
		return false;

	/*
	 * NOTE: rl_status transition from *->DISCHARGE on charger FULL (during
	 * charge or at the end of recharge) and transition from
	 * NONE->RECHARGE when battery is full (SOC==100%) before charger is.
	 */
	if (rl_status == BATT_RL_STATUS_DISCHARGE) {
		ssoc_uicurve_dup(ssoc_state->ssoc_curve, dsg_curve);
		ssoc_state->ssoc_curve_type = SSOC_UIC_TYPE_DSG;
	}

	ssoc_update(ssoc_state, ssoc_state->ssoc_gdf);
	ssoc_state->rl_status = rl_status;

	return true;
}

static int ssoc_rl_read_dt(struct batt_ssoc_rl_state *rls,
			   struct device_node *node)
{
	u32 tmp, delta_soc[RL_DELTA_SOC_MAX];
	int ret, i;

	ret = of_property_read_u32(node, "google,rl_delta-max-soc", &tmp);
	if (ret == 0)
		rls->rl_delta_max_soc = qnum_fromint(tmp);

	ret = of_property_read_u32(node, "google,rl_delta-max-time", &tmp);
	if (ret == 0)
		rls->rl_delta_max_time = tmp;

	if (!rls->rl_delta_max_soc || !rls->rl_delta_max_time)
		return -EINVAL;

	rls->rl_no_zero = of_property_read_bool(node, "google,rl_no-zero");
	rls->rl_track_target = of_property_read_bool(node,
						     "google,rl_track-target");

	ret = of_property_read_u32(node, "google,rl_ft-low-limit", &tmp);
	if (ret == 0)
		rls->rl_ft_low_limit = qnum_fromint(tmp);

	ret = of_property_read_u32(node, "google,rl_ft-delta-limit", &tmp);
	if (ret == 0)
		rls->rl_ft_delta_limit = qnum_fromint(tmp);

	rls->rl_delta_soc_cnt = of_property_count_elems_of_size(node,
					      "google,rl_soc-limits",
					      sizeof(u32));
	tmp = of_property_count_elems_of_size(node, "google,rl_soc-rates",
					      sizeof(u32));
	if (rls->rl_delta_soc_cnt != tmp || tmp == 0) {
		rls->rl_delta_soc_cnt = 0;
		goto done;
	}

	if (rls->rl_delta_soc_cnt > RL_DELTA_SOC_MAX)
		return -EINVAL;

	ret = of_property_read_u32_array(node, "google,rl_soc-limits",
					 delta_soc,
					 rls->rl_delta_soc_cnt);
	if (ret < 0)
		return ret;

	for (i = 0; i < rls->rl_delta_soc_cnt; i++)
		rls->rl_delta_soc_limit[i] = qnum_fromint(delta_soc[i]);

	ret = of_property_read_u32_array(node, "google,rl_soc-rates",
					 delta_soc,
					 rls->rl_delta_soc_cnt);
	if (ret < 0)
		return ret;

	for (i = 0; i < rls->rl_delta_soc_cnt; i++)
		rls->rl_delta_soc_ratio[i] = delta_soc[i];

done:
	return 0;
}


/*
 * NOTE: might need to use SOC from bootloader as starting point to avoid UI
 * SSOC jumping around or taking long time to coverge. Could technically read
 * charger voltage and estimate SOC% based on empty and full voltage.
 */
static int ssoc_init(struct batt_ssoc_state *ssoc_state,
		     struct device_node *node,
		     struct power_supply *fg_psy)
{
	int ret, capacity;

	ret = ssoc_rl_read_dt(&ssoc_state->ssoc_rl_state, node);
	if (ret < 0)
		ssoc_state->ssoc_rl_state.rl_track_target = 1;
	ssoc_state->ssoc_rl_state.rl_ssoc_target = -1;

	/*
	 * ssoc_work() needs a curve: start with the charge curve to prevent
	 * SSOC% from increasing after a reboot. Curve type must be NONE until
	 * battery knows the charger BUCK_EN state.
	 */
	ssoc_uicurve_dup(ssoc_state->ssoc_curve, chg_curve);
	ssoc_state->ssoc_curve_type = SSOC_UIC_TYPE_NONE;

	ret = ssoc_work(ssoc_state, fg_psy);
	if (ret < 0)
		return -EIO;

	capacity = ssoc_get_capacity(ssoc_state);
	if (capacity >= SSOC_FULL) {
		/* consistent behavior when booting without adapter */
		ssoc_uicurve_dup(ssoc_state->ssoc_curve, dsg_curve);
	} else if (capacity < SSOC_TRUE) {
		/* no split */
	} else if (capacity < SSOC_SPOOF) {
		/* mark the initial point if under spoof */
		ssoc_uicurve_splice(ssoc_state->ssoc_curve,
						ssoc_state->ssoc_gdf,
						ssoc_state->ssoc_rl);

	}

	return 0;
}

/* ------------------------------------------------------------------------- */

/*
 * just reset state, no PS notifications no changes in the UI curve. This is
 * called on startup and on disconnect when the charge driver state is reset
 * NOTE: call holding chg_lock
 */
static void batt_rl_reset(struct batt_drv *batt_drv)
{
	batt_drv->ssoc_state.rl_status = BATT_RL_STATUS_NONE;
}

/* RL recharge: after SSOC work, restart charging.
 * NOTE: call holding chg_lock
 */
static void batt_rl_update_status(struct batt_drv *batt_drv)
{
	struct batt_ssoc_state *ssoc_state = &batt_drv->ssoc_state;
	int soc;

	/* already in _RECHARGE or _NONE, done */
	if (ssoc_state->rl_status != BATT_RL_STATUS_DISCHARGE)
		return;

	/* recharge logic work on real soc */
	soc = ssoc_get_real(ssoc_state);
	if (ssoc_state->rl_soc_threshold &&
	    soc <= ssoc_state->rl_soc_threshold) {

		/* change state (will restart charge) on trigger */
		ssoc_state->rl_status = BATT_RL_STATUS_RECHARGE;
		if (batt_drv->psy)
			power_supply_changed(batt_drv->psy);
	}

}

/* ------------------------------------------------------------------------- */

static int batt_ttf_estimate(time_t *res, const struct batt_drv *batt_drv)
{
	int rc;
	time_t estimate = batt_drv->ttf_stats.ttf_fake;

	if (batt_drv->ssoc_state.buck_enabled != 1)
		return -EINVAL;

	if (batt_drv->ttf_stats.ttf_fake != -1)
		goto done;

	rc = ttf_soc_estimate(&estimate, &batt_drv->ttf_stats,
			      &batt_drv->ce_data,
			      ssoc_get_capacity_raw(&batt_drv->ssoc_state),
			      ssoc_point_full);
	if (rc < 0)
		estimate = -1;

done:
	*res = estimate;
	return 0;
}

/* ------------------------------------------------------------------------- */

static void batt_chg_stats_init(struct gbms_charging_event *ce_data,
				const struct gbms_chg_profile *profile)
{
	int i;

	memset(ce_data, 0, sizeof(*ce_data));

	ce_data->chg_profile = profile;
	ce_data->charging_stats.voltage_in = -1;
	ce_data->charging_stats.ssoc_in = -1;
	ce_data->charging_stats.voltage_out = -1;
	ce_data->charging_stats.ssoc_out = -1;

	ttf_soc_init(&ce_data->soc_stats);
	ce_data->last_soc = -1;

	for (i = 0; i < GBMS_STATS_TIER_COUNT ; i++) {
		ce_data->tier_stats[i].temp_idx = -1;
		ce_data->tier_stats[i].vtier_idx = i;
		ce_data->tier_stats[i].soc_in = -1;
	}

}

static void batt_chg_stats_start(struct batt_drv *batt_drv)
{
	union gbms_ce_adapter_details ad;
	struct gbms_charging_event *ce_data = &batt_drv->ce_data;
	const time_t now = get_boot_sec();
	int vin, cc_in;

	mutex_lock(&batt_drv->stats_lock);
	ad.v = batt_drv->ce_data.adapter_details.v;
	batt_chg_stats_init(ce_data, &batt_drv->chg_profile);
	batt_drv->ce_data.adapter_details.v = ad.v;

	vin = GPSY_GET_PROP(batt_drv->fg_psy,
				POWER_SUPPLY_PROP_VOLTAGE_NOW);
	ce_data->charging_stats.voltage_in = (vin < 0) ? -1 : vin / 1000;
	ce_data->charging_stats.ssoc_in =
				ssoc_get_capacity(&batt_drv->ssoc_state);
	cc_in = GPSY_GET_PROP(batt_drv->fg_psy,
				POWER_SUPPLY_PROP_CHARGE_COUNTER);
	ce_data->charging_stats.cc_in = (cc_in < 0) ? -1 : cc_in / 1000;

	ce_data->charging_stats.ssoc_out = -1;
	ce_data->charging_stats.voltage_out = -1;

	ce_data->first_update = now;
	ce_data->last_update = now;

	mutex_unlock(&batt_drv->stats_lock);
}

/* call holding stats_lock */
static bool batt_chg_stats_qual(const struct gbms_charging_event *ce_data)
{
	const long elap = ce_data->last_update - ce_data->first_update;
	const long ssoc_delta = ce_data->charging_stats.ssoc_out -
				ce_data->charging_stats.ssoc_in;

	return elap >= ce_data->chg_sts_qual_time ||
	    ssoc_delta >= ce_data->chg_sts_delta_soc;
}

/* call holding stats_lock */
static void batt_chg_stats_tier(struct gbms_ce_tier_stats *tier,
				int msc_state,
				time_t elap)
{
	if (msc_state < 0 || msc_state >= MSC_STATES_COUNT)
		return;

	tier->msc_cnt[msc_state] += 1;
	tier->msc_elap[msc_state] += elap;
}

/* call holding stats_lock */
static void batt_chg_stats_soc_update(struct gbms_charging_event *ce_data,
				      qnum_t soc, time_t elap, int tier_index,
				      int cc)
{
	int index;
	const int last_soc = ce_data->last_soc;

	index = qnum_toint(soc);
	if (index < 0)
		index = 0;
	if (index > 100)
		index = 100;
	if (index < last_soc)
		return;

	if (ce_data->soc_stats.elap[index] == 0) {
		ce_data->soc_stats.ti[index] = tier_index;
		ce_data->soc_stats.cc[index] = cc;
	}

	if (last_soc != -1)
		ce_data->soc_stats.elap[last_soc] += elap;

	ce_data->last_soc = index;
}

/* call holding stats_lock */
static void batt_chg_stats_update(struct batt_drv *batt_drv,
				  int temp_idx, int tier_idx,
				  int ibatt_ma, int temp, time_t elap)
{
	int cc;
	const int msc_state = batt_drv->msc_state;
	const uint16_t icl_settled = batt_drv->chg_state.f.icl;
	struct gbms_ce_tier_stats *tier =
				&batt_drv->ce_data.tier_stats[tier_idx];

	/* TODO: read at start of tier and update cc_total of previous */
	cc = GPSY_GET_PROP(batt_drv->fg_psy,
				POWER_SUPPLY_PROP_CHARGE_COUNTER);
	if (cc < 0) {
		pr_info("MSC_STAT cannot read cc=%d\n", cc);
		return;
	}
	cc = cc / 1000;

	/* book to previous soc unless discharging */
	if (msc_state != MSC_DSG) {
		qnum_t soc = ssoc_get_capacity_raw(&batt_drv->ssoc_state);

		/* TODO: should I use ssoc instead? */
		batt_chg_stats_soc_update(&batt_drv->ce_data, soc, elap,
					  tier_idx, cc);
	}

	/* book to previous state */
	batt_chg_stats_tier(tier, msc_state, elap);

	if (tier->soc_in == -1) {
		int soc_in;

		soc_in = GPSY_GET_PROP(batt_drv->fg_psy,
				       POWER_SUPPLY_PROP_CAPACITY_RAW);
		if (soc_in < 0) {
			pr_info("MSC_STAT cannot read soc_in=%d\n", soc_in);
			return;
		}

		tier->temp_idx = temp_idx;

		tier->temp_in = temp;
		tier->temp_min = temp;
		tier->temp_max = temp;

		tier->ibatt_min = ibatt_ma;
		tier->ibatt_max = ibatt_ma;

		tier->icl_min = icl_settled;
		tier->icl_max = icl_settled;

		tier->soc_in = soc_in;
		tier->cc_in = cc;
		tier->cc_total = 0;
	} else {
		const u8 flags = batt_drv->chg_state.f.flags;

		/* crossed temperature tier */
		if (temp_idx != tier->temp_idx)
			tier->temp_idx = -1;

		if (flags & GBMS_CS_FLAG_CC) {
			tier->time_fast += elap;
		} else if (flags & GBMS_CS_FLAG_CV) {
			tier->time_taper += elap;
		} else {
			tier->time_other += elap;
		}

		/*
		 * averages: temp < 100. icl_settled < 3000, sum(ibatt)
		 * is bound to battery capacity, elap in seconds, sums
		 * are stored in an s64. For icl_settled I need a tier
		 * to last for more than ~97M years.
		 */
		if (temp < tier->temp_min)
			tier->temp_min = temp;
		if (temp > tier->temp_max)
			tier->temp_max = temp;
		tier->temp_sum += temp * elap;

		if (icl_settled < tier->icl_min)
			tier->icl_min = icl_settled;
		if (icl_settled > tier->icl_max)
			tier->icl_max = icl_settled;
		tier->icl_sum += icl_settled * elap;

		if (ibatt_ma < tier->ibatt_min)
			tier->ibatt_min = ibatt_ma;
		if (ibatt_ma > tier->ibatt_max)
			tier->ibatt_max = ibatt_ma;
		tier->ibatt_sum += ibatt_ma * elap;

		tier->cc_total = cc - tier->cc_in;
	}

	tier->sample_count += 1;
}

/* Only the qualified copy gets the timestamp and the exit voltage. */
static bool batt_chg_stats_close(struct batt_drv *batt_drv,
				 char *reason,
				 bool force)
{
	bool publish;
	const int vout = GPSY_GET_PROP(batt_drv->fg_psy,
				POWER_SUPPLY_PROP_VOLTAGE_NOW);
	const int cc_out = GPSY_GET_PROP(batt_drv->fg_psy,
				POWER_SUPPLY_PROP_CHARGE_COUNTER);

	/* book last period to the current tier
	 * NOTE: vbatt_idx != -1 -> temp_idx != -1
	 */
	if (batt_drv->vbatt_idx != -1 && batt_drv->temp_idx != -1) {
		const time_t now = get_boot_sec();
		const time_t elap = now - batt_drv->ce_data.last_update;

		batt_chg_stats_update(batt_drv,
				      batt_drv->temp_idx, batt_drv->vbatt_idx,
				      0, 0, elap);
		batt_drv->ce_data.last_update = now;
	}

	/* record the closing in data (and qual) */
	batt_drv->ce_data.charging_stats.voltage_out =
				(vout < 0) ? -1 : vout / 1000;
	batt_drv->ce_data.charging_stats.ssoc_out =
				ssoc_get_capacity(&batt_drv->ssoc_state);
	batt_drv->ce_data.charging_stats.cc_out =
				(cc_out < 0) ? -1 : cc_out / 1000;

	/* TODO: add a field to ce_data to qual weird charge sessions */
	publish = force || batt_chg_stats_qual(&batt_drv->ce_data);
	if (publish) {
		struct gbms_charging_event *ce_qual = &batt_drv->ce_qual;

		memcpy(ce_qual, &batt_drv->ce_data, sizeof(*ce_qual));

		pr_info("MSC_STAT %s: elap=%ld ssoc=%d->%d v=%d->%d c=%d->%d\n",
			reason,
			ce_qual->last_update - ce_qual->first_update,
			ce_qual->charging_stats.ssoc_in,
			ce_qual->charging_stats.ssoc_out,
			ce_qual->charging_stats.voltage_in,
			ce_qual->charging_stats.voltage_out,
			ce_qual->charging_stats.cc_in,
			ce_qual->charging_stats.cc_out);
	}

	return publish;
}

static void batt_chg_stats_pub(struct batt_drv *batt_drv,
			       char *reason,
			       bool force)
{
	bool publish;

	mutex_lock(&batt_drv->stats_lock);
	publish = batt_chg_stats_close(batt_drv, reason, force);
	if (publish) {
		ttf_stats_update(&batt_drv->ttf_stats,
				 &batt_drv->ce_qual, false);

		kobject_uevent(&batt_drv->device->kobj, KOBJ_CHANGE);
	}
	mutex_unlock(&batt_drv->stats_lock);
}


static int batt_chg_stats_soc_next(const struct gbms_charging_event *ce_data,
				   int i)
{
	int soc_next;

	if (i == GBMS_STATS_TIER_COUNT -1)
		return ce_data->last_soc;

	soc_next = ce_data->tier_stats[i + 1].soc_in >> 8;
	if (soc_next <= 0)
		return ce_data->last_soc;

	return soc_next;
}

static int batt_chg_stats_cstr(char *buff, int size,
			       const struct gbms_charging_event *ce_data,
			       bool verbose)
{
	int i, j, len = 0;
	static char *codes[] = { "n", "s", "d", "l", "v", "vo", "p", "f", "t",
				"dl", "st", "tc", "r", "w", "rs", "n", "ny" };

	if (verbose) {
		const char *adapter_name =
			gbms_chg_ev_adapter_s(ce_data->adapter_details.ad_type);

		len += scnprintf(&buff[len], size - len, "A: %s,",
				adapter_name);
	}

	len += scnprintf(&buff[len], size - len, "%d,%d,%d",
				ce_data->adapter_details.ad_type,
				ce_data->adapter_details.ad_voltage * 100,
				ce_data->adapter_details.ad_amperage * 100);

	len += scnprintf(&buff[len], size - len, "%s%hu,%hu, %hu,%hu",
				(verbose) ?  "\nS: " : ", ",
				ce_data->charging_stats.ssoc_in,
				ce_data->charging_stats.voltage_in,
				ce_data->charging_stats.ssoc_out,
				ce_data->charging_stats.voltage_out);


	if (verbose) {
		len += scnprintf(&buff[len], size - len, " %hu,%hu",
				ce_data->charging_stats.cc_in,
				ce_data->charging_stats.cc_out);

		len += scnprintf(&buff[len], size - len, " %ld,%ld",
				ce_data->first_update,
				ce_data->last_update);
	}

	for (i = 0; i < GBMS_STATS_TIER_COUNT; i++) {
		const int soc_next = batt_chg_stats_soc_next(ce_data, i);
		const int soc_in = ce_data->tier_stats[i].soc_in >> 8;
		const long elap = ce_data->tier_stats[i].time_fast +
				  ce_data->tier_stats[i].time_taper +
				  ce_data->tier_stats[i].time_other;

		if (!elap)
			continue;

		len += scnprintf(&buff[len], size - len, "\n%d%c ",
			ce_data->tier_stats[i].vtier_idx,
			(verbose) ? ':' : ',');

		len += scnprintf(&buff[len], size - len,
			"%d.%d,%d,%d, %d,%d,%d, %d,%ld,%d, %d,%ld,%d, %d,%ld,%d",
				soc_in,
				ce_data->tier_stats[i].soc_in & 0xff,
				ce_data->tier_stats[i].cc_in,
				ce_data->tier_stats[i].temp_in,
				ce_data->tier_stats[i].time_fast,
				ce_data->tier_stats[i].time_taper,
				ce_data->tier_stats[i].time_other,
				ce_data->tier_stats[i].temp_min,
				ce_data->tier_stats[i].temp_sum / elap,
				ce_data->tier_stats[i].temp_max,
				ce_data->tier_stats[i].ibatt_min,
				ce_data->tier_stats[i].ibatt_sum / elap,
				ce_data->tier_stats[i].ibatt_max,
				ce_data->tier_stats[i].icl_min,
				ce_data->tier_stats[i].icl_sum / elap,
				ce_data->tier_stats[i].icl_max);

		if (!verbose)
			continue;

		/* time spent in every multi step charging state */
		len += scnprintf(&buff[len], size - len, "\n%d:",
				ce_data->tier_stats[i].vtier_idx);

		for (j = 0; j < MSC_STATES_COUNT; j++)
			len += scnprintf(&buff[len], size - len, " %s=%d",
				codes[j], ce_data->tier_stats[i].msc_elap[j]);

		/* count spent in each step charging state */
		len += scnprintf(&buff[len], size - len, "\n%d:",
				ce_data->tier_stats[i].vtier_idx);

		for (j = 0; j < MSC_STATES_COUNT; j++)
			len += scnprintf(&buff[len], size - len, " %s=%d",
				codes[j], ce_data->tier_stats[i].msc_cnt[j]);

		if (soc_next) {
			len += scnprintf(&buff[len], size - len, "\n");
			len += ttf_soc_cstr(&buff[len], size - len,
					&ce_data->soc_stats,
					soc_in, soc_next);
		}
	}

	len += scnprintf(&buff[len], size - len, "\n");

	return len;
}

/* ------------------------------------------------------------------------- */

static void batt_res_dump_logs(struct batt_res *rstate)
{
	pr_info("RES: req:%d, sample:%d[%d], filt_cnt:%d, res_avg:%d\n",
		rstate->estimate_requested, rstate->sample_accumulator,
		rstate->sample_count, rstate->filter_count,
		rstate->resistance_avg);
}

static void batt_res_state_set(struct batt_res *rstate, bool breq)
{
	rstate->estimate_requested = breq;
	rstate->sample_accumulator = 0;
	rstate->sample_count = 0;
	batt_res_dump_logs(rstate);
}

static void batt_res_store_data(struct batt_res *rstate,
				struct power_supply *fg_psy)
{
	int ret = 0;
	int filter_estimate = 0;
	int total_estimate = 0;
	long new_estimate = 0;
	union power_supply_propval val;

	new_estimate = rstate->sample_accumulator / rstate->sample_count;
	filter_estimate = rstate->resistance_avg * rstate->filter_count;

	rstate->filter_count++;
	if (rstate->filter_count > rstate->estimate_filter) {
		rstate->filter_count = rstate->estimate_filter;
		filter_estimate -= rstate->resistance_avg;
	}
	total_estimate = filter_estimate + new_estimate;
	rstate->resistance_avg = total_estimate / rstate->filter_count;

	/* Save to NVRam*/
	val.intval = rstate->resistance_avg;
	ret = power_supply_set_property(fg_psy,
					POWER_SUPPLY_PROP_RESISTANCE_AVG,
					&val);
	if (ret < 0)
		pr_err("failed to write resistance_avg\n");

	val.intval = rstate->filter_count;
	ret = power_supply_set_property(fg_psy,
					POWER_SUPPLY_PROP_RES_FILTER_COUNT,
					&val);
	if (ret < 0)
		pr_err("failed to write resistenace filt_count\n");

	batt_res_dump_logs(rstate);
}

static int batt_res_load_data(struct batt_res *rstate,
			      struct power_supply *fg_psy)
{
	union power_supply_propval val;
	int ret = 0;

	ret = power_supply_get_property(fg_psy,
					POWER_SUPPLY_PROP_RESISTANCE_AVG,
					&val);
	if (ret < 0) {
		pr_err("failed to get resistance_avg(%d)\n", ret);
		return ret;
	}
	rstate->resistance_avg = val.intval;

	ret = power_supply_get_property(fg_psy,
					POWER_SUPPLY_PROP_RES_FILTER_COUNT,
					&val);
	if (ret < 0) {
		rstate->resistance_avg = 0;
		pr_err("failed to get resistance filt_count(%d)\n", ret);
		return ret;
	}
	rstate->filter_count = val.intval;

	batt_res_dump_logs(rstate);
	return 0;
}

static void batt_res_work(struct batt_drv *batt_drv)
{
	int temp, ret, resistance;
	struct batt_res *rstate = &batt_drv->res_state;
	const int ssoc_threshold = rstate->ssoc_threshold;
	const int res_temp_low = rstate->res_temp_low;
	const int res_temp_high = rstate->res_temp_high;

	temp = GPSY_GET_INT_PROP(batt_drv->fg_psy,
				 POWER_SUPPLY_PROP_TEMP, &ret);
	if (ret < 0 || temp < res_temp_low || temp > res_temp_high) {
		if (ssoc_get_real(&batt_drv->ssoc_state) > ssoc_threshold) {
			if (rstate->sample_count > 0) {
				/* update the filter */
				batt_res_store_data(&batt_drv->res_state,
						    batt_drv->fg_psy);
				batt_res_state_set(rstate, false);
			}
		}
		return;
	}

	resistance = GPSY_GET_INT_PROP(batt_drv->fg_psy,
				POWER_SUPPLY_PROP_RESISTANCE, &ret);
	if (ret < 0)
		return;

	if (ssoc_get_real(&batt_drv->ssoc_state) < ssoc_threshold) {
		rstate->sample_accumulator += resistance / 100;
		rstate->sample_count++;
		batt_res_dump_logs(rstate);
	} else {
		if (rstate->sample_count > 0) {
			/* update the filter here */
			batt_res_store_data(&batt_drv->res_state,
					    batt_drv->fg_psy);
		}
		batt_res_state_set(rstate, false);
	}
}

/* ------------------------------------------------------------------------- */

/* should not reset rl state */
static inline void batt_reset_chg_drv_state(struct batt_drv *batt_drv)
{
	/* the wake assertion will be released on disconnect and on SW JEITA */
	if (batt_drv->hold_taper_ws) {
		batt_drv->hold_taper_ws = false;
		__pm_relax(batt_drv->taper_ws);
	}

	/* polling */
	batt_drv->batt_fast_update_cnt = 0;
	batt_drv->fg_status = POWER_SUPPLY_STATUS_UNKNOWN;
	batt_drv->chg_done = false;
	/* algo */
	batt_drv->temp_idx = -1;
	batt_drv->vbatt_idx = -1;
	batt_drv->fv_uv = -1;
	batt_drv->cc_max = -1;
	batt_drv->msc_update_interval = -1;
	batt_drv->jeita_stop_charging = -1;
	/* timers */
	batt_drv->checked_cv_cnt = 0;
	batt_drv->checked_ov_cnt = 0;
	batt_drv->checked_tier_switch_cnt = 0;
	/* stats */
	batt_drv->msc_state = -1;
}

/*
 * software JEITA, disable charging when outside the charge table.
 * NOTE: ->jeita_stop_charging is either -1 (init or disable) or 0
 * TODO: need to be able to disable (leave to HW)
 */
static bool msc_logic_soft_jeita(const struct batt_drv *batt_drv, int temp)
{
	const struct gbms_chg_profile *profile = &batt_drv->chg_profile;

	if (temp < profile->temp_limits[0] ||
	    temp > profile->temp_limits[profile->temp_nb_limits - 1]) {
		if (batt_drv->jeita_stop_charging < 0) {
			pr_info("MSC_JEITA temp=%d off limits, do not enable charging\n",
				temp);
		} else if (batt_drv->jeita_stop_charging == 0) {
			pr_info("MSC_JEITA temp=%d off limits, disabling charging\n",
				temp);
		}

		return true;
	}

	return false;
}

/* TODO: only change batt_drv->checked_ov_cnt, an */
static int msc_logic_irdrop(struct batt_drv *batt_drv,
			    int vbatt, int ibatt, int temp_idx,
			    int *vbatt_idx, int *fv_uv,
			    int *update_interval)
{
	int msc_state = MSC_NONE;
	const bool match_enable = batt_drv->chg_state.f.vchrg != 0;
	const struct gbms_chg_profile *profile = &batt_drv->chg_profile;
	const int vtier = profile->volt_limits[*vbatt_idx];
	const int chg_type = batt_drv->chg_state.f.chg_type;
	const int utv_margin = profile->cv_range_accuracy;
	const int otv_margin = profile->cv_otv_margin;
	const int switch_cnt = profile->cv_tier_switch_cnt;

	if ((vbatt - vtier) > otv_margin) {
		/* OVER: vbatt over vtier for more than margin */
		const int cc_max = GBMS_CCCM_LIMITS(profile, temp_idx,
						    *vbatt_idx);

		/*
		 * pullback when over tier voltage, fast poll, penalty
		 * on TAPER_RAISE and no cv debounce (so will consider
		 * switching voltage tiers if the current is right).
		 * NOTE: lowering voltage might cause a small drop in
		 * current (we should remain  under next tier)
		 */
		*fv_uv = gbms_msc_round_fv_uv(profile, vtier,
			*fv_uv - profile->fv_uv_resolution);
		if (*fv_uv < vtier)
			*fv_uv = vtier;

		*update_interval = profile->cv_update_interval;
		batt_drv->checked_ov_cnt = profile->cv_tier_ov_cnt;
		batt_drv->checked_cv_cnt = 0;

		if (batt_drv->checked_tier_switch_cnt > 0 || !match_enable) {
			/* no pullback, next tier if already counting */
			msc_state = MSC_VSWITCH;
			*vbatt_idx = batt_drv->vbatt_idx + 1;

			pr_info("MSC_VSWITCH vt=%d vb=%d ibatt=%d\n",
				vtier, vbatt, ibatt);
		} else if (-ibatt == cc_max) {
			/* pullback, double penalty if at full current */
			msc_state = MSC_VOVER;
			batt_drv->checked_ov_cnt *= 2;

			pr_info("MSC_VOVER vt=%d  vb=%d ibatt=%d fv_uv=%d->%d\n",
				vtier, vbatt, ibatt,
				batt_drv->fv_uv, *fv_uv);
		} else {
			msc_state = MSC_PULLBACK;
			pr_info("MSC_PULLBACK vt=%d vb=%d ibatt=%d fv_uv=%d->%d\n",
				vtier, vbatt, ibatt,
				batt_drv->fv_uv, *fv_uv);
		}

		/*
		 * might get here after windup because algo will track the
		 * voltage drop caused from load as IRDROP.
		 * TODO: make sure that being current limited clear
		 * the taper condition.
		 */

	} else if (chg_type == POWER_SUPPLY_CHARGE_TYPE_FAST) {
		/*
		 * FAST: usual compensation (vchrg is vqcom)
		 * NOTE: there is a race in reading from charger and
		 * data might not be consistent (b/110318684)
		 * NOTE: could add PID loop for management of thermals
		 */
		const int vchrg = batt_drv->chg_state.f.vchrg * 1000;

		msc_state = MSC_FAST;

		/* invalid or 0 vchg disable IDROP compensation */
		if (vchrg <= 0) {
			/* could keep it steady instead */
			*fv_uv = vtier;
		} else if (vchrg > vbatt) {
			*fv_uv = gbms_msc_round_fv_uv(profile, vtier,
				vtier + (vchrg - vbatt));
		}

		/* no tier switch during fast charge */
		if (batt_drv->checked_cv_cnt == 0)
			batt_drv->checked_cv_cnt = 1;

		pr_info("MSC_FAST vt=%d vb=%d fv_uv=%d->%d vchrg=%d cv_cnt=%d\n",
			vtier, vbatt, batt_drv->fv_uv, *fv_uv,
			batt_drv->chg_state.f.vchrg,
			batt_drv->checked_cv_cnt);

	} else if (chg_type == POWER_SUPPLY_CHARGE_TYPE_TRICKLE) {
		/*
		 * Precharge: charging current/voltage are limited in
		 * hardware, no point in applying irdrop compensation.
		 * Just wait for battery voltage to raise over the
		 * precharge to fast charge threshold.
		 */
		msc_state = MSC_TYPE;

		/* no tier switching in trickle */
		if (batt_drv->checked_cv_cnt == 0)
			batt_drv->checked_cv_cnt = 1;

		pr_info("MSC_PRE vt=%d vb=%d fv_uv=%d chg_type=%d\n",
			vtier, vbatt, *fv_uv, chg_type);
	} else if (chg_type != POWER_SUPPLY_CHARGE_TYPE_TAPER) {
		/*
		 * Not fast, taper or precharge: in *_UNKNOWN and *_NONE
		 * set checked_cv_cnt=0 and check current to avoid early
		 * termination in case of lack of headroom
		 * NOTE: this can cause early switch on low ilim
		 * TODO: check if we are really lacking hedrooom.
		 */
		msc_state = MSC_TYPE;
		*update_interval = profile->cv_update_interval;
		batt_drv->checked_cv_cnt = 0;

		pr_info("MSC_TYPE vt=%d vb=%d fv_uv=%d chg_type=%d\n",
			vtier, vbatt, *fv_uv, chg_type);

	} else if (batt_drv->checked_ov_cnt) {
		/*
		 * TAPER_DLY: countdown to raise fv_uv and/or check
		 * for tier switch, will keep steady...
		 */
		pr_info("MSC_DLY vt=%d vb=%d fv_uv=%d margin=%d cv_cnt=%d, ov_cnt=%d\n",
			vtier, vbatt, *fv_uv, profile->cv_range_accuracy,
			batt_drv->checked_cv_cnt,
			batt_drv->checked_ov_cnt);

		msc_state = MSC_DLY;
		batt_drv->checked_ov_cnt -= 1;
		*update_interval = profile->cv_update_interval;

	} else if ((vtier - vbatt) < utv_margin) {
		/* TAPER_STEADY: close enough to tier */

		msc_state = MSC_STEADY;
		*update_interval = profile->cv_update_interval;

		pr_info("MSC_STEADY vt=%d vb=%d fv_uv=%d margin=%d\n",
			vtier, vbatt, *fv_uv,
			profile->cv_range_accuracy);
	} else if (batt_drv->checked_tier_switch_cnt >= (switch_cnt - 1)) {
		/*
		 * TAPER_TIERCNTING: prepare to switch to next tier
		 * so not allow to raise vfloat to prevent battery
		 * voltage over than tier
		 */
		msc_state = MSC_TIERCNTING;
		*update_interval = profile->cv_update_interval;

		pr_info("MSC_TIERCNTING vt=%d vb=%d fv_uv=%d margin=%d\n",
			vtier, vbatt, *fv_uv,
			profile->cv_range_accuracy);
	} else if (match_enable) {
		/*
		 * TAPER_RAISE: under tier vlim, raise one click &
		 * debounce taper (see above handling of STEADY)
		 */
		msc_state = MSC_RAISE;
		*fv_uv = gbms_msc_round_fv_uv(profile, vtier,
			*fv_uv + profile->fv_uv_resolution);
		*update_interval = profile->cv_update_interval;

		/* debounce next taper voltage adjustment */
		batt_drv->checked_cv_cnt = profile->cv_debounce_cnt;

		pr_info("MSC_RAISE vt=%d vb=%d fv_uv=%d->%d\n",
			vtier, vbatt, batt_drv->fv_uv, *fv_uv);
	} else {
		msc_state = MSC_STEADY;
		pr_info("MSC_DISB vt=%d vb=%d fv_uv=%d->%d\n",
			vtier, vbatt, batt_drv->fv_uv, *fv_uv);
	}

	return msc_state;
}

static int msc_pm_hold(int msc_state)
{
	int pm_state = -1;

	switch (msc_state) {
	case MSC_RAISE:
	case MSC_VOVER:
	case MSC_PULLBACK:
		pm_state = 1; /* __pm_stay_awake */
		break;
	case MSC_SEED:
	case MSC_DSG:
	case MSC_VSWITCH:
	case MSC_NEXT:
	case MSC_LAST:
		pm_state = 0;  /* pm_relax */
		break;
	}

	return pm_state;
}

/* TODO: this function is too long and need to be split (b/117897301) */
static int msc_logic_internal(struct batt_drv *batt_drv)
{
	bool sw_jeita;
	int msc_state = MSC_NONE;
	struct power_supply *fg_psy = batt_drv->fg_psy;
	struct gbms_chg_profile *profile = &batt_drv->chg_profile;
	int vbatt_idx = batt_drv->vbatt_idx, fv_uv = batt_drv->fv_uv, temp_idx;
	int temp, ibatt, vbatt, ioerr;
	int update_interval = MSC_DEFAULT_UPDATE_INTERVAL;
	const time_t now = get_boot_sec();
	time_t elap = now - batt_drv->ce_data.last_update;

	temp = GPSY_GET_INT_PROP(fg_psy, POWER_SUPPLY_PROP_TEMP, &ioerr);
	if (ioerr < 0)
		return -EIO;

	/*
	 * driver state is (was) reset when we hit the SW jeita limit.
	 * NOTE: resetting driver state will release the wake assertion
	 */
	sw_jeita = msc_logic_soft_jeita(batt_drv, temp);
	if (sw_jeita) {
		/* reset batt_drv->jeita_stop_charging to -1 */
		if (batt_drv->jeita_stop_charging == 0)
			batt_reset_chg_drv_state(batt_drv);

		return 0;
	} else if (batt_drv->jeita_stop_charging) {
		pr_info("MSC_JEITA temp=%d ok, enabling charging\n", temp);
		batt_drv->jeita_stop_charging = 0;
	}

	ibatt = GPSY_GET_INT_PROP(fg_psy, POWER_SUPPLY_PROP_CURRENT_NOW,
					  &ioerr);
	if (ioerr < 0)
		return -EIO;

	vbatt = GPSY_GET_PROP(fg_psy, POWER_SUPPLY_PROP_VOLTAGE_NOW);
	if (vbatt < 0)
		return -EIO;

	/*
	 * Multi Step Charging with IRDROP compensation when vchrg is != 0
	 * vbatt_idx = batt_drv->vbatt_idx, fv_uv = batt_drv->fv_uv
	 */
	temp_idx = gbms_msc_temp_idx(profile, temp);
	if (temp_idx != batt_drv->temp_idx || batt_drv->fv_uv == -1 ||
		batt_drv->vbatt_idx == -1) {

		msc_state = MSC_SEED;

		/* seed voltage only on connect, book 0 time */
		if (batt_drv->vbatt_idx == -1)
			vbatt_idx = gbms_msc_voltage_idx(profile, vbatt);

		pr_info("MSC_SEED temp=%d vbatt=%d temp_idx:%d->%d, vbatt_idx:%d->%d\n",
			temp, vbatt, batt_drv->temp_idx, temp_idx,
			batt_drv->vbatt_idx, vbatt_idx);

		/* Debounce tier switch only when not already switching */
		if (batt_drv->checked_tier_switch_cnt == 0)
			batt_drv->checked_cv_cnt = profile->cv_debounce_cnt;
	} else if (ibatt > 0) {
		/*
		 * Track battery voltage if discharging is due to system load,
		 * low ILIM or lack of headroom; stop charging work and reset
		 * batt_drv state() when discharging is due to disconnect.
		 * NOTE: POWER_SUPPLY_PROP_STATUS return *_DISCHARGING only on
		 * disconnect.
		 * NOTE: same vbat_idx will not change fv_uv
		 */
		msc_state = MSC_DSG;
		vbatt_idx = gbms_msc_voltage_idx(profile, vbatt);

		pr_info("MSC_DSG vbatt_idx:%d->%d vbatt=%d ibatt=%d fv_uv=%d cv_cnt=%d ov_cnt=%d\n",
			batt_drv->vbatt_idx, vbatt_idx,
			vbatt, ibatt, fv_uv,
			batt_drv->checked_cv_cnt,
			batt_drv->checked_ov_cnt);
	} else if (batt_drv->vbatt_idx == profile->volt_nb_limits - 1) {
		const int chg_type = batt_drv->chg_state.f.chg_type;

		/*
		 * will not adjust charger voltage only in the configured
		 * last tier.
		 * NOTE: might not be the "real" last tier since can I have
		 * tiers with max charge current == 0.
		 * NOTE: should I use a voltage limit instead?
		 */

		if (chg_type == POWER_SUPPLY_CHARGE_TYPE_FAST) {
			msc_state = MSC_FAST;
		} else if (chg_type != POWER_SUPPLY_CHARGE_TYPE_TAPER) {
			msc_state = MSC_TYPE;
		} else {
			msc_state = MSC_LAST;
		}

		pr_info("MSC_LAST vbatt=%d ibatt=%d fv_uv=%d\n",
			vbatt, ibatt, fv_uv);

	} else {
		const int tier_idx = batt_drv->vbatt_idx;
		const int vtier = profile->volt_limits[vbatt_idx];
		const int switch_cnt = profile->cv_tier_switch_cnt;
		const int cc_next_max = GBMS_CCCM_LIMITS(profile, temp_idx,
							vbatt_idx + 1);

		/* book elapsed time to previous tier & msc_irdrop_state */
		msc_state = msc_logic_irdrop(batt_drv,
					     vbatt, ibatt, temp_idx,
					     &vbatt_idx, &fv_uv,
					     &update_interval);

		if (msc_pm_hold(msc_state) == 1 && !batt_drv->hold_taper_ws) {
			__pm_stay_awake(batt_drv->taper_ws);
			batt_drv->hold_taper_ws = true;
		}

		mutex_lock(&batt_drv->stats_lock);
		batt_chg_stats_tier(&batt_drv->ce_data.tier_stats[tier_idx],
				    batt_drv->msc_irdrop_state, elap);
		batt_drv->msc_irdrop_state = msc_state;
		mutex_unlock(&batt_drv->stats_lock);

		/*
		 * Basic multi step charging: switch to next tier when ibatt
		 * is under next tier cc_max.
		 */
		if (batt_drv->checked_cv_cnt > 0) {
			/* debounce period on tier switch */
			msc_state = MSC_WAIT;
			batt_drv->checked_cv_cnt -= 1;

			pr_info("MSC_WAIT vt=%d vb=%d fv_uv=%d ibatt=%d cv_cnt=%d ov_cnt=%d t_cnt=%d\n",
				vtier, vbatt, fv_uv, ibatt,
				batt_drv->checked_cv_cnt,
				batt_drv->checked_ov_cnt,
				batt_drv->checked_tier_switch_cnt);

			if (-ibatt > cc_next_max)
				batt_drv->checked_tier_switch_cnt = 0;

		} else if (-ibatt > cc_next_max) {
			/* current over next tier, reset tier switch count */
			msc_state = MSC_RSTC;
			batt_drv->checked_tier_switch_cnt = 0;

			pr_info("MSC_RSTC vt=%d vb=%d fv_uv=%d ibatt=%d cc_next_max=%d t_cnt=%d\n",
				vtier, vbatt, fv_uv, ibatt, cc_next_max,
				batt_drv->checked_tier_switch_cnt);
		} else if (batt_drv->checked_tier_switch_cnt >= switch_cnt) {
			/* next tier, fv_uv detemined at MSC_SET */
			msc_state = MSC_NEXT;
			vbatt_idx = batt_drv->vbatt_idx + 1;

			pr_info("MSC_NEXT tier vb=%d ibatt=%d vbatt_idx=%d->%d\n",
				vbatt, ibatt, batt_drv->vbatt_idx, vbatt_idx);
		} else {
			/* current under next tier, +1 on tier switch count */
			msc_state = MSC_NYET;
			batt_drv->checked_tier_switch_cnt++;

			pr_info("MSC_NYET ibatt=%d cc_next_max=%d t_cnt=%d\n",
				ibatt, cc_next_max,
				batt_drv->checked_tier_switch_cnt);
		}

	}

	if (msc_pm_hold(msc_state) == 0 && batt_drv->hold_taper_ws) {
		batt_drv->hold_taper_ws = false;
		__pm_relax(batt_drv->taper_ws);
	}

	/* need a new fv_uv only on a new voltage tier.  */
	if (vbatt_idx != batt_drv->vbatt_idx) {
		fv_uv = profile->volt_limits[vbatt_idx];
		batt_drv->checked_tier_switch_cnt = 0;
		batt_drv->checked_ov_cnt = 0;
	}

	/* book elapsed time to previous tier & msc_state
	 * NOTE: temp_idx != -1 but batt_drv->msc_state could be -1
	 */
	mutex_lock(&batt_drv->stats_lock);
	if (vbatt_idx != -1 && vbatt_idx < GBMS_STATS_TIER_COUNT) {
		int tier_idx = batt_drv->vbatt_idx;

		/* this is the seed after the connect */
		if (batt_drv->vbatt_idx == -1) {
			tier_idx = vbatt_idx;
			elap = 0;
		}

		batt_chg_stats_update(batt_drv, temp_idx, tier_idx,
				      ibatt / 1000, temp,
				      elap);

	}

	batt_drv->msc_state = msc_state;
	batt_drv->ce_data.last_update = now;
	mutex_unlock(&batt_drv->stats_lock);

	pr_debug("MSC_LOGIC cv_cnt=%d ov_cnt=%d temp_idx:%d->%d, vbatt_idx:%d->%d, fv=%d->%d, ui=%d->%d\n",
		batt_drv->checked_cv_cnt, batt_drv->checked_ov_cnt,
		batt_drv->temp_idx, temp_idx, batt_drv->vbatt_idx,
		vbatt_idx, batt_drv->fv_uv, fv_uv, batt_drv->cc_max,
		update_interval);

	/* next update */
	batt_drv->msc_update_interval = update_interval;
	batt_drv->vbatt_idx = vbatt_idx;
	batt_drv->temp_idx = temp_idx;
	batt_drv->cc_max = GBMS_CCCM_LIMITS(profile, temp_idx, vbatt_idx);
	batt_drv->fv_uv = fv_uv;

	return 0;
}

/* called holding chg_lock */
static int msc_logic(struct batt_drv *batt_drv)
{
	int err = 0;
	bool changed = false;
	union gbms_charger_state *chg_state = &batt_drv->chg_state;

	if (!batt_drv->chg_profile.cccm_limits)
		return -EINVAL;

	__pm_stay_awake(batt_drv->msc_ws);

	pr_info("MSC_DIN chg_state=%lx f=0x%x chg_s=%s chg_t=%s vchg=%d icl=%d\n",
		(unsigned long)chg_state->v,
		chg_state->f.flags,
		gbms_chg_status_s(chg_state->f.chg_status),
		gbms_chg_type_s(chg_state->f.chg_type),
		chg_state->f.vchrg,
		chg_state->f.icl);

	if ((batt_drv->chg_state.f.flags & GBMS_CS_FLAG_BUCK_EN) == 0) {

		if (batt_drv->ssoc_state.buck_enabled == 0)
			goto msc_logic_exit;

		/* here on: disconnect */
		batt_chg_stats_pub(batt_drv, "disconnect", false);
		batt_res_state_set(&batt_drv->res_state, false);

		/* change curve before changing the state */
		ssoc_change_curve(&batt_drv->ssoc_state, SSOC_UIC_TYPE_DSG);
		batt_reset_chg_drv_state(batt_drv);
		batt_update_cycle_count(batt_drv);
		batt_rl_reset(batt_drv);

		/* this will trigger another capacity learning. */
		err = GPSY_SET_PROP(batt_drv->fg_psy,
				    POWER_SUPPLY_PROP_BATT_CE_CTRL,
				    false);
		if (err < 0)
			pr_err("Cannot set the BATT_CE_CTRL.\n");

		batt_drv->ssoc_state.buck_enabled = 0;
		changed = true;

		goto msc_logic_done;
	}

	/* here when connected to power supply */
	if (batt_drv->ssoc_state.buck_enabled <= 0) {

		ssoc_change_curve(&batt_drv->ssoc_state, SSOC_UIC_TYPE_CHG);

		if (batt_drv->res_state.estimate_filter)
			batt_res_state_set(&batt_drv->res_state, true);

		batt_chg_stats_start(batt_drv);
		err = GPSY_SET_PROP(batt_drv->fg_psy,
				    POWER_SUPPLY_PROP_BATT_CE_CTRL,
				    true);
		if (err < 0)
			pr_err("Cannot set the BATT_CE_CTRL.\n");

		/* released in battery_work() */
		__pm_stay_awake(batt_drv->poll_ws);
		batt_drv->batt_fast_update_cnt = BATT_WORK_FAST_RETRY_CNT;
		mod_delayed_work(system_wq, &batt_drv->batt_work,
			BATT_WORK_FAST_RETRY_MS);

		batt_drv->ssoc_state.buck_enabled = 1;
		changed = true;
	}

	/*
	 * enter RL in DISCHARGE on charger DONE and enter RL in RECHARGE on
	 * battery FULL (i.e. SSOC==100%). charger DONE forces the discharge
	 * curve while RECHARGE will not modify the current curve.
	 */
	if ((batt_drv->chg_state.f.flags & GBMS_CS_FLAG_DONE) != 0) {
		changed = batt_rl_enter(&batt_drv->ssoc_state,
					BATT_RL_STATUS_DISCHARGE);
		batt_drv->chg_done = true;
	} else if (batt_drv->batt_full) {
		changed = batt_rl_enter(&batt_drv->ssoc_state,
					BATT_RL_STATUS_RECHARGE);
		if (changed)
			batt_chg_stats_pub(batt_drv, "100%", false);
	}

	err = msc_logic_internal(batt_drv);
	if (err < 0) {
		/* NOTE: google charger will poll again. */
		batt_drv->msc_update_interval = -1;

		pr_err("MSC_DOUT ERROR=%d fv_uv=%d cc_max=%d update_interval=%d\n",
			err, batt_drv->fv_uv, batt_drv->cc_max,
			batt_drv->msc_update_interval);

		goto msc_logic_exit;
	}

msc_logic_done:
	/* set ->cc_max = 0 on RL and SW_JEITA, no vote on interval in RL_DSG */
	if (batt_drv->ssoc_state.rl_status == BATT_RL_STATUS_DISCHARGE) {
		batt_drv->msc_update_interval = -1;
		batt_drv->cc_max = 0;
	}
	if (batt_drv->jeita_stop_charging)
		batt_drv->cc_max = 0;

	pr_info("%s msc_state=%d cv_cnt=%d ov_cnt=%d temp_idx:%d, vbatt_idx:%d  fv_uv=%d cc_max=%d update_interval=%d\n",
		(batt_drv->disable_votes) ? "MSC_DOUT" : "MSC_VOTE",
		batt_drv->msc_state,
		batt_drv->checked_cv_cnt, batt_drv->checked_ov_cnt,
		batt_drv->temp_idx, batt_drv->vbatt_idx,
		batt_drv->fv_uv, batt_drv->cc_max,
		batt_drv->msc_update_interval);

	 /*
	  * google_charger has voted(<=0) on msc_interval_votable and the
	  * votes on fcc and fv_uv will not be applied until google_charger
	  * votes a non-zero value.
	  *
	  * SW_JEITA: ->jeita_stop_charging != 0
	  * . ->msc_update_interval = -1 , fv_uv = -1 and ->cc_max = 0
	  * . vote(0) on ->fcc_votable with SW_JEITA_VOTER
	  * BATT_RL: rl_status == BATT_RL_STATUS_DISCHARGE
	  * . ->msc_update_interval = -1 , fv_uv = -1 and ->cc_max = 0
	  * . vote(0) on ->fcc_votable with SW_JEITA_VOTER
	  *
	  * Votes for MSC_LOGIC_VOTER will be all disabled.
	  */
	if (!batt_drv->fv_votable)
		batt_drv->fv_votable = find_votable(VOTABLE_MSC_FV);
	if (batt_drv->fv_votable)
		vote(batt_drv->fv_votable, MSC_LOGIC_VOTER,
			!batt_drv->disable_votes && (batt_drv->fv_uv != -1),
			batt_drv->fv_uv);

	if (!batt_drv->fcc_votable)
		batt_drv->fcc_votable = find_votable(VOTABLE_MSC_FCC);
	if (batt_drv->fcc_votable) {
		enum batt_rl_status rl_status = batt_drv->ssoc_state.rl_status;

		/* while in RL => ->cc_max != -1 && ->fv_uv != -1 */
		vote(batt_drv->fcc_votable, RL_STATE_VOTER,
			!batt_drv->disable_votes &&
			(rl_status == BATT_RL_STATUS_DISCHARGE),
			0);

		/* jeita_stop_charging != 0 => ->fv_uv = -1 && cc_max == -1 */
		vote(batt_drv->fcc_votable, SW_JEITA_VOTER,
			!batt_drv->disable_votes &&
			(batt_drv->jeita_stop_charging != 0),
			0);

		vote(batt_drv->fcc_votable, MSC_LOGIC_VOTER,
			!batt_drv->disable_votes && (batt_drv->cc_max != -1),
			batt_drv->cc_max);
	}

	if (!batt_drv->msc_interval_votable)
		batt_drv->msc_interval_votable =
			find_votable(VOTABLE_MSC_INTERVAL);
	if (batt_drv->msc_interval_votable)
		vote(batt_drv->msc_interval_votable, MSC_LOGIC_VOTER,
			!batt_drv->disable_votes &&
			(batt_drv->msc_update_interval != -1),
			batt_drv->msc_update_interval);

msc_logic_exit:

	if (changed) {
		dump_ssoc_state(&batt_drv->ssoc_state, batt_drv->ssoc_log);
		if (batt_drv->psy)
			power_supply_changed(batt_drv->psy);
	}

	__pm_relax(batt_drv->msc_ws);
	return err;
}

/* charge profile not in battery */
static int batt_init_chg_profile(struct batt_drv *batt_drv)
{
	struct device_node *node = batt_drv->device->of_node;
	struct gbms_chg_profile *profile = &batt_drv->chg_profile;
	int ret = 0;

	/* handle retry */
	if (!profile->cccm_limits) {
		ret = gbms_init_chg_profile(profile, node);
		if (ret < 0)
			return -EINVAL;
	}

	ret = of_property_read_u32(node, "google,chg-battery-capacity",
				   &batt_drv->battery_capacity);
	if (ret < 0)
		pr_warn("read chg-battery-capacity from gauge\n");

	/*
	 * use battery FULL design when is not specified in DT. When battery is
	 * not present use default capacity from DT (if present) or disable
	 * charging altogether.
	 */
	if (batt_drv->battery_capacity == 0) {
		u32 fc = 0;
		struct power_supply *fg_psy = batt_drv->fg_psy;

		if (batt_drv->batt_present) {
			fc = GPSY_GET_PROP(fg_psy,
					POWER_SUPPLY_PROP_CHARGE_FULL_DESIGN);
			if (fc == -EAGAIN)
				return -EPROBE_DEFER;
			if (fc > 0) {
				pr_info("successfully read charging profile:\n");
				/* convert uA to mAh*/
				batt_drv->battery_capacity = fc / 1000;
			}

		}

		if (batt_drv->battery_capacity == 0) {
			struct device_node *node = batt_drv->device->of_node;

			ret = of_property_read_u32(node,
					"google,chg-battery-default-capacity",
						&batt_drv->battery_capacity);
			if (ret < 0)
				pr_warn("battery not present, no default capacity, zero charge table\n");
			else
				pr_warn("battery not present, using default capacity:\n");
		}
	}

	/* NOTE: with NG charger tolerance is applied from "charger" */
	gbms_init_chg_table(&batt_drv->chg_profile, batt_drv->battery_capacity);

	return 0;
}

/* ------------------------------------------------------------------------- */

/* call holding mutex_unlock(&ccd->lock); */
static int batt_cycle_count_store(struct gbatt_ccbin_data *ccd)
{
	int ret;

	ret = gbms_storage_write(GBMS_TAG_BCNT, ccd->count, sizeof(ccd->count));
	if (ret < 0 && ret != -ENOENT) {
		pr_err("failed to set bin_counts ret=%d\n", ret);
		return ret;
	}

	return 0;
}

/* call holding mutex_unlock(&ccd->lock); */
static int batt_cycle_count_load(struct gbatt_ccbin_data *ccd)
{
	int ret;

	ret = gbms_storage_read(GBMS_TAG_BCNT, ccd->count, sizeof(ccd->count));
	if (ret < 0 && ret != -ENOENT) {
		pr_err("failed to get bin_counts ret=%d\n", ret);
		return ret;
	}

	ccd->prev_soc = -1;
	return 0;
}

/* update only when SSOC is increasing, not need to check charging */
static void batt_cycle_count_update(struct batt_drv *batt_drv, int soc)
{
	struct gbatt_ccbin_data *ccd = &batt_drv->cc_data;

	if (soc < 0 || soc > 100)
		return;

	mutex_lock(&ccd->lock);

	if (ccd->prev_soc != -1 && soc > ccd->prev_soc) {
		int bucket, cnt;

		for (cnt = soc ; cnt > ccd->prev_soc ; cnt--) {
			/* cnt decremented by 1 for bucket symmetry */
			bucket = (cnt - 1) * GBMS_CCBIN_BUCKET_COUNT / 100;
			ccd->count[bucket]++;
		}

		/* NOTE: could store on FULL or disconnect instead */
		(void)batt_cycle_count_store(ccd);
	}

	ccd->prev_soc = soc;

	mutex_unlock(&ccd->lock);
}

/* ------------------------------------------------------------------------- */

#define BATTERY_DEBUG_ATTRIBUTE(name, fn_read, fn_write) \
static const struct file_operations name = {	\
	.open	= simple_open,			\
	.llseek	= no_llseek,			\
	.read	= fn_read,			\
	.write	= fn_write,			\
}

static ssize_t batt_cycle_count_set_bins(struct file *filp,
					 const char __user *user_buf,
					 size_t count, loff_t *ppos)
{
	struct batt_drv *batt_drv = (struct batt_drv *)filp->private_data;
	char buf[GBMS_CCBIN_CSTR_SIZE];
	int ret;

	ret = simple_write_to_buffer(buf, sizeof(buf), ppos, user_buf, count);
	if (!ret)
		return -EFAULT;

	mutex_lock(&batt_drv->cc_data.lock);

	ret = gbms_cycle_count_sscan(batt_drv->cc_data.count, buf);
	if (ret == 0) {
		ret = batt_cycle_count_store(&batt_drv->cc_data);
		if (ret < 0)
			pr_err("cannot store bin count ret=%d\n", ret);
	}

	if (ret == 0)
		ret = count;

	mutex_unlock(&batt_drv->cc_data.lock);

	return ret;
}

BATTERY_DEBUG_ATTRIBUTE(cycle_count_bins_fops,
				NULL, batt_cycle_count_set_bins);


static int cycle_count_bins_store(void *data, u64 val)
{
	struct batt_drv *batt_drv = (struct batt_drv *)data;
	int ret;

	mutex_lock(&batt_drv->cc_data.lock);
	ret = batt_cycle_count_store(&batt_drv->cc_data);
	if (ret < 0)
		pr_err("cannot store bin count ret=%d\n", ret);
	mutex_unlock(&batt_drv->cc_data.lock);

	return ret;
}

static int cycle_count_bins_reload(void *data, u64 *val)
{
	struct batt_drv *batt_drv = (struct batt_drv *)data;
	int ret;

	mutex_lock(&batt_drv->cc_data.lock);
	ret = batt_cycle_count_load(&batt_drv->cc_data);
	if (ret < 0)
		pr_err("cannot restore bin count ret=%d\n", ret);
	mutex_unlock(&batt_drv->cc_data.lock);
	*val = ret;

	return ret;
}

DEFINE_SIMPLE_ATTRIBUTE(cycle_count_bins_sync_fops,
				cycle_count_bins_reload,
				cycle_count_bins_store, "%llu\n");


static int debug_get_ssoc_gdf(void *data, u64 *val)
{
	struct batt_drv *batt_drv = (struct batt_drv *)data;
	*val = batt_drv->ssoc_state.ssoc_gdf;
	return 0;
}

DEFINE_SIMPLE_ATTRIBUTE(debug_ssoc_gdf_fops, debug_get_ssoc_gdf, NULL, "%u\n");


static int debug_get_ssoc_uic(void *data, u64 *val)
{
	struct batt_drv *batt_drv = (struct batt_drv *)data;
	*val = batt_drv->ssoc_state.ssoc_uic;
	return 0;
}

DEFINE_SIMPLE_ATTRIBUTE(debug_ssoc_uic_fops, debug_get_ssoc_uic, NULL, "%u\n");

static int debug_get_ssoc_rls(void *data, u64 *val)
{
	struct batt_drv *batt_drv = (struct batt_drv *)data;

	mutex_lock(&batt_drv->chg_lock);
	*val = batt_drv->ssoc_state.rl_status;
	mutex_unlock(&batt_drv->chg_lock);

	return 0;
}

static int debug_set_ssoc_rls(void *data, u64 val)
{
	struct batt_drv *batt_drv = (struct batt_drv *)data;

	if (val < 0 || val > 2)
		return -EINVAL;

	mutex_lock(&batt_drv->chg_lock);
	batt_drv->ssoc_state.rl_status = val;
	if (!batt_drv->fcc_votable)
		batt_drv->fcc_votable = find_votable(VOTABLE_MSC_FCC);
	if (batt_drv->fcc_votable)
		vote(batt_drv->fcc_votable, RL_STATE_VOTER,
			batt_drv->ssoc_state.rl_status ==
						BATT_RL_STATUS_DISCHARGE,
			0);
	mutex_unlock(&batt_drv->chg_lock);

	return 0;
}

DEFINE_SIMPLE_ATTRIBUTE(debug_ssoc_rls_fops,
				debug_get_ssoc_rls, debug_set_ssoc_rls, "%u\n");

static int debug_force_psy_update(void *data, u64 val)
{
	struct batt_drv *batt_drv = (struct batt_drv *)data;

	if (!batt_drv->psy)
		return -EINVAL;

	power_supply_changed(batt_drv->psy);
	return 0;
}

DEFINE_SIMPLE_ATTRIBUTE(debug_force_psy_update_fops,
				NULL, debug_force_psy_update, "%u\n");


static ssize_t debug_get_ssoc_uicurve(struct file *filp,
					   char __user *buf,
					   size_t count, loff_t *ppos)
{
	struct batt_drv *batt_drv = (struct batt_drv *)filp->private_data;
	char tmp[UICURVE_BUF_SZ] = { 0 };

	mutex_lock(&batt_drv->chg_lock);
	ssoc_uicurve_cstr(tmp, sizeof(tmp), batt_drv->ssoc_state.ssoc_curve);
	mutex_unlock(&batt_drv->chg_lock);

	return simple_read_from_buffer(buf, count, ppos, tmp, strlen(tmp));
}

static ssize_t debug_set_ssoc_uicurve(struct file *filp,
					 const char __user *user_buf,
					 size_t count, loff_t *ppos)
{
	struct batt_drv *batt_drv = (struct batt_drv *)filp->private_data;
	int ret, curve_type;
	char buf[8];

	ret = simple_write_to_buffer(buf, sizeof(buf), ppos, user_buf, count);
	if (!ret)
		return -EFAULT;

	mutex_lock(&batt_drv->chg_lock);

	curve_type = (int)simple_strtoull(buf, NULL, 10);
	if (curve_type >= -1 && curve_type <= 1)
		ssoc_change_curve(&batt_drv->ssoc_state, curve_type);
	else
		ret = -EINVAL;

	mutex_unlock(&batt_drv->chg_lock);

	if (ret == 0)
		ret = count;

	return 0;
}

BATTERY_DEBUG_ATTRIBUTE(debug_ssoc_uicurve_cstr_fops,
					debug_get_ssoc_uicurve,
					debug_set_ssoc_uicurve);

static ssize_t debug_get_fake_temp(struct file *filp,
					   char __user *buf,
					   size_t count, loff_t *ppos)
{
	struct batt_drv *batt_drv = (struct batt_drv *)filp->private_data;
	char tmp[8];

	mutex_lock(&batt_drv->chg_lock);
	scnprintf(tmp, sizeof(tmp), "%d\n", batt_drv->fake_temp);
	mutex_unlock(&batt_drv->chg_lock);

	return simple_read_from_buffer(buf, count, ppos, tmp, strlen(tmp));
}

static ssize_t debug_set_fake_temp(struct file *filp,
					 const char __user *user_buf,
					 size_t count, loff_t *ppos)
{
	struct batt_drv *batt_drv = (struct batt_drv *)filp->private_data;
	int ret = 0, val;
	char buf[8];

	ret = simple_write_to_buffer(buf, sizeof(buf), ppos, user_buf, count);
	if (!ret)
		return -EFAULT;

	buf[ret] = '\0';
	ret = kstrtoint(buf, 0, &val);
	if (ret < 0)
		return ret;

	mutex_lock(&batt_drv->chg_lock);
	batt_drv->fake_temp = val;
	mutex_unlock(&batt_drv->chg_lock);

	return count;
}

BATTERY_DEBUG_ATTRIBUTE(debug_fake_temp_fops,
				debug_get_fake_temp, debug_set_fake_temp);


static enum batt_paired_state
batt_reset_pairing_state(const struct batt_drv *batt_drv)
{
	char dev_info[GBMS_DINF_LEN];
	int ret = 0;

	memset(dev_info, 0xff, sizeof(dev_info));
	ret = gbms_storage_write(GBMS_TAG_DINF, dev_info, sizeof(dev_info));
	if (ret < 0)
		return -EIO;

	return 0;
}

static ssize_t debug_set_pairing_state(struct file *filp,
					 const char __user *user_buf,
					 size_t count, loff_t *ppos)
{
	struct batt_drv *batt_drv = (struct batt_drv *)filp->private_data;
	int ret = 0, val;
	char buf[8];

	ret = simple_write_to_buffer(buf, sizeof(buf), ppos, user_buf, count);
	if (ret <= 0)
		return ret;

	buf[ret] = '\0';
	ret = kstrtoint(buf, 0, &val);
	if (ret < 0)
		return ret;

	mutex_lock(&batt_drv->chg_lock);

	if (val == BATT_PAIRING_ENABLED) {
		batt_drv->pairing_state = BATT_PAIRING_ENABLED;
		mod_delayed_work(system_wq, &batt_drv->batt_work, 0);
	} else if (val == BATT_PAIRING_RESET) {

		/* send a paring enable to re-pair OR reboot */
		ret = batt_reset_pairing_state(batt_drv);
		if (ret == 0)
			batt_drv->pairing_state = BATT_PAIRING_DISABLED;
		else
			count = -EIO;
	} else {
		count = -EINVAL;
	}
	mutex_unlock(&batt_drv->chg_lock);

	return count;
}

BATTERY_DEBUG_ATTRIBUTE(debug_pairing_fops, 0, debug_set_pairing_state);

/* TODO: add write to stop/start collection, erase history etc. */
static ssize_t debug_get_blf_state(struct file *filp, char __user *buf,
				   size_t count, loff_t *ppos)
{
	struct batt_drv *batt_drv = (struct batt_drv *)filp->private_data;
	char tmp[8];

	mutex_lock(&batt_drv->chg_lock);
	scnprintf(tmp, sizeof(tmp), "%d\n", batt_drv->blf_state);
	mutex_unlock(&batt_drv->chg_lock);

	return simple_read_from_buffer(buf, count, ppos, tmp, strlen(tmp));
}
BATTERY_DEBUG_ATTRIBUTE(debug_blf_state_fops, debug_get_blf_state, 0);

/* TODO: add writes to restart pairing (i.e. provide key) */
static ssize_t batt_pairing_state_show(struct device *dev,
				       struct device_attribute *attr,
				       char *buf)
{
	struct power_supply *psy = container_of(dev, struct power_supply, dev);
	struct batt_drv *batt_drv = power_supply_get_drvdata(psy);
	int len;

	mutex_lock(&batt_drv->chg_lock);
	len = scnprintf(buf, PAGE_SIZE, "%d\n", batt_drv->pairing_state);
	mutex_unlock(&batt_drv->chg_lock);
	return len;
}

static const DEVICE_ATTR(pairing_state, 0444, batt_pairing_state_show, NULL);


static ssize_t batt_ctl_chg_stats_actual(struct device *dev,
					 struct device_attribute *attr,
					 const char *buf, size_t count)
{
	struct power_supply *psy = container_of(dev, struct power_supply, dev);
	struct batt_drv *batt_drv =(struct batt_drv *)
					power_supply_get_drvdata(psy);

	if (count < 1)
		return -ENODATA;

	switch (buf[0]) {
	case 'p': /* publish data to qual */
	case 'P': /* force publish data to qual */
		batt_chg_stats_pub(batt_drv, "debug cmd", buf[0] == 'P');
		break;
	default:
		count = -EINVAL;
		break;
	}

	return count;
}

static ssize_t batt_show_chg_stats_actual(struct device *dev,
				   struct device_attribute *attr, char *buf)
{
	struct power_supply *psy = container_of(dev, struct power_supply, dev);
	struct batt_drv *batt_drv =(struct batt_drv *)
					power_supply_get_drvdata(psy);
	int len;

	mutex_lock(&batt_drv->stats_lock);
	len = batt_chg_stats_cstr(buf, PAGE_SIZE, &batt_drv->ce_data, false);
	mutex_unlock(&batt_drv->stats_lock);

	return len;
}

static const DEVICE_ATTR(charge_stats_actual, 0664,
					     batt_show_chg_stats_actual,
					     batt_ctl_chg_stats_actual);

static ssize_t batt_ctl_chg_stats(struct device *dev,
				  struct device_attribute *attr,
				  const char *buf, size_t count)
{
	struct power_supply *psy = container_of(dev, struct power_supply, dev);
	struct batt_drv *batt_drv =(struct batt_drv *)
					power_supply_get_drvdata(psy);

	if (count < 1)
		return -ENODATA;

	mutex_lock(&batt_drv->stats_lock);
	switch (buf[0]) {
	case 0:
	case '0': /* invalidate current qual */
		batt_chg_stats_init(&batt_drv->ce_qual, &batt_drv->chg_profile);
		break;
	}
	mutex_unlock(&batt_drv->stats_lock);

	return count;
}

static ssize_t batt_show_chg_stats(struct device *dev,
				   struct device_attribute *attr, char *buf)
{
	struct power_supply *psy = container_of(dev, struct power_supply, dev);
	struct batt_drv *batt_drv =(struct batt_drv *)
					power_supply_get_drvdata(psy);
	int len = -ENODATA;

	mutex_lock(&batt_drv->stats_lock);

	if (batt_drv->ce_qual.last_update - batt_drv->ce_qual.first_update)
		len = batt_chg_stats_cstr(buf,
					  PAGE_SIZE,
					  &batt_drv->ce_qual, false);

	mutex_unlock(&batt_drv->stats_lock);

	return len;
}

static const DEVICE_ATTR(charge_stats, 0664, batt_show_chg_stats,
					     batt_ctl_chg_stats);

static ssize_t batt_show_chg_details(struct device *dev,
				     struct device_attribute *attr, char *buf)
{
	struct power_supply *psy = container_of(dev, struct power_supply, dev);
	struct batt_drv *batt_drv =(struct batt_drv *)
					power_supply_get_drvdata(psy);
	const bool qual_valid = (batt_drv->ce_qual.last_update -
				batt_drv->ce_qual.first_update) != 0;
	int len = 0;

	mutex_lock(&batt_drv->stats_lock);

	len += batt_chg_stats_cstr(&buf[len], PAGE_SIZE - len,
				   &batt_drv->ce_data, true);
	if (qual_valid)
		len += batt_chg_stats_cstr(&buf[len], PAGE_SIZE - len,
					   &batt_drv->ce_qual, true);

	mutex_unlock(&batt_drv->stats_lock);

	return len;
}

static const DEVICE_ATTR(charge_details, 0444, batt_show_chg_details,
					       NULL);

/* tier and soc details */
static ssize_t batt_show_ttf_details(struct device *dev,
				     struct device_attribute *attr, char *buf)
{
	struct power_supply *psy = container_of(dev, struct power_supply, dev);
	struct batt_drv *batt_drv = (struct batt_drv *)
					power_supply_get_drvdata(psy);
	struct batt_ttf_stats *ttf_stats;
	int i, len = 0;

	if (!batt_drv->ssoc_state.buck_enabled)
		return -ENODATA;

	ttf_stats = kzalloc(sizeof(*ttf_stats), GFP_KERNEL);
	if (!ttf_stats)
		return -ENOMEM;

	mutex_lock(&batt_drv->stats_lock);
	/* update a private copy of ttf stats */
	ttf_stats_update(ttf_stats_dup(ttf_stats, &batt_drv->ttf_stats),
			 &batt_drv->ce_data, true);
	mutex_unlock(&batt_drv->stats_lock);

	/* interleave tier with SOC data */
	for (i = 0; i < GBMS_STATS_TIER_COUNT; i++) {
		int next_soc_in;

		len += scnprintf(&buf[len], PAGE_SIZE - len, "%d: ", i);
		len += ttf_tier_cstr(&buf[len], PAGE_SIZE - len,
				     &ttf_stats->tier_stats[i]);
		len += scnprintf(&buf[len], PAGE_SIZE - len, "\n");

		/* continue only first */
		if (ttf_stats->tier_stats[i].avg_time == 0)
			continue;

		if (i == GBMS_STATS_TIER_COUNT - 1) {
			next_soc_in = -1;
		} else {
			next_soc_in = ttf_stats->tier_stats[i + 1].soc_in >> 8;
			if (next_soc_in == 0)
				next_soc_in = -1;
		}

		if (next_soc_in == -1)
			next_soc_in = batt_drv->ce_data.last_soc - 1;

		len += ttf_soc_cstr(&buf[len], PAGE_SIZE - len,
				    &ttf_stats->soc_stats,
				    ttf_stats->tier_stats[i].soc_in >> 8,
				    next_soc_in);
	}

	kfree(ttf_stats);

	return len;
}

static const DEVICE_ATTR(ttf_details, 0444, batt_show_ttf_details,
					    NULL);

/* house stats */
static ssize_t batt_show_ttf_stats(struct device *dev,
				   struct device_attribute *attr, char *buf)
{
	struct power_supply *psy = container_of(dev, struct power_supply, dev);
	struct batt_drv *batt_drv =(struct batt_drv *)
					power_supply_get_drvdata(psy);
	const int verbose = true;
	int i, len = 0;

	mutex_lock(&batt_drv->stats_lock);

	for (i = 0; i < GBMS_STATS_TIER_COUNT; i++)
		len += ttf_tier_cstr(&buf[len], PAGE_SIZE,
				     &batt_drv->ttf_stats.tier_stats[i]);

	len += scnprintf(&buf[len], PAGE_SIZE - len, "\n");

	if (verbose)
		len += ttf_soc_cstr(&buf[len], PAGE_SIZE - len,
				    &batt_drv->ttf_stats.soc_stats,
				    0, 99);

	mutex_unlock(&batt_drv->stats_lock);

	return len;
}

/* userspace restore the TTF data with this */
static ssize_t batt_ctl_ttf_stats(struct device *dev,
				  struct device_attribute *attr,
				  const char *buf, size_t count)
{
	int res;
	struct power_supply *psy = container_of(dev, struct power_supply, dev);
	struct batt_drv *batt_drv =(struct batt_drv *)
					power_supply_get_drvdata(psy);

	if (count < 1)
		return -ENODATA;
	if (!batt_drv->ssoc_state.buck_enabled)
		return -ENODATA;

	mutex_lock(&batt_drv->stats_lock);
	switch (buf[0]) {
	case 'u':
	case 'U': /* force update */
		ttf_stats_update(&batt_drv->ttf_stats, &batt_drv->ce_data,
				 (buf[0] == 'U'));
		break;
	default:
		/* TODO: userspace restore of the data */
		res = ttf_stats_sscan(&batt_drv->ttf_stats, buf, count);
		if (res < 0)
			count = res;
		break;
	}
	mutex_unlock(&batt_drv->stats_lock);

	return count;
}

static const DEVICE_ATTR(ttf_stats, 0664, batt_show_ttf_stats,
					  batt_ctl_ttf_stats);

static int batt_init_fs(struct batt_drv *batt_drv)
{
	struct dentry *de = NULL;
	int ret;

	/* stats */
	ret = device_create_file(&batt_drv->psy->dev, &dev_attr_charge_stats);
	if (ret)
		dev_err(&batt_drv->psy->dev,
				"Failed to create charge_stats\n");

	ret = device_create_file(&batt_drv->psy->dev,
				 &dev_attr_charge_stats_actual);
	if (ret)
		dev_err(&batt_drv->psy->dev,
				"Failed to create charge_stats_actual\n");

	ret = device_create_file(&batt_drv->psy->dev,
				 &dev_attr_charge_details);
	if (ret)
		dev_err(&batt_drv->psy->dev,
				"Failed to create charge_details\n");

	/* time to full */
	ret = device_create_file(&batt_drv->psy->dev, &dev_attr_ttf_stats);
	if (ret)
		dev_err(&batt_drv->psy->dev,
				"Failed to create ttf_stats\n");

	ret = device_create_file(&batt_drv->psy->dev, &dev_attr_ttf_details);
	if (ret)
		dev_err(&batt_drv->psy->dev,
				"Failed to create ttf_details\n");

	ret = device_create_file(&batt_drv->psy->dev,
					&dev_attr_pairing_state);
	if (ret)
		dev_err(&batt_drv->psy->dev,
			"Failed to create pairing_state\n");

	de = debugfs_create_dir("google_battery", 0);
	if (IS_ERR_OR_NULL(de))
		return 0;

	debugfs_create_file("cycle_count_bins", 0400, de,
				batt_drv, &cycle_count_bins_fops);
	debugfs_create_file("cycle_count_sync", 0600, de,
				batt_drv, &cycle_count_bins_sync_fops);
	debugfs_create_file("ssoc_gdf", 0600, de,
				batt_drv, &debug_ssoc_gdf_fops);
	debugfs_create_file("ssoc_uic", 0600, de,
				batt_drv, &debug_ssoc_uic_fops);
	debugfs_create_file("ssoc_rls", 0400, de,
				batt_drv, &debug_ssoc_rls_fops);
	debugfs_create_file("ssoc_uicurve", 0600, de,
				batt_drv, &debug_ssoc_uicurve_cstr_fops);
	debugfs_create_file("force_psy_update", 0400, de,
				batt_drv, &debug_force_psy_update_fops);
	debugfs_create_file("pairing_state", 0200, de,
				batt_drv, &debug_pairing_fops);
	debugfs_create_file("blf_state", 0400, de,
				batt_drv, &debug_blf_state_fops);

	return 0;
}

/* ------------------------------------------------------------------------- */

/* could also use battery temperature, age */
static bool gbatt_check_dead_battery(const struct batt_drv *batt_drv)
{
	return ssoc_get_capacity(&batt_drv->ssoc_state) == 0;
}

#define SSOC_LEVEL_FULL		SSOC_SPOOF
#define SSOC_LEVEL_HIGH		80
#define SSOC_LEVEL_NORMAL	30
#define SSOC_LEVEL_LOW		0

/*
 * could also use battery temperature, age.
 * NOTE: this implementation looks at the SOC% but it might be looking to
 * other quantities or flags.
 * NOTE: CRITICAL_LEVEL implies BATTERY_DEAD but BATTERY_DEAD doesn't imply
 * CRITICAL.
 */
static int gbatt_get_capacity_level(struct batt_ssoc_state *ssoc_state,
				    int fg_status)
{
	const int ssoc = ssoc_get_capacity(ssoc_state);
	int capacity_level;

	if (ssoc >= SSOC_LEVEL_FULL) {
		capacity_level = POWER_SUPPLY_CAPACITY_LEVEL_FULL;
	} else if (ssoc > SSOC_LEVEL_HIGH) {
		capacity_level = POWER_SUPPLY_CAPACITY_LEVEL_HIGH;
	} else if (ssoc > SSOC_LEVEL_NORMAL) {
		capacity_level = POWER_SUPPLY_CAPACITY_LEVEL_NORMAL;
	} else if (ssoc > SSOC_LEVEL_LOW) {
		capacity_level = POWER_SUPPLY_CAPACITY_LEVEL_LOW;
	} else if (ssoc_state->buck_enabled == 0) {
		capacity_level = POWER_SUPPLY_CAPACITY_LEVEL_CRITICAL;
	} else if (ssoc_state->buck_enabled == -1) {
		/* only at startup, this should not happen */
		capacity_level = POWER_SUPPLY_CAPACITY_LEVEL_UNKNOWN;
	} else if (fg_status == POWER_SUPPLY_STATUS_DISCHARGING ||
		   fg_status == POWER_SUPPLY_STATUS_UNKNOWN) {
		capacity_level = POWER_SUPPLY_CAPACITY_LEVEL_CRITICAL;
	} else {
		capacity_level = POWER_SUPPLY_CAPACITY_LEVEL_LOW;
	}

	return capacity_level;
}

static int gbatt_get_temp(struct batt_drv *batt_drv, int *temp)
{
	int err = 0;
	union power_supply_propval val;

	if (batt_drv->fake_temp) {
		*temp = batt_drv->fake_temp;
	} else if (!batt_drv->fg_psy) {
		err = -EINVAL;
	} else {
		err = power_supply_get_property(batt_drv->fg_psy,
						POWER_SUPPLY_PROP_TEMP, &val);
		if (err == 0)
			*temp = val.intval;
	}

	return err;
}

void log_ttf_estimate(const char *label, int ssoc, struct batt_drv *batt_drv)
{
	int cc, err;
	time_t res = 0;


	err = batt_ttf_estimate(&res, batt_drv);
	if (err < 0) {
		logbuffer_log(batt_drv->ttf_log, "%s ssoc=%d time=%ld err=%d",
			      (label) ? label : "", ssoc, get_boot_sec(), err);
		return;
	}

	cc = GPSY_GET_PROP(batt_drv->fg_psy, POWER_SUPPLY_PROP_CHARGE_COUNTER);
	logbuffer_log(batt_drv->ttf_log,
		      "%s ssoc=%d cc=%d time=%ld %d:%d:%d (est=%ld)",
		      (label) ? label : "", ssoc, cc / 1000, get_boot_sec(),
		      res / 3600, (res % 3600) / 60, (res % 3600) % 60,
		      res);
}

static int batt_do_sha256(const u8 *data, unsigned int len, u8 *result)
{
	struct crypto_shash *tfm;
	struct shash_desc *shash;
	int size, ret = 0;

	tfm = crypto_alloc_shash("sha256", 0, 0);
	if (IS_ERR(tfm)) {
		pr_err("Error SHA-256 transform: %ld\n", PTR_ERR(tfm));
		return PTR_ERR(tfm);
	}

	size = sizeof(struct shash_desc) + crypto_shash_descsize(tfm);
	shash = kmalloc(size, GFP_KERNEL);
	if (!shash)
		return -ENOMEM;

	shash->tfm = tfm;
	ret = crypto_shash_digest(shash, data, len, result);
	kfree(shash);
	crypto_free_shash(tfm);

	return ret;
}


/* called with a lock on ->chg_lock */
static enum batt_paired_state
batt_check_pairing_state(struct batt_drv *batt_drv)
{
	const int len = strlen(dev_sn);
	char dev_info[GBMS_DINF_LEN];
	char mfg_info[GBMS_MINF_LEN];
	u8 *dev_info_check = batt_drv->dev_info_check;
	int ret;

	ret = gbms_storage_read(GBMS_TAG_DINF, dev_info, GBMS_DINF_LEN);
	if (ret < 0) {
		pr_err("Read device pairing info failed, ret=%d\n", ret);
		return BATT_PAIRING_READ_ERROR;
	}

	if (batt_drv->dev_info_check[0] == 0) {
		char data[len + GBMS_MINF_LEN];

		ret = gbms_storage_read(GBMS_TAG_MINF, mfg_info,
					GBMS_MINF_LEN);
		if (ret < 0) {
			pr_err("read mfg info. fail, ret=%d\n", ret);
			return BATT_PAIRING_READ_ERROR;
		}

		memcpy(data, dev_sn, len);
		memcpy(&data[len], mfg_info, GBMS_MINF_LEN);

		ret = batt_do_sha256(data, len + GBMS_MINF_LEN, dev_info_check);
		if (ret < 0) {
			pr_err("execute batt_do_sha256 fail, ret=%d\n", ret);
			return BATT_PAIRING_MISMATCH;
		}

		pr_info("dev_info_check: %s\n", dev_info_check);
	}

	/* new battery: pair the battery to this device */
	if (dev_info[0] == 0xFF) {

		ret = gbms_storage_write(GBMS_TAG_DINF, dev_info_check,
					 strlen(dev_info_check));
		if (ret < 0) {
			pr_err("Pairing to this device failed, ret=%d\n", ret);
			return BATT_PAIRING_WRITE_ERROR;
		}

	/* recycled battery */
	} else if (strncmp(dev_info, dev_info_check, strlen(dev_info_check))) {
		pr_warn("dev_sn=%*s\n", strlen(dev_sn), dev_sn);
		pr_warn("dev_info=%*s\n", GBMS_DINF_LEN, dev_info);
		pr_warn("Battery paired to a different device\n");

		return BATT_PAIRING_MISMATCH;
	}

	return BATT_PAIRING_PAIRED;
}

/* battery history data collection */
static int batt_history_data_work(struct batt_drv *batt_drv)
{
	int cycle_cnt, idx, ret;

	/* TODO: google_battery caches cycle count, should use that */
	cycle_cnt = GPSY_GET_PROP(batt_drv->fg_psy,
				  POWER_SUPPLY_PROP_CYCLE_COUNT);
	if (cycle_cnt < 0)
		return -EIO;

	idx = cycle_cnt / batt_drv->hist_delta_cycle_cnt;

	/* check if the cycle_cnt is valid */
	if (idx >= batt_drv->hist_data_max_cnt)
		return -ENOENT;

	/* TODO: too many arguments, redesign API affter specs */
	ret = batt_hist_data_collect(batt_drv->hist_data, cycle_cnt, idx,
				     batt_drv->fg_psy);
	if (ret < 0)
		pr_debug("Data collection failure %d\n", ret);

	return 0;
}

/*
 * poll the battery, run SOC%, dead battery, critical.
 * scheduled from psy_changed and from timer
 */
static void google_battery_work(struct work_struct *work)
{
	struct batt_drv *batt_drv =
	    container_of(work, struct batt_drv, batt_work.work);
	struct power_supply *fg_psy = batt_drv->fg_psy;
	struct batt_ssoc_state *ssoc_state = &batt_drv->ssoc_state;
	int update_interval = batt_drv->batt_update_interval;
	const int prev_ssoc = ssoc_get_capacity(ssoc_state);
	bool notify_psy_changed = false;
	int fg_status, ret, batt_temp;

	pr_debug("battery work item\n");

	__pm_stay_awake(batt_drv->batt_ws);

	fg_status = GPSY_GET_INT_PROP(fg_psy, POWER_SUPPLY_PROP_STATUS, &ret);
	if (ret < 0)
		goto reschedule;

	if (fg_status != batt_drv->fg_status)
		notify_psy_changed = true;
	batt_drv->fg_status = fg_status;

	/* chg_lock protect msc_logic */
	mutex_lock(&batt_drv->chg_lock);
	/* batt_lock protect SSOC code etc. */
	mutex_lock(&batt_drv->batt_lock);

	/* TODO: poll rate should be min between ->batt_update_interval and
	 * whatever ssoc_work() decides (typically rls->rl_delta_max_time)
	 */
	ret = ssoc_work(ssoc_state, fg_psy);
	if (ret < 0) {
		update_interval = BATT_WORK_ERROR_RETRY_MS;
	} else {
		bool full;
		int ssoc, level;

		/* handle charge/recharge */
		batt_rl_update_status(batt_drv);

		ssoc = ssoc_get_capacity(ssoc_state);
		if (prev_ssoc != ssoc) {
			if (ssoc > prev_ssoc)
				log_ttf_estimate("SSOC", ssoc, batt_drv);
			notify_psy_changed = true;
		}

		/* TODO(b/138860602): clear ->chg_done to enforce the
		 * same behavior during the transition 99 -> 100 -> Full
		 */

		level = gbatt_get_capacity_level(&batt_drv->ssoc_state,
						 fg_status);
		if (level != batt_drv->capacity_level) {
			batt_drv->capacity_level = level;
			notify_psy_changed = true;
		}

		if (batt_drv->dead_battery) {
			batt_drv->dead_battery =
					gbatt_check_dead_battery(batt_drv);
			if (!batt_drv->dead_battery)
				notify_psy_changed = true;
		}

		/* fuel gauge triggered recharge logic. */
		full = (ssoc == SSOC_FULL);
		if (full && !batt_drv->batt_full)
			log_ttf_estimate("Full", ssoc, batt_drv);
		batt_drv->batt_full = full;
	}

	/* TODO: poll other data here if needed */

	ret = gbatt_get_temp(batt_drv, &batt_temp);
	if (ret == 0 && batt_temp != batt_drv->batt_temp) {
		const int limit = batt_drv->batt_update_high_temp_threshold;

		batt_drv->batt_temp = batt_temp;
		if (batt_drv->batt_temp > limit)
			notify_psy_changed = true;
	}

	mutex_unlock(&batt_drv->batt_lock);

	/*
	 * wait for timeout or state equal to CHARGING, FULL or UNKNOWN
	 * (which will likely not happen) even on ssoc error. msc_logic
	 * hold poll_ws wakelock during this time.
	 */
	if (batt_drv->batt_fast_update_cnt) {

		if (fg_status != POWER_SUPPLY_STATUS_DISCHARGING &&
		    fg_status != POWER_SUPPLY_STATUS_NOT_CHARGING) {
			log_ttf_estimate("Start", prev_ssoc, batt_drv);
			batt_drv->batt_fast_update_cnt = 0;
		} else {
			update_interval = BATT_WORK_FAST_RETRY_MS;
			batt_drv->batt_fast_update_cnt -= 1;
		}
	}

	/* acquired in msc_logic */
	if (batt_drv->batt_fast_update_cnt == 0)
		__pm_relax(batt_drv->poll_ws);

	if (batt_drv->res_state.estimate_requested)
		batt_res_work(batt_drv);

	/* check only once and when/if the pairing state is reset */
	if (batt_drv->pairing_state == BATT_PAIRING_ENABLED) {
		enum batt_paired_state state;

		state = batt_check_pairing_state(batt_drv);
		switch (state) {
		/* somethig is wrong with eeprom comms, HW problem? */
		case BATT_PAIRING_READ_ERROR:
			break;
		/* somethig is wrong with eeprom, HW problem? */
		case BATT_PAIRING_WRITE_ERROR:
			break;
		default:
			batt_drv->pairing_state = state;
			break;
		}
	}

	mutex_unlock(&batt_drv->chg_lock);

	batt_cycle_count_update(batt_drv, ssoc_get_real(ssoc_state));
	dump_ssoc_state(ssoc_state, batt_drv->ssoc_log);

reschedule:

	if (notify_psy_changed)
		power_supply_changed(batt_drv->psy);


	/* collect lifetime and write to storage */
	if (batt_drv->blf_state == BATT_LFCOLLECT_ENABLED) {
		int cnt;

		/* gbms_storage will return -EPROBE_DEFER during init */
		cnt = gbms_storage_read_data(GBMS_TAG_HIST, NULL, 0, 0);
		if (cnt == -EPROBE_DEFER) {
			/* wait until storage is up */
		} else if (cnt < 0) {
			batt_drv->blf_state =  BATT_LFCOLLECT_NOT_AVAILABLE;
		} else {
			batt_drv->blf_state = BATT_LFCOLLECT_COLLECT;
			batt_drv->hist_data_max_cnt = cnt;
		}
	}

	if (batt_drv->blf_state == BATT_LFCOLLECT_COLLECT) {
		ret = batt_history_data_work(batt_drv);
		if (ret == -ENOENT) {
			batt_drv->blf_state = BATT_LFCOLLECT_NOT_AVAILABLE;
			pr_info("Battery data collection disabled\n");
		}  else if (ret < 0) {
			pr_debug("cannot collect battery data %d\n", ret);
		}
	}

	if (update_interval) {
		pr_debug("rerun battery work in %d ms\n", update_interval);
		schedule_delayed_work(&batt_drv->batt_work,
				      msecs_to_jiffies(update_interval));
	}

	__pm_relax(batt_drv->batt_ws);
}

/* ------------------------------------------------------------------------- */

/*
 * Keep the number of properies under UEVENT_NUM_ENVP (minus # of
 * standard uevent variables) i.e 26. Removed the following from
 * sysnodes
 *
 * POWER_SUPPLY_PROP_ADAPTER_DETAILS,	gbms
 * POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT, gbms
 * POWER_SUPPLY_PROP_CONSTANT_CHARGE_VOLTAGE, gbms
 *
 * POWER_SUPPLY_PROP_CHARGE_TYPE,
 * POWER_SUPPLY_PROP_CURRENT_AVG,
 * POWER_SUPPLY_PROP_INPUT_CURRENT_LIMITED,
 * POWER_SUPPLY_PROP_RESISTANCE_ID,	use BRID tag
 * POWER_SUPPLY_PROP_SOH,
 * POWER_SUPPLY_PROP_VOLTAGE_AVG,
 * POWER_SUPPLY_PROP_VOLTAGE_MAX_DESIGN,
 * POWER_SUPPLY_PROP_VOLTAGE_MIN_DESIGN,
 * POWER_SUPPLY_PROP_VOLTAGE_OCV,
 */

static enum power_supply_property gbatt_battery_props[] = {
	POWER_SUPPLY_PROP_CAPACITY,
	POWER_SUPPLY_PROP_CAPACITY_LEVEL,
	POWER_SUPPLY_PROP_CHARGE_COUNTER,
	POWER_SUPPLY_PROP_CHARGE_CHARGER_STATE,
	POWER_SUPPLY_PROP_CHARGE_DONE,
	POWER_SUPPLY_PROP_CHARGE_FULL,
	POWER_SUPPLY_PROP_CHARGE_FULL_DESIGN,
	POWER_SUPPLY_PROP_CHARGE_FULL_ESTIMATE,	/* google */
	POWER_SUPPLY_PROP_CURRENT_NOW,
	POWER_SUPPLY_PROP_CYCLE_COUNT,
	POWER_SUPPLY_PROP_CYCLE_COUNTS,
	POWER_SUPPLY_PROP_DEAD_BATTERY,
	POWER_SUPPLY_PROP_HEALTH,
	POWER_SUPPLY_PROP_PRESENT,
	POWER_SUPPLY_PROP_RESISTANCE,
	POWER_SUPPLY_PROP_RESISTANCE_AVG,	/* google */
	POWER_SUPPLY_PROP_SERIAL_NUMBER,
	POWER_SUPPLY_PROP_STATUS,
	POWER_SUPPLY_PROP_TEMP,
	POWER_SUPPLY_PROP_TECHNOLOGY,
	POWER_SUPPLY_PROP_TIME_TO_EMPTY_AVG,	/* No need for this? */
	POWER_SUPPLY_PROP_TIME_TO_FULL_NOW,
	POWER_SUPPLY_PROP_VOLTAGE_NOW,		/* 23 */

	/*  hard limit to 26 */
};

/*
 * status is:
 * . _UNKNOWN during init
 * . _DISCHARGING when not connected
 * when connected to a power supply status is
 * . _FULL (until disconnect) after the charger flags DONE if SSOC=100%
 * . _CHARGING if FG reports _FULL but SSOC < 100% (should not happen)
 * . _CHARGING if FG reports _NOT_CHARGING
 * . _NOT_CHARGING if FG report _DISCHARGING
 * . same as FG state otherwise
 */
static int gbatt_get_status(struct batt_drv *batt_drv,
			    union power_supply_propval *val)
{
	int err, ssoc;

	if (batt_drv->ssoc_state.buck_enabled == 0) {
		val->intval = POWER_SUPPLY_STATUS_DISCHARGING;
		return 0;
	}

	if (batt_drv->ssoc_state.buck_enabled == -1) {
		val->intval = POWER_SUPPLY_STATUS_UNKNOWN;
		return 0;
	}

	/* ->buck_enabled = 1, from here ownward device is connected */
	if (!batt_drv->fg_psy)
		return -EINVAL;

	ssoc = ssoc_get_capacity(&batt_drv->ssoc_state);

	/* FULL when the charger said so and SSOC == 100% */
	if (batt_drv->chg_done && ssoc == SSOC_FULL) {
		val->intval = POWER_SUPPLY_STATUS_FULL;
		return 0;
	}

	err = power_supply_get_property(batt_drv->fg_psy,
					POWER_SUPPLY_PROP_STATUS,
					val);
	if (err != 0)
		return err;

	if (val->intval == POWER_SUPPLY_STATUS_FULL) {

		/* not full unless the charger says so */
		if (!batt_drv->chg_done)
			val->intval = POWER_SUPPLY_STATUS_CHARGING;

		/* NOTE: FG driver could flag FULL before GDF is at 100% when
		 * gauge is not tuned or when capacity estimates are wrong.
		 */
		if (ssoc != SSOC_FULL)
			val->intval = POWER_SUPPLY_STATUS_CHARGING;

	} else if (val->intval == POWER_SUPPLY_STATUS_NOT_CHARGING) {
		/* smooth transition between charging and full */
		val->intval = POWER_SUPPLY_STATUS_CHARGING;
	} else if (val->intval == POWER_SUPPLY_STATUS_DISCHARGING) {
		/* connected and discharging is NOT charging */
		val->intval = POWER_SUPPLY_STATUS_NOT_CHARGING;
	}

	return 0;
}

static int gbatt_get_property(struct power_supply *psy,
				 enum power_supply_property psp,
				 union power_supply_propval *val)
{
	struct batt_drv *batt_drv = (struct batt_drv *)
					power_supply_get_drvdata(psy);
	struct batt_ssoc_state *ssoc_state = &batt_drv->ssoc_state;
	int rc, err = 0;

	pm_runtime_get_sync(batt_drv->device);
	if (!batt_drv->init_complete || !batt_drv->resume_complete) {
		pm_runtime_put_sync(batt_drv->device);
		return -EAGAIN;
	}
	pm_runtime_put_sync(batt_drv->device);

	switch (psp) {
	case POWER_SUPPLY_PROP_ADAPTER_DETAILS:
		val->intval = batt_drv->ce_data.adapter_details.v;
		break;

	case POWER_SUPPLY_PROP_CYCLE_COUNT:
		if (batt_drv->cycle_count < 0)
			err = batt_drv->cycle_count;
		else
			val->intval = batt_drv->cycle_count;
		break;

	case POWER_SUPPLY_PROP_CYCLE_COUNTS:
		mutex_lock(&batt_drv->cc_data.lock);
		(void)gbms_cycle_count_cstr(batt_drv->cc_data.cyc_ctr_cstr,
				sizeof(batt_drv->cc_data.cyc_ctr_cstr),
				batt_drv->cc_data.count);
		val->strval = batt_drv->cc_data.cyc_ctr_cstr;
		mutex_unlock(&batt_drv->cc_data.lock);
		break;

	case POWER_SUPPLY_PROP_CAPACITY:
		if (batt_drv->fake_capacity >= 0 &&
				batt_drv->fake_capacity <= 100)
			val->intval = batt_drv->fake_capacity;
		else {
			mutex_lock(&batt_drv->batt_lock);
			val->intval = ssoc_get_capacity(ssoc_state);
			mutex_unlock(&batt_drv->batt_lock);
		}
		break;

	case POWER_SUPPLY_PROP_DEAD_BATTERY:
		val->intval = batt_drv->dead_battery;
		break;

	case POWER_SUPPLY_PROP_CAPACITY_LEVEL:
		val->intval = batt_drv->capacity_level;
		break;

	/*
	 * ng charging:
	 * 1) write to POWER_SUPPLY_PROP_CHARGE_CHARGER_STATE,
	 * 2) read POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT and
	 *    POWER_SUPPLY_PROP_CONSTANT_CHARGE_VOLTAGE
	 */
	case POWER_SUPPLY_PROP_CHARGE_CHARGER_STATE:
		val->intval = batt_drv->chg_state.v;
		break;
	case POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT:
		mutex_lock(&batt_drv->chg_lock);
		val->intval = batt_drv->cc_max;
		mutex_unlock(&batt_drv->chg_lock);
		break;
	case POWER_SUPPLY_PROP_CONSTANT_CHARGE_VOLTAGE:
		mutex_lock(&batt_drv->chg_lock);
		val->intval = batt_drv->fv_uv;
		mutex_unlock(&batt_drv->chg_lock);
		break;

	/*
	 * POWER_SUPPLY_PROP_CHARGE_DONE comes from the charger BUT battery
	 * has also an idea about it. Now using a software state: charge is
	 * DONE when we are in the discharge phase of the recharge logic.
	 * NOTE: might change to keep DONE while rl_status != NONE
	 */
	case POWER_SUPPLY_PROP_CHARGE_DONE:
		mutex_lock(&batt_drv->chg_lock);
		val->intval = batt_drv->chg_done;
		mutex_unlock(&batt_drv->chg_lock);
		break;
	/*
	 * compat: POWER_SUPPLY_PROP_CHARGE_TYPE comes from the charger so
	 * using the last value reported from the CHARGER. This (of course)
	 * means that NG charging needs to be enabled.
	 */
	case POWER_SUPPLY_PROP_CHARGE_TYPE:
		mutex_lock(&batt_drv->chg_lock);
		val->intval = batt_drv->chg_state.f.chg_type;
		mutex_unlock(&batt_drv->chg_lock);
		break;

	/* compat, for *_CURRENT_LIMITED could return this one:
	 *	(batt_drv->chg_state.f.flags & GBMS_CS_FLAG_ILIM)
	 */
	case POWER_SUPPLY_PROP_INPUT_CURRENT_LIMITED:
		val->intval = 0;
		break;

	case POWER_SUPPLY_PROP_STATUS:
		err = gbatt_get_status(batt_drv, val);
		break;

	/* health */
	case POWER_SUPPLY_PROP_HEALTH:
		if (!batt_drv->fg_psy)
			return -EINVAL;
		err = power_supply_get_property(batt_drv->fg_psy, psp, val);
		if (err == 0)
			batt_drv->soh = val->intval;
		break;
	/* define this better */
	case POWER_SUPPLY_PROP_SOH:
		val->intval = batt_drv->soh;
		break;

	case POWER_SUPPLY_PROP_CHARGE_FULL_ESTIMATE:
		if (!batt_drv->fg_psy)
			return -EINVAL;
		err = power_supply_get_property(batt_drv->fg_psy, psp, val);
		break;
	case POWER_SUPPLY_PROP_RESISTANCE_AVG:
		if (batt_drv->res_state.filter_count <
			batt_drv->res_state.estimate_filter)
			val->intval = 0;
		else
			val->intval = batt_drv->res_state.resistance_avg;
		break;

	case POWER_SUPPLY_PROP_TIME_TO_FULL_NOW: {
		time_t res;

		rc = batt_ttf_estimate(&res, batt_drv);
		if (rc == 0) {
			val->intval = res;
		} else if (!batt_drv->fg_psy) {
			val->intval = -1;
		} else {
			rc = power_supply_get_property(batt_drv->fg_psy,
							psp, val);
			if (rc < 0)
				val->intval = -1;
		}
	} break;

	case POWER_SUPPLY_PROP_TEMP:
		err = gbatt_get_temp(batt_drv, &val->intval);
		break;

	case POWER_SUPPLY_PROP_CURRENT_AVG:
	case POWER_SUPPLY_PROP_CURRENT_NOW:
		if (!batt_drv->fg_psy)
			return -EINVAL;
		err = power_supply_get_property(batt_drv->fg_psy, psp, val);
		if (err == 0)
			val->intval = -val->intval;
		break;

	/*
	 * TODO: "charger" will expose this but I'd rather use an API from
	 * google_bms.h. Right now route it to fg_psy: just make sure that
	 * fg_psy doesn't look it up in google_battery
	 */
	case POWER_SUPPLY_PROP_RESISTANCE_ID:
		/* fall through */
	default:
		if (!batt_drv->fg_psy)
			return -EINVAL;
		err = power_supply_get_property(batt_drv->fg_psy, psp, val);
		break;
	}

	if (err < 0) {
		pr_debug("gbatt: get_prop cannot read psp=%d\n", psp);
		return err;
	}

	return 0;
}

static int gbatt_set_property(struct power_supply *psy,
				 enum power_supply_property psp,
				 const union power_supply_propval *val)
{
	struct batt_drv *batt_drv = (struct batt_drv *)
					power_supply_get_drvdata(psy);
	int ret = 0;

	pm_runtime_get_sync(batt_drv->device);
	if (!batt_drv->init_complete || !batt_drv->resume_complete) {
		pm_runtime_put_sync(batt_drv->device);
		return -EAGAIN;
	}
	pm_runtime_put_sync(batt_drv->device);

	switch (psp) {
	case POWER_SUPPLY_PROP_ADAPTER_DETAILS:
		mutex_lock(&batt_drv->stats_lock);
		batt_drv->ce_data.adapter_details.v = val->intval;
		mutex_unlock(&batt_drv->stats_lock);
	break;

	/* NG Charging, where it all begins */
	case POWER_SUPPLY_PROP_CHARGE_CHARGER_STATE:
		mutex_lock(&batt_drv->chg_lock);
		batt_drv->chg_state.v = val->int64val;
		ret = msc_logic(batt_drv);
		mutex_unlock(&batt_drv->chg_lock);
		break;

	case POWER_SUPPLY_PROP_CAPACITY:
		batt_drv->fake_capacity = val->intval;
		if (batt_drv->psy)
			power_supply_changed(batt_drv->psy);
		break;
	/* TODO: compat */
	case POWER_SUPPLY_PROP_CYCLE_COUNTS:
		ret = gbms_cycle_count_sscan(batt_drv->cc_data.count,
					     val->strval);
		if (ret == 0) {
			ret = batt_cycle_count_store(&batt_drv->cc_data);
			if (ret < 0)
				pr_err("cannot store bin count ret=%d\n", ret);
		}
		break;

	case POWER_SUPPLY_PROP_TIME_TO_FULL_NOW:
		if (val->intval <= 0)
			batt_drv->ttf_stats.ttf_fake = -1;
		else
			batt_drv->ttf_stats.ttf_fake = val->intval;
		pr_info("time_to_full = %ld\n", batt_drv->ttf_stats.ttf_fake);
		if (batt_drv->psy)
			power_supply_changed(batt_drv->psy);
		break;
	default:
		ret = -EINVAL;
		break;
	}

	if (ret < 0) {
		pr_debug("gbatt: get_prop cannot write psp=%d\n", psp);
		return ret;
	}


	return 0;
}

static int gbatt_property_is_writeable(struct power_supply *psy,
					  enum power_supply_property psp)
{
	switch (psp) {
	case POWER_SUPPLY_PROP_CHARGE_CHARGER_STATE:
	case POWER_SUPPLY_PROP_CAPACITY:
	case POWER_SUPPLY_PROP_ADAPTER_DETAILS:
	case POWER_SUPPLY_PROP_CYCLE_COUNTS:
	case POWER_SUPPLY_PROP_TIME_TO_FULL_NOW:
		return 1;
	default:
		break;
	}

	return 0;
}

static struct power_supply_desc gbatt_psy_desc = {
	.name = "battery",
	.type = POWER_SUPPLY_TYPE_BATTERY,
	.get_property = gbatt_get_property,
	.set_property = gbatt_set_property,
	.property_is_writeable = gbatt_property_is_writeable,
	.properties = gbatt_battery_props,
	.num_properties = ARRAY_SIZE(gbatt_battery_props),
};

/* ------------------------------------------------------------------------ */

static void google_battery_init_work(struct work_struct *work)
{
	struct batt_drv *batt_drv = container_of(work, struct batt_drv,
						 init_work.work);
	struct device_node *node = batt_drv->device->of_node;
	struct power_supply *fg_psy = batt_drv->fg_psy;
	bool has_eeprom;
	int ret = 0;

	batt_rl_reset(batt_drv);
	batt_drv->dead_battery = true; /* clear in batt_work() */
	batt_drv->capacity_level = POWER_SUPPLY_CAPACITY_LEVEL_UNKNOWN;
	batt_drv->ssoc_state.buck_enabled = -1;
	batt_drv->hold_taper_ws = false;
	batt_drv->fake_temp = 0;
	batt_reset_chg_drv_state(batt_drv);

	mutex_init(&batt_drv->chg_lock);
	mutex_init(&batt_drv->batt_lock);
	mutex_init(&batt_drv->stats_lock);
	mutex_init(&batt_drv->cc_data.lock);

	if (!batt_drv->fg_psy) {

		fg_psy = power_supply_get_by_name(batt_drv->fg_psy_name);
		if (!fg_psy) {
			pr_info("failed to get \"%s\" power supply, retrying...\n",
				batt_drv->fg_psy_name);
			goto retry_init_work;
		}

		batt_drv->fg_psy = fg_psy;
	}

	if (!batt_drv->batt_present) {
		ret = GPSY_GET_PROP(fg_psy, POWER_SUPPLY_PROP_PRESENT);
		if (ret == -EAGAIN)
			goto retry_init_work;

		batt_drv->batt_present = (ret > 0);
		if (!batt_drv->batt_present)
			pr_warn("battery not present (ret=%d)\n", ret);
	}

	ret = of_property_read_u32(node, "google,recharge-soc-threshold",
				   &batt_drv->ssoc_state.rl_soc_threshold);
	if (ret < 0)
		batt_drv->ssoc_state.rl_soc_threshold =
				DEFAULT_BATT_DRV_RL_SOC_THRESHOLD;

	/* cycle count is cached: read here bc SSOC, chg_profile might use it */
	batt_update_cycle_count(batt_drv);

	ret = ssoc_init(&batt_drv->ssoc_state, node, fg_psy);
	if (ret < 0 && batt_drv->batt_present)
		goto retry_init_work;

	dump_ssoc_state(&batt_drv->ssoc_state, batt_drv->ssoc_log);

	/* could read EEPROM and history here */

	ret = batt_init_chg_profile(batt_drv);
	if (ret == -EPROBE_DEFER)
		goto retry_init_work;

	if (ret < 0) {
		pr_err("charging profile disabled, ret=%d\n", ret);
	} else if (batt_drv->battery_capacity) {
		gbms_dump_chg_profile(&batt_drv->chg_profile);
	}

	batt_chg_stats_init(&batt_drv->ce_data, &batt_drv->chg_profile);
	batt_chg_stats_init(&batt_drv->ce_qual, &batt_drv->chg_profile);

	batt_drv->fg_nb.notifier_call = psy_changed;
	ret = power_supply_reg_notifier(&batt_drv->fg_nb);
	if (ret < 0)
		pr_err("cannot register power supply notifer, ret=%d\n",
			ret);

	batt_drv->batt_ws = wakeup_source_register(NULL, gbatt_psy_desc.name);
	batt_drv->taper_ws = wakeup_source_register(NULL, "Taper");
	batt_drv->poll_ws = wakeup_source_register(NULL, "Poll");
	batt_drv->msc_ws = wakeup_source_register(NULL, "MSC");
	if (!batt_drv->batt_ws || !batt_drv->taper_ws ||
			!batt_drv->poll_ws || !batt_drv->msc_ws)
		pr_err("failed to register wakeup sources\n");

	mutex_lock(&batt_drv->cc_data.lock);
	ret = batt_cycle_count_load(&batt_drv->cc_data);
	if (ret < 0)
		pr_err("cannot restore bin count ret=%d\n", ret);
	mutex_unlock(&batt_drv->cc_data.lock);

	batt_drv->fake_capacity = (batt_drv->batt_present) ? -EINVAL
						: DEFAULT_BATT_FAKE_CAPACITY;

	/* charging configuration */
	ret = of_property_read_u32(node, "google,update-interval",
				   &batt_drv->batt_update_interval);
	if (ret < 0)
		batt_drv->batt_update_interval = DEFAULT_BATT_UPDATE_INTERVAL;

	/* high temperature notify configuration */
	ret = of_property_read_u32(batt_drv->device->of_node,
				   "google,update-high-temp-threshold",
				   &batt_drv->batt_update_high_temp_threshold);
	if (ret < 0)
		batt_drv->batt_update_high_temp_threshold =
					DEFAULT_HIGH_TEMP_UPDATE_THRESHOLD;
	/* charge statistics */
	ret = of_property_read_u32(node, "google,chg-stats-qual-time",
				   &batt_drv->ce_data.chg_sts_qual_time);
	if (ret < 0)
		batt_drv->ce_data.chg_sts_qual_time =
					DEFAULT_CHG_STATS_MIN_QUAL_TIME;

	ret = of_property_read_u32(node, "google,chg-stats-delta-soc",
				   &batt_drv->ce_data.chg_sts_delta_soc);
	if (ret < 0)
		batt_drv->ce_data.chg_sts_delta_soc =
					DEFAULT_CHG_STATS_MIN_DELTA_SOC;

	/* time to full */
	ret = ttf_stats_init(&batt_drv->ttf_stats, batt_drv->device,
			     batt_drv->battery_capacity);
	if (ret < 0)
		pr_info("time to full not available\n");

	/* google_resistance  */
	batt_res_load_data(&batt_drv->res_state, batt_drv->fg_psy);

	/* override setting google,battery-roundtrip = 0 in device tree */
	batt_drv->disable_votes =
		of_property_read_bool(node, "google,disable-votes");
	if (batt_drv->disable_votes)
		pr_info("battery votes disabled\n");

	/* TODO: split pairing and collect, not all EEPROMS support it */
	has_eeprom = of_property_read_bool(node, "google,eeprom-inside");
	if (has_eeprom) {
		batt_drv->pairing_state = BATT_PAIRING_ENABLED;
		batt_drv->blf_state = BATT_LFCOLLECT_ENABLED;
	} else {
		batt_drv->pairing_state = BATT_PAIRING_DISABLED;
	}

	/* TODO: use delta cycle count to enable collecting history */
	ret = of_property_read_u32(batt_drv->device->of_node,
					"google,history-delta-cycle-count",
					&batt_drv->hist_delta_cycle_cnt);
	if (ret < 0)
		batt_drv->hist_delta_cycle_cnt = HCC_DEFAULT_DELTA_CYCLE_CNT;
	else
		batt_drv->blf_state = BATT_LFCOLLECT_ENABLED;

	/* TODO: use delta cycle count to enable collecting history */
	if (batt_drv->blf_state == BATT_LFCOLLECT_ENABLED) {
		batt_drv->hist_data = batt_hist_init_data(batt_drv->device);
		if (!batt_drv->hist_data) {
			batt_drv->blf_state = BATT_LFCOLLECT_DISABLED;
			pr_err("Cannot collect history data\n");
		}
	}

	/* google_battery expose history via a standard device */
	batt_drv->history = gbms_storage_create_device("battery_history",
						       GBMS_TAG_HIST);
	if (!batt_drv->history)
		pr_err("history not available\n");

	/* debugfs */
	(void)batt_init_fs(batt_drv);

	pr_info("init_work done\n");

	batt_drv->init_complete = true;
	batt_drv->resume_complete = true;

	schedule_delayed_work(&batt_drv->batt_work, 0);

	return;

retry_init_work:
	schedule_delayed_work(&batt_drv->init_work,
			      msecs_to_jiffies(BATT_DELAY_INIT_MS));
}

static struct thermal_zone_of_device_ops google_battery_tz_ops = {
	.get_temp = google_battery_tz_get_cycle_count,
};

static int google_battery_probe(struct platform_device *pdev)
{
	const char *fg_psy_name, *psy_name = NULL;
	struct batt_drv *batt_drv;
	int ret;
	struct power_supply_config psy_cfg = {};

	batt_drv = devm_kzalloc(&pdev->dev, sizeof(*batt_drv), GFP_KERNEL);
	if (!batt_drv)
		return -ENOMEM;

	batt_drv->device = &pdev->dev;

	ret = of_property_read_string(pdev->dev.of_node,
				      "google,fg-psy-name", &fg_psy_name);
	if (ret != 0) {
		pr_err("cannot read google,fg-psy-name, ret=%d\n", ret);
		return -EINVAL;
	}

	batt_drv->fg_psy_name =
	    devm_kstrdup(&pdev->dev, fg_psy_name, GFP_KERNEL);
	if (!batt_drv->fg_psy_name)
		return -ENOMEM;

	/* change name and type for debug/test */
	if (of_property_read_bool(pdev->dev.of_node, "google,psy-type-unknown"))
		gbatt_psy_desc.type = POWER_SUPPLY_TYPE_UNKNOWN;

	ret = of_property_read_string(pdev->dev.of_node,
				      "google,psy-name", &psy_name);
	if (ret == 0) {
		gbatt_psy_desc.name =
		    devm_kstrdup(&pdev->dev, psy_name, GFP_KERNEL);
	}

	INIT_DELAYED_WORK(&batt_drv->init_work, google_battery_init_work);
	INIT_DELAYED_WORK(&batt_drv->batt_work, google_battery_work);
	platform_set_drvdata(pdev, batt_drv);

	psy_cfg.drv_data = batt_drv;
	psy_cfg.of_node = pdev->dev.of_node;

	batt_drv->psy = devm_power_supply_register(batt_drv->device,
						   &gbatt_psy_desc, &psy_cfg);
	if (IS_ERR(batt_drv->psy)) {
		ret = PTR_ERR(batt_drv->psy);
		if (ret == -EPROBE_DEFER)
			return -EPROBE_DEFER;

		/* TODO: fail with -ENODEV */
		dev_err(batt_drv->device,
			"Couldn't register as power supply, ret=%d\n", ret);
	}

	batt_drv->ssoc_log = debugfs_logbuffer_register("ssoc");
	if (IS_ERR(batt_drv->ssoc_log)) {
		ret = PTR_ERR(batt_drv->ssoc_log);
		dev_err(batt_drv->device,
			"failed to create ssoc_log, ret=%d\n", ret);
		batt_drv->ssoc_log = NULL;
	}

	batt_drv->ttf_log = debugfs_logbuffer_register("ttf");
	if (IS_ERR(batt_drv->ttf_log)) {
		ret = PTR_ERR(batt_drv->ttf_log);
		dev_err(batt_drv->device,
			"failed to create ttf_log, ret=%d\n", ret);
		batt_drv->ttf_log = NULL;
	}

	/* Resistance Estimation configuration */
	ret = of_property_read_u32(pdev->dev.of_node, "google,res-temp-hi",
				   &batt_drv->res_state.res_temp_high);
	if (ret < 0)
		batt_drv->res_state.res_temp_high = DEFAULT_RES_TEMP_HIGH;

	ret = of_property_read_u32(pdev->dev.of_node, "google,res-temp-lo",
				   &batt_drv->res_state.res_temp_low);
	if (ret < 0)
		batt_drv->res_state.res_temp_low = DEFAULT_RES_TEMP_LOW;

	ret = of_property_read_u32(pdev->dev.of_node, "google,res-soc-thresh",
				   &batt_drv->res_state.ssoc_threshold);
	if (ret < 0)
		batt_drv->res_state.ssoc_threshold = DEFAULT_RES_SSOC_THR;

	ret = of_property_read_u32(pdev->dev.of_node, "google,res-filt-length",
				   &batt_drv->res_state.estimate_filter);
	if (ret < 0)
		batt_drv->res_state.estimate_filter = DEFAULT_RES_FILT_LEN;

	batt_drv->tz_dev = thermal_zone_of_sensor_register(batt_drv->device,
				0, batt_drv, &google_battery_tz_ops);
	if (IS_ERR(batt_drv->tz_dev)) {
		pr_err("battery tz register failed. err:%ld\n",
			PTR_ERR(batt_drv->tz_dev));
		ret = PTR_ERR(batt_drv->tz_dev);
		batt_drv->tz_dev = NULL;
	} else {
		thermal_zone_device_update(batt_drv->tz_dev, THERMAL_DEVICE_UP);
	}
	/* give time to fg driver to start */
	schedule_delayed_work(&batt_drv->init_work,
					msecs_to_jiffies(BATT_DELAY_INIT_MS));

	return 0;
}

static int google_battery_remove(struct platform_device *pdev)
{
	struct batt_drv *batt_drv = platform_get_drvdata(pdev);

	if (!batt_drv)
		return 0;

	if (batt_drv->ssoc_log)
		debugfs_logbuffer_unregister(batt_drv->ssoc_log);
	if (batt_drv->ttf_log)
		debugfs_logbuffer_unregister(batt_drv->ttf_log);
	if (batt_drv->tz_dev)
		thermal_zone_of_sensor_unregister(batt_drv->device,
				batt_drv->tz_dev);
	if (batt_drv->history)
		gbms_storage_cleanup_device(batt_drv->history);

	if (batt_drv->fg_psy)
		power_supply_put(batt_drv->fg_psy);

	batt_hist_free_data(batt_drv->hist_data);

	gbms_free_chg_profile(&batt_drv->chg_profile);

	wakeup_source_unregister(batt_drv->msc_ws);
	wakeup_source_unregister(batt_drv->batt_ws);
	wakeup_source_unregister(batt_drv->taper_ws);
	wakeup_source_unregister(batt_drv->poll_ws);

	if (batt_drv->tz_dev)
		thermal_zone_of_sensor_unregister(batt_drv->device,
				batt_drv->tz_dev);
	return 0;
}

#ifdef SUPPORT_PM_SLEEP
static int gbatt_pm_suspend(struct device *dev)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct batt_drv *batt_drv = platform_get_drvdata(pdev);

	pm_runtime_get_sync(batt_drv->device);
	batt_drv->resume_complete = false;
	pm_runtime_put_sync(batt_drv->device);

	return 0;
}

static int gbatt_pm_resume(struct device *dev)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct batt_drv *batt_drv = platform_get_drvdata(pdev);

	pm_runtime_get_sync(batt_drv->device);
	batt_drv->resume_complete = true;
	pm_runtime_put_sync(batt_drv->device);

	mod_delayed_work(system_wq, &batt_drv->batt_work, 0);

	return 0;
}

static const struct dev_pm_ops gbatt_pm_ops = {
	SET_LATE_SYSTEM_SLEEP_PM_OPS(gbatt_pm_suspend, gbatt_pm_resume)
};
#endif


static const struct of_device_id google_charger_of_match[] = {
	{.compatible = "google,battery"},
	{},
};
MODULE_DEVICE_TABLE(of, google_charger_of_match);


static struct platform_driver google_battery_driver = {
	.driver = {
		   .name = "google,battery",
		   .owner = THIS_MODULE,
		   .of_match_table = google_charger_of_match,
#ifdef SUPPORT_PM_SLEEP
		   .pm = &gbatt_pm_ops,
#endif
		   /* .probe_type = PROBE_PREFER_ASYNCHRONOUS, */
		   },
	.probe = google_battery_probe,
	.remove = google_battery_remove,
};

static int __init google_battery_init(void)
{
	int ret;

	ret = platform_driver_register(&google_battery_driver);
	if (ret < 0) {
		pr_err("device registration failed: %d\n", ret);
		return ret;
	}
	return 0;
}

static void __init google_battery_exit(void)
{
	platform_driver_unregister(&google_battery_driver);
	pr_info("unregistered platform driver\n");
}

module_init(google_battery_init);
module_exit(google_battery_exit);
MODULE_DESCRIPTION("Google Battery Driver");
MODULE_AUTHOR("AleX Pelosi <apelosi@google.com>");
MODULE_LICENSE("GPL");