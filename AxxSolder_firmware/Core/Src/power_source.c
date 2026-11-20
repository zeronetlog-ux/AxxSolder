#include "power_source.h"
#include "main.h"        /* sensor_values */
#include "stusb4500.h"
#include "sensors.h"     /* sensors_get_bus_voltage */
#include "settings.h"    /* flash_values.pd_profile */
#include "display_app.h" /* boot-time probe status screen */
#include "debug.h"
#include <stdbool.h>
#include <math.h>        /* fabsf */

/* USB-PD power limit safety margin. The negotiated contract (measured bus
 * voltage x negotiated current) is multiplied by this factor to produce the
 * value we actually allow the heater to draw, giving headroom for inrush and
 * cable losses. Set to 1.0 = "trust the contract's number." */
#define USB_PD_POWER_REDUCTION_FACTOR 1.0f

/* USB-PD fixed-supply grid.
 *
 * The STUSB4500 cannot report the source's Source_Capabilities (its message
 * buffer reads back empty and Get_Source_Cap is not answered), so the grid is
 * DISCOVERED by asking: a PDO2 request is only turned into a contract when the
 * source has a PDO at that voltage with at least that current, otherwise the
 * link falls back to PDO1 (5 V) - which is visible in the RDO.
 *
 * PD 2.0 fixed PDOs may only use 5/9/12/15/20 V, so those are the voltages to
 * probe (PDO1 = 5 V is always there and is our fallback). For every voltage the
 * maximum accepted current is searched: acceptance is monotonic in current (a
 * source either has a PDO at that voltage and accepts any current up to its
 * rating, or rejects everything), so the full rating is tried first, then the
 * smallest current to detect "no PDO at this voltage", then a binary search.
 *
 * The candidate currents are the values real PDO tables use (P/V for the usual
 * 27/36/40/45/60/65/87/90/100 W supplies), so e.g. 15 V/2.66 A or 20 V/2 A are
 * resolved exactly instead of being rounded to a 3 A rung. */
static const uint16_t pd_probe_voltages_mv[] = { 20000, 15000, 12000, 9000 };
#define PD_VOLTAGE_COUNT (sizeof(pd_probe_voltages_mv)/sizeof(pd_probe_voltages_mv[0]))

static const uint16_t pd_probe_currents_ma[] = {
	5000, 4500, 4000, 3500, 3250, 3000, 2750, 2660, 2500, 2250, 2220,
	2000, 1750, 1670, 1500, 1250, 1000,  900,  750,  700,  500
};
#define PD_CURRENT_COUNT (sizeof(pd_probe_currents_ma)/sizeof(pd_probe_currents_ma[0]))

/* Time to let a renegotiation settle before reading the RDO to learn whether
 * the requested PDO2 was accepted. */
#define PD_RENEGOTIATE_SETTLE_MS 300

/* --- Public state --- */
power_source_t power_source = POWER_DC;
pd_status_t    pd_status    = PD_STATUS_NO_CHIP;
uint8_t        pd_chip_id;

/* Probe results, filled by power_source_init() and read by the boot banner. */
pd_offer_t pd_offers[PD_OFFER_MAX];
uint8_t    pd_offer_count;
uint8_t    pd_offer_selected = PD_OFFER_NONE;

/* Apply PDO1=5V/3A baseline only (valid count 1) and renegotiate. This is the
 * guaranteed-safe contract: the source can always meet 5 V, so the MCU stays
 * powered no matter what. Used as the starting point and the final fallback. */
static void request_5v_baseline(void){
	stusb_update_pdo(1, 5000, 3000);
	stusb_set_valid_pdo(1);
	stusb_soft_reset();
}

/* Wait for VBUS to ramp to the newly negotiated voltage and hold steady.
 *
 * IMPORTANT: immediately after a soft-reset the OLD contract can still report
 * SNK_READY, and VBUS only ramps once the source accepts the new request. So we
 * must NOT sample right away (that reads the stale, lower voltage). Wait a fixed
 * minimum for the transition to begin, then poll the instantaneous bus voltage
 * until it has held steady. */
static void wait_for_vbus_settle(void){
	HAL_Delay(300);                 /* let the old contract tear down & VBUS start ramping */
	float v_prev = sensors_read_bus_voltage_instant();
	uint8_t stable_ticks = 0;
	uint32_t settle_timeout = HAL_GetTick() + 2000;
	while(HAL_GetTick() < settle_timeout){
		HAL_Delay(50);
		float v_now = sensors_read_bus_voltage_instant();
		stable_ticks = (fabsf(v_now - v_prev) < 0.2f) ? (stable_ticks + 1) : 0;
		v_prev = v_now;
		if(stable_ticks >= 3){ break; }   /* steady for ~150 ms => settled */
	}
}

/* Wait (bounded) for the STUSB4500 to report a settled contract. */
static bool wait_for_sink_ready(uint32_t timeout_ms){
	uint32_t deadline = HAL_GetTick() + timeout_ms;

	for(;;){
		if(stusb_is_sink_ready()){
			return true;
		}
		if(HAL_GetTick() >= deadline){
			return false;
		}
		HAL_Delay(10);
	}
}

/* Adopt the given contract: source = USB-PD, and the heater power limit becomes
 * the measured bus voltage times the negotiated operating current. */
static void apply_contract(const STUSB_GEN1S_RDO_REG_STATUS_RegTypeDef *rdo){
	power_source = POWER_USB;
	pd_status    = PD_STATUS_OK;

	/* PRIME the bus-voltage moving average so the displayed input voltage shows
	 * the negotiated value immediately instead of ramping across the window. */
	sensors_prime_bus_voltage();
	sensors_get_bus_voltage();

	float current_a = rdo->b.OperatingCurrent * 0.01f;   /* 10 mA units -> A */
	sensor_values.USB_PD_power_limit =
		sensor_values.bus_voltage * current_a * USB_PD_POWER_REDUCTION_FACTOR;

	debug_print_int(DEBUG_INFO,"USB-PD power limit (W)", (int)sensor_values.USB_PD_power_limit);
}

/* Ask the source for one (mV, mA) profile. Returns true when the contract was
 * formed at that voltage (RDO Object_Pos >= 2 means our PDO2 matched a source
 * PDO), false when the source refused and the link fell back to 5 V. */
static bool probe_profile(uint16_t mv, uint16_t ma){
	STUSB_GEN1S_RDO_REG_STATUS_RegTypeDef rdo = {0};

	LCD_show_pd_probe_trying(mv, ma);      /* live feedback on the boot screen */

	stusb_update_pdo(2, mv, ma);
	stusb_set_valid_pdo(2);
	stusb_soft_reset();
	HAL_Delay(PD_RENEGOTIATE_SETTLE_MS);

	if(stusb_read_rdo(&rdo) != HAL_OK){
		return false;
	}

	debug_print_int(DEBUG_INFO,"Probe mA", ma);
	debug_print_int(DEBUG_INFO," -> Object_Pos", rdo.b.Object_Pos);

	return (rdo.b.Object_Pos >= 2);
}

/* Highest current the source accepts at `mv`; 0 when it has no PDO there.
 * Acceptance is monotonic in current, so: try the full rating first (one probe
 * for a 5 A supply), then the smallest current to detect "no PDO at this
 * voltage", then binary search the bracket. */
static uint16_t probe_voltage(uint16_t mv){
	if(probe_profile(mv, pd_probe_currents_ma[0])){
		return pd_probe_currents_ma[0];
	}
	if(!probe_profile(mv, pd_probe_currents_ma[PD_CURRENT_COUNT - 1])){
		return 0;                                   /* no PDO at this voltage */
	}

	uint8_t lo = 0;                                 /* known rejected, high I  */
	uint8_t hi = PD_CURRENT_COUNT - 1;              /* known accepted, low I   */

	while((uint8_t)(hi - lo) > 1u){
		uint8_t mid = (uint8_t)(((uint16_t)lo + hi) / 2u);
		if(probe_profile(mv, pd_probe_currents_ma[mid])){
			hi = mid;
		}
		else{
			lo = mid;
		}
	}

	return pd_probe_currents_ma[hi];
}

/* Request one discovered profile and adopt the resulting contract. */
static bool request_offer(uint8_t index){
	STUSB_GEN1S_RDO_REG_STATUS_RegTypeDef rdo = {0};

	stusb_update_pdo(2, pd_offers[index].mv, pd_offers[index].ma);
	stusb_set_valid_pdo(2);
	stusb_soft_reset();
	HAL_Delay(PD_RENEGOTIATE_SETTLE_MS);

	if((stusb_read_rdo(&rdo) != HAL_OK) || (rdo.b.Object_Pos < 2)){
		return false;          /* rejected: the link fell back to 5 V */
	}

	wait_for_vbus_settle();
	apply_contract(&rdo);
	pd_offer_selected = index;

	debug_print_int(DEBUG_INFO,"PD profile mV", pd_offers[index].mv);
	debug_print_int(DEBUG_INFO,"PD profile mA", pd_offers[index].ma);
	return true;
}

/* check STUSB4500 */
void power_source_init(void){
	if(stusb_check_connection() != HAL_OK){
		//do error handling for STUSB
		pd_chip_id = stusb_chip_id;
		pd_status  = PD_STATUS_NO_CHIP;
		debug_print_str(DEBUG_ERROR,"STUSB4500 unavailable");
		return;
	}
	pd_chip_id = stusb_chip_id;
	debug_print_str(DEBUG_INFO,"STUSB4500 found");

	stusb_init();

	/* Is a USB-C cable actually attached? */
	if(!stusb_is_sink_connected()){
		pd_status = PD_STATUS_NO_SINK;
		debug_print_str(DEBUG_INFO,"No USB-PD sink connected");
		return;
	}

	STUSB_GEN1S_RDO_REG_STATUS_RegTypeDef rdo = {0};

	/* Put the probe screen up immediately - the discovery below takes several
	 * seconds and the panel would otherwise stay black until then. */
	LCD_show_pd_probe_start(pd_probe_voltages_mv, (uint8_t)PD_VOLTAGE_COUNT);

	/* Wait for the chip to report a settled contract. Note the STUSB4500 keeps
	 * its state across an MCU reset (it is powered from VBUS), so on a plain
	 * reboot a contract negotiated by an earlier run is still in force. */
	(void)wait_for_sink_ready(1500);
	HAL_Delay(100);                       /* let VBUS finish settling */

	/* Discover the source's fixed-supply grid: for each PD voltage find the
	 * highest current the charger accepts (see probe_voltage()). PDO1 (5 V) is
	 * left untouched, so every rejected probe falls back to a safe 5 V contract
	 * - no brown-out. This takes a few seconds (up to ~6 probes per voltage),
	 * which is fine at boot and buys an exact picture of the supply. */
	pd_offer_count = 0;

	for(uint8_t v = 0; v < PD_VOLTAGE_COUNT; v++){
		uint16_t mv = pd_probe_voltages_mv[v];
		uint16_t ma = probe_voltage(mv);

		LCD_show_pd_probe_result(v, mv, ma);

		if(ma == 0u){
			debug_print_int(DEBUG_INFO,"PD no PDO at mV", mv);
			continue;
		}

		pd_offers[pd_offer_count].mv       = mv;
		pd_offers[pd_offer_count].ma       = ma;
		pd_offers[pd_offer_count].accepted = true;
		pd_offer_count++;
	}

	/* Highest power first, so entry 0 is the automatic choice. */
	for(uint8_t i = 1; i < pd_offer_count; i++){
		pd_offer_t tmp = pd_offers[i];
		uint32_t p = (uint32_t)tmp.mv * tmp.ma;
		int8_t   j = (int8_t)(i - 1);

		while((j >= 0) && (((uint32_t)pd_offers[j].mv * pd_offers[j].ma) < p)){
			pd_offers[j + 1] = pd_offers[j];
			j--;
		}
		pd_offers[j + 1] = tmp;
	}

	/* Pick the profile to run on: the user's stored preference (a voltage) when
	 * the source still offers it, otherwise the most powerful discovered one. */
	uint8_t chosen = 0;
	float   pref   = flash_values.pd_profile;

	if(pref > 0.0f){
		for(uint8_t i = 0; i < pd_offer_count; i++){
			if(fabsf(((float)pd_offers[i].mv / 1000.0f) - pref) < 0.6f){
				chosen = i;
				debug_print_str(DEBUG_INFO,"PD: using stored voltage");
				break;
			}
		}
	}

	if(pd_offer_count > 0u){
		pd_offer_selected = chosen;
		if(request_offer(chosen)){
			return;                       /* contract adopted and applied */
		}
		debug_print_str(DEBUG_WARNING,"Chosen profile rejected - falling back");
	}

	/* Nothing usable: hold the guaranteed-safe 5 V baseline. */
	pd_offer_selected = PD_OFFER_NONE;
	debug_print_str(DEBUG_WARNING,"No high-power PDO accepted - holding 5V");
	request_5v_baseline();
	HAL_Delay(PD_RENEGOTIATE_SETTLE_MS);

	if((stusb_read_rdo(&rdo) != HAL_OK) || (rdo.b.Object_Pos == 0)){
		pd_status = PD_STATUS_NO_CONTRACT;
		debug_print_str(DEBUG_INFO,"No USB-PD contract");
		return;   /* leave power_source = POWER_DC */
	}

	/* Let VBUS settle at the negotiated voltage before measuring it. */
	wait_for_vbus_settle();
	apply_contract(&rdo);
}

/* Switch the link to one of the discovered profiles and recompute the power
 * limit. Used by the boot-time mode chooser: the caller persists the choice so
 * the next start uses it directly. */
bool power_source_apply_offer(uint8_t index){
	if((index >= pd_offer_count) || !pd_offers[index].accepted){
		return false;
	}

	uint8_t previous = pd_offer_selected;

	if(request_offer(index)){
		return true;
	}

	/* The source changed its mind: keep the station alive on the profile it
	 * was using before. */
	debug_print_str(DEBUG_WARNING,"PD profile switch failed");
	if((previous != PD_OFFER_NONE) && (previous != index)){
		(void)request_offer(previous);
	}
	return false;
}
