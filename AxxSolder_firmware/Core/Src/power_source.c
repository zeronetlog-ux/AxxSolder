#include "power_source.h"
#include "main.h"        /* sensor_values */
#include "stusb4500.h"
#include "sensors.h"     /* sensors_get_bus_voltage */
#include "settings.h"    /* flash_values.pd_profile */
#include "display_app.h" /* NVM dump screen (STUSB_DEBUG builds) */
#include "debug.h"
#include <stdbool.h>
#include <math.h>        /* fabsf */

/* The station itself draws power from the same contract as the heater (MCU,
 * display, gate driver): roughly 1 W regardless of the bus voltage. Letting the
 * heater use the whole contract means the port sees more than it granted - at a
 * 5 V contract ~14 W of tip power is already most of 3 A and the logic comes on
 * top. Subtracting a fixed power keeps the headroom in the right proportion:
 * 1 W is ~50 mA at 20 V (a 30 W contract gives the tip 29 W) and 200 mA at 5 V,
 * where input current, not power, is the scarce resource. */
#define STATION_AUX_POWER_W 1.0f

/* Where the supply profiles come from.
 *
 * The STUSB4500 does expose the source's Source_Capabilities: a soft reset makes
 * the source repeat them and the message lands in the chip's RX buffer (see
 * stusb_read_source_pdos()). The offers are therefore taken from the source
 * itself - no probing of voltage/current combinations.
 *
 * When the message cannot be read the chip's own NVM contract (PDO1, 5 V) is
 * left untouched, so the station keeps running on USB default power instead of
 * losing VBUS.
 */
#define PD_CAPS_TRY_MS   80u   /* poll window after one soft reset             */
#ifndef PD_CAPS_ATTEMPTS
#define PD_CAPS_ATTEMPTS 4u    /* soft resets before giving up                 */
#endif

/* --- Public state --- */
power_source_t power_source = POWER_DC;
pd_status_t    pd_status    = PD_STATUS_NO_CHIP;
uint8_t        pd_chip_id;
bool           pd_cable_attached;

/* Result of the last profile switch, for the UI (see power_source.h). */
pd_switch_result_t pd_last_switch = PD_SWITCH_OK;

/* Probe results, filled by power_source_init() and read by the boot banner. */
pd_offer_t pd_offers[PD_OFFER_MAX];
uint8_t    pd_offer_count;
uint8_t    pd_offer_selected = PD_OFFER_NONE;
uint8_t    pd_offer_default  = PD_OFFER_NONE;

/* True when the source's capability list could not be read (5 V baseline). */
static bool caps_unreadable;

/* Wait for VBUS to ramp to the newly negotiated voltage and hold steady.
 *
 * IMPORTANT: immediately after a soft-reset the OLD contract can still report
 * SNK_READY, and VBUS only ramps once the source accepts the new request. So we
 * must NOT sample right away (that reads the stale, lower voltage). Wait a fixed
 * minimum for the transition to begin, then poll the instantaneous bus voltage
 * until it has held steady. */
/* Wait for VBUS to reach `target_v` (up to 1.5 s) and then to hold steady for
 * ~150 ms. Call with target_v = 0 to wait for stability only.
 *
 * The target matters: a source answers PS_RDY before VBUS has actually moved, so
 * "the voltage stopped changing" is true on the OLD value for a while. Reading
 * the contract then describes the previous mode - that is how a 15 V/2 A
 * contract ended up reported as 17 W (20 V x the old 1 A). */
static void wait_for_vbus_settle(float target_v){
	uint32_t deadline = HAL_GetTick() + 1500;
	float    v_prev;
	uint8_t  stable_ticks = 0;
	uint32_t settle_timeout;

	if(target_v > 0.0f){
		while(HAL_GetTick() < deadline){
			if(fabsf(sensors_read_bus_voltage_instant() - target_v) < 0.5f){
				break;
			}
			HAL_Delay(20);
		}
	}
	else{
		HAL_Delay(150);            /* let the old contract tear down */
	}

	v_prev = sensors_read_bus_voltage_instant();
	settle_timeout = HAL_GetTick() + 2000;
	while(HAL_GetTick() < settle_timeout){
		HAL_Delay(25);
		float v_now = sensors_read_bus_voltage_instant();
		stable_ticks = (fabsf(v_now - v_prev) < 0.2f) ? (uint8_t)(stable_ticks + 1u) : 0u;
		v_prev = v_now;
		if(stable_ticks >= 3){ break; }   /* steady for ~75 ms => settled */
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
 * the measured bus voltage times the negotiated operating current minus the
 * current the station itself needs. `req_mv`/`req_ma` are what was requested
 * (0/0 when the controller's own contract simply stands). */
static void apply_contract(const STUSB_GEN1S_RDO_REG_STATUS_RegTypeDef *rdo,
                           uint16_t req_mv, uint16_t req_ma,
                           const stusb_dpm_trace_t *trace){
	power_source = POWER_USB;
	pd_status    = PD_STATUS_OK;

	/* PRIME the bus-voltage moving average so the displayed input voltage shows
	 * the negotiated value immediately instead of ramping across the window. */
	sensors_prime_bus_voltage();
	sensors_get_bus_voltage();

	float contract_w = sensor_values.bus_voltage * (rdo->b.OperatingCurrent * 0.01f);
	float heater_w   = contract_w - STATION_AUX_POWER_W;

	if(heater_w < 0.0f){
		heater_w = 0.0f;
	}

	/* The UI reports what the source granted ("PD 30W"); the heater gets that
	 * minus the station's own supply, which stays inside the firmware. */
	sensor_values.USB_PD_contract_power = contract_w;
	sensor_values.USB_PD_power_limit    = heater_w;

	debug_print_int(DEBUG_INFO,"USB-PD contract (W)", (int)sensor_values.USB_PD_contract_power);
	debug_print_int(DEBUG_INFO,"USB-PD power limit (W)", (int)sensor_values.USB_PD_power_limit);

#ifdef STUSB_DEBUG
	/* Diagnostic build: show what was asked for, what the controller reports and
	 * what the limit became, so a wrong number can be traced without a console. */
	LCD_show_pd_apply((float)req_mv / 1000.0f, (float)req_ma / 1000.0f,
	                  sensor_values.bus_voltage, rdo->d32,
	                  (uint8_t)rdo->b.Object_Pos,
	                  (float)rdo->b.MaxCurrent * 0.01f,
	                  (float)rdo->b.OperatingCurrent * 0.01f,
	                  sensor_values.USB_PD_contract_power,
	                  sensor_values.USB_PD_power_limit, trace);
	HAL_Delay(8000);
#endif
}

/* Build pd_offers[] from the source's own capability list, highest power first.
 * Returns the number of usable entries (0 = the list could not be read; the
 * caller then falls back to the chip's NVM contract). */
static uint8_t discover_offers(void){
	stusb_pdo_t source[STUSB_SRC_PDO_MAX];
	uint8_t     count;
	uint8_t     offers = 0;

	count = 0;
	if (PD_CAPS_ATTEMPTS > 0u) {
		/* The capabilities are captured without touching the sink PDOs: the soft
		 * reset makes the chip re-request the profile it already negotiated, so
		 * VBUS stays where it is and the list arrives without a voltage jump.
		 * stusb_read_source_pdos() first tries the RX buffer (the message the
		 * controller received at plug-in) and only then resets the link. */
		count = stusb_read_source_pdos(source, STUSB_SRC_PDO_MAX,
		                               PD_CAPS_TRY_MS, PD_CAPS_ATTEMPTS);
	}

	debug_print_int(DEBUG_INFO,"PD source PDOs", count);

	for(uint8_t i = 0; i < count; i++){
		/* 5 V is the minimum a fixed PDO may carry; anything below that is a
		 * decoding artefact and is not worth negotiating. */
		if((source[i].mv < 5000u) || (source[i].ma < 100u)){
			continue;
		}

		pd_offers[offers].mv       = source[i].mv;
		pd_offers[offers].ma       = source[i].ma;
		pd_offers[offers].accepted = true;
		offers++;
	}

	/* Highest power first; on equal power the higher voltage goes first, so the
	 * list reads 20/15/12/9/5 whatever order the source used. */
	for(uint8_t i = 1; i < offers; i++){
		pd_offer_t tmp = pd_offers[i];
		uint32_t p = (uint32_t)tmp.mv * tmp.ma;
		int8_t   j = (int8_t)(i - 1);

		while(j >= 0){
			uint32_t pj = (uint32_t)pd_offers[j].mv * pd_offers[j].ma;

			if((pj > p) || ((pj == p) && (pd_offers[j].mv >= tmp.mv))){
				break;
			}
			pd_offers[j + 1] = pd_offers[j];
			j--;
		}
		pd_offers[j + 1] = tmp;
	}

	return offers;
}

/* Request one of the discovered profiles and adopt the resulting contract.
 *
 * One request is not always enough: the controller sometimes reports the old
 * contract right after the exchange, or falls back to the profile stored in its
 * own NVM while the source keeps 5 V. The link is therefore verified against
 * the *measured* bus voltage and against the granted current, and the request
 * is repeated once before the mode is reported as failed. The best contract
 * seen during the attempts is the one adopted. */
static bool request_offer(uint8_t index){
	STUSB_GEN1S_RDO_REG_STATUS_RegTypeDef rdo      = {0};
	STUSB_GEN1S_RDO_REG_STATUS_RegTypeDef rdo_best = {0};
	stusb_dpm_trace_t trace;
	stusb_contract_t  result;
	float             target_v  = (float)pd_offers[index].mv / 1000.0f;
	uint16_t          want_ma   = pd_offers[index].ma;
	float             measured;
	bool              have_rdo  = false;
	bool              full_ma   = false;

	pd_last_switch = PD_SWITCH_OK;

	for(uint8_t attempt = 1u; attempt <= 2u; attempt++){
		result = stusb_request_contract(pd_offers[index].mv, pd_offers[index].ma, &trace);
		if(result != STUSB_CONTRACT_OK){
			pd_last_switch = (result == STUSB_CONTRACT_IO)      ? PD_SWITCH_I2C :
			                 (result == STUSB_CONTRACT_REJECTED) ? PD_SWITCH_REJECTED :
			                                                       PD_SWITCH_TIMEOUT;
			debug_print_str(DEBUG_WARNING,"PD switch: request failed");
			if((result == STUSB_CONTRACT_IO) && (attempt == 1u)){
				continue;               /* bus hiccup: one more try */
			}
			break;
		}

		/* Let the source actually move before the contract is read - PS_RDY
		 * arrives while VBUS is still on the old value (see
		 * wait_for_vbus_settle()). */
		wait_for_vbus_settle(target_v);

		/* Accept/PS_RDY and a non-zero RDO position do not prove that the
		 * station runs on the requested mode: a controller that falls back to
		 * the profile in its own NVM, or a source that ignored the request,
		 * still reports the old 5 V contract (object position 1). Only VBUS
		 * says what is really there. */
		measured = sensors_read_bus_voltage_instant();
		if(fabsf(measured - target_v) > 1.0f){
			pd_last_switch = PD_SWITCH_NO_VBUS;
			debug_print_int(DEBUG_WARNING,"PD switch mV", (int)(measured * 1000.0f));
			debug_print_str(DEBUG_WARNING,"PD switch: VBUS did not move");
			continue;                   /* retry once, then give up below */
		}

		if((stusb_read_rdo(&rdo) != HAL_OK) || (rdo.b.Object_Pos == 0u) ||
		   (rdo.b.OperatingCurrent == 0u)){
			pd_last_switch = PD_SWITCH_I2C;
			continue;
		}

		pd_last_switch = PD_SWITCH_OK;

		/* Keep the contract with the most current: a second attempt must not
		 * replace a good result with a poorer one. */
		if(!have_rdo ||
		   (((uint32_t)rdo.b.OperatingCurrent * 10u) >
		    ((uint32_t)rdo_best.b.OperatingCurrent * 10u))){
			rdo_best = rdo;
			have_rdo = true;
		}
		full_ma = (((uint32_t)rdo.b.OperatingCurrent * 10u) >= (uint32_t)want_ma);

		if(full_ma || (attempt == 2u)){
			break;
		}
		/* The link came up with less current than the source offers for this
		 * voltage - ask again once before accepting the lower contract. */
		debug_print_str(DEBUG_WARNING,"PD switch: retry for full current");
	}

	if(!have_rdo){
		return false;               /* the link stays where it was */
	}

	apply_contract(&rdo_best, pd_offers[index].mv, pd_offers[index].ma, &trace);
	pd_offer_selected = index;

	debug_print_int(DEBUG_INFO,"PD profile mV", pd_offers[index].mv);
	debug_print_int(DEBUG_INFO,"PD profile mA", pd_offers[index].ma);
	debug_print_int(DEBUG_INFO,"PD granted mA", (int)rdo_best.b.OperatingCurrent * 10);
	return true;
}

/* check STUSB4500 */
void power_source_init(void){
	pd_cable_attached = false;

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

#if defined(STUSB_DEBUG) && defined(STUSB_NVM_DUMP)
	/* Diagnostic build: dump the controller's own NVM once, so a board that
	 * negotiates an unexpectedly low 5 V profile can be compared with a known
	 * good one (see LCD_show_nvm_dump()). */
	{
		uint8_t nvm[STUSB_NVM_SECTORS * STUSB_NVM_SECTOR_SIZE];

		if(stusb_nvm_read(nvm) == HAL_OK){
			LCD_show_nvm_dump(nvm);
			HAL_Delay(8000);
		}
	}
#endif

	/* Is a USB-C cable actually attached? */
	if(!stusb_is_sink_connected()){
		pd_status = PD_STATUS_NO_SINK;
		debug_print_str(DEBUG_INFO,"No USB-PD sink connected");
		return;
	}
	pd_cable_attached = true;

	float boot_voltage = 0.0f;

	/* The contract is deliberately NOT negotiated here: the chooser only needs
	 * the list, and waiting for the PD exchange (PS_RDY, voltage ramp, settle)
	 * would hold the window back by up to a second. The chosen - or stored -
	 * profile is applied from main() after the chooser (see
	 * power_source_apply_offer()), and power_source_adopt_contract() covers the
	 * case where the source advertises nothing. */

	/* Wait for the chip to report a settled contract. Note the STUSB4500 keeps
	 * its state across an MCU reset (it is powered from VBUS), so on a plain
	 * reboot a contract negotiated by an earlier run is still in force. */
	(void)wait_for_sink_ready(1500);

	/* Read the source's own capability list, whatever voltage the controller
	 * happened to negotiate on its own: the station must offer what the supply
	 * can actually deliver, not what its controller NVM says. The soft reset
	 * inside discover_offers() needs the settled contract above - before that
	 * the source may ignore it, which would look like "capabilities
	 * unreadable". */
	boot_voltage    = sensors_read_bus_voltage_instant();
	debug_print_int(DEBUG_INFO,"PD boot mV", (int)(boot_voltage * 1000.0f));

	pd_offer_count  = discover_offers();
	caps_unreadable = (pd_offer_count == 0u);

	/* Pick the mode the station falls back to when the chooser closes without a
	 * selection: the user's stored preference (a voltage) when the source still
	 * offers it, otherwise the most powerful advertised one. */
	if(pd_offer_count > 0u){
		uint8_t preferred = 0;
		float   pref      = flash_values.pd_profile;

		if(pref > 0.0f){
			for(uint8_t i = 0; i < pd_offer_count; i++){
				if(fabsf(((float)pd_offers[i].mv / 1000.0f) - pref) < 0.6f){
					preferred = i;
					debug_print_str(DEBUG_INFO,"PD: using stored voltage");
					break;
				}
			}
		}

		pd_offer_default = preferred;
		debug_print_int(DEBUG_INFO,"PD default offer", (int)pd_offer_default);
	}
	else if(caps_unreadable){
		pd_status = PD_STATUS_NO_CAPS;      /* drives the chooser text */
		debug_print_str(DEBUG_WARNING,"PD capabilities unreadable");
	}
	else{
		/* 5 V link: nothing was asked, the chip's own contract stays. */
		debug_print_str(DEBUG_INFO,"PD link left as negotiated");
	}
}

/* No advertised modes: keep the contract the chip negotiated on its own from its
 * NVM settings (PDO1, 5 V on a plain supply; whatever the source and the NVM
 * profile agree on otherwise). Nothing is written to the DPM registers, so VBUS
 * never drops - only the power limit is derived from the RDO. Called after the
 * chooser, so the window does not wait for it. */
void power_source_adopt_contract(void){
	STUSB_GEN1S_RDO_REG_STATUS_RegTypeDef rdo = {0};

	if((stusb_read_rdo(&rdo) != HAL_OK) || (rdo.b.Object_Pos == 0)){
		pd_status = PD_STATUS_NO_CONTRACT;
		debug_print_str(DEBUG_INFO,"No USB-PD contract");
		return;   /* leave power_source = POWER_DC */
	}

	/* Let VBUS settle at the negotiated voltage before measuring it. */
	wait_for_vbus_settle(0.0f);
	apply_contract(&rdo, 0u, 0u, NULL);

	if(caps_unreadable){
		/* The contract is fine, only the capability list is missing - keep that
		 * visible instead of letting apply_contract() report a clean OK (the
		 * info line then shows "PD?R" next to the input voltage). */
		pd_status = PD_STATUS_NO_CAPS;
	}
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

	/* Keep the reason of the requested switch: the restore below may fail too
	 * and would otherwise overwrite pd_last_switch with its own reason. */
	pd_switch_result_t reason = pd_last_switch;

	/* The source changed its mind: keep the station alive on the profile it
	 * was using before. */
	debug_print_str(DEBUG_WARNING,"PD profile switch failed");
	if((previous != PD_OFFER_NONE) && (previous != index)){
		(void)request_offer(previous);
	}
	pd_last_switch = reason;
	return false;
}

const char *power_source_switch_error(void)
{
	switch(pd_last_switch){
	case PD_SWITCH_I2C:      return "PD: I2C FAIL";
	case PD_SWITCH_REJECTED: return "PD: REJECTED";
	case PD_SWITCH_TIMEOUT:  return "PD: NO REPLY";
	case PD_SWITCH_NO_VBUS:  return "PD: VBUS 5V";
	default:                 return "PD: NO SWITCH";
	}
}
