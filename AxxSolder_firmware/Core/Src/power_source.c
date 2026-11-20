#include "power_source.h"
#include "main.h"        /* sensor_values */
#include "stusb4500.h"
#include "sensors.h"     /* sensors_get_bus_voltage */
#include "settings.h"    /* flash_values.pd_profile */
#include "display_app.h" /* NVM dump screen (STUSB_DEBUG builds) */
#include "debug.h"
#include <stdbool.h>
#include <math.h>        /* fabsf */

/* The station itself draws current from the same contract as the heater (MCU,
 * display, gate driver). Letting the heater use the full contract current means
 * the port sees more than it granted - which is what happens at a 5 V contract,
 * where ~15 W of tip power is already the whole 3 A and the logic comes on top.
 * Subtracting a small fixed current keeps that headroom and, unlike a blanket
 * percentage, costs almost nothing at 20 V. */
#define STATION_AUX_CURRENT_A 0.15f

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
#define PD_CAPS_TRY_MS   120u  /* poll window after one soft reset             */
#ifndef PD_CAPS_ATTEMPTS
#define PD_CAPS_ATTEMPTS 4u    /* soft resets before giving up                 */
#endif

/* Only renegotiate when the link already runs above this voltage.
 *
 * The controller negotiates a contract from its own NVM as soon as the cable is
 * plugged in, before this code runs, so the bus voltage says what kind of source
 * is attached: above 5 V it is a supply that speaks PD and has headroom to gain,
 * at 5 V it is a plain 5 V source or a laptop port. Those restart when the link
 * is touched during boot - a station on a MacBook's USB-C port bootloops, while
 * the very same station behind a USB hub (which does no PD at all) is fine. So
 * 5 V links are left exactly as the chip negotiated them. */
#define PD_RENEGOTIATE_MIN_V 9.0f

/* --- Public state --- */
power_source_t power_source = POWER_DC;
pd_status_t    pd_status    = PD_STATUS_NO_CHIP;
uint8_t        pd_chip_id;

/* Probe results, filled by power_source_init() and read by the boot banner. */
pd_offer_t pd_offers[PD_OFFER_MAX];
uint8_t    pd_offer_count;
uint8_t    pd_offer_selected = PD_OFFER_NONE;

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
 * the measured bus voltage times the negotiated operating current minus the
 * current the station itself needs. */
static void apply_contract(const STUSB_GEN1S_RDO_REG_STATUS_RegTypeDef *rdo){
	power_source = POWER_USB;
	pd_status    = PD_STATUS_OK;

	/* PRIME the bus-voltage moving average so the displayed input voltage shows
	 * the negotiated value immediately instead of ramping across the window. */
	sensors_prime_bus_voltage();
	sensors_get_bus_voltage();

	float current_a = rdo->b.OperatingCurrent * 0.01f;   /* 10 mA units -> A */
	float heater_a  = current_a - STATION_AUX_CURRENT_A;

	if(heater_a < 0.0f){
		heater_a = 0.0f;
	}

	sensor_values.USB_PD_power_limit = sensor_values.bus_voltage * heater_a;

	debug_print_int(DEBUG_INFO,"USB-PD power limit (W)", (int)sensor_values.USB_PD_power_limit);
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
		 * VBUS stays where it is and the list arrives without a voltage jump. The
		 * 5 V case never gets here (see PD_RENEGOTIATE_MIN_V), so the park-at-5 V
		 * step that used to sit here only caused a 20 -> 5 -> 20 V dip on a real
		 * PD supply. */
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

	/* Highest power first, so entry 0 is the automatic choice. */
	for(uint8_t i = 1; i < offers; i++){
		pd_offer_t tmp = pd_offers[i];
		uint32_t p = (uint32_t)tmp.mv * tmp.ma;
		int8_t   j = (int8_t)(i - 1);

		while((j >= 0) && (((uint32_t)pd_offers[j].mv * pd_offers[j].ma) < p)){
			pd_offers[j + 1] = pd_offers[j];
			j--;
		}
		pd_offers[j + 1] = tmp;
	}

	return offers;
}

/* Request one of the discovered profiles and adopt the resulting contract. */
static bool request_offer(uint8_t index){
	STUSB_GEN1S_RDO_REG_STATUS_RegTypeDef rdo = {0};

	if(stusb_request_contract(pd_offers[index].mv, pd_offers[index].ma) != STUSB_CONTRACT_OK){
		return false;          /* rejected or no answer: the link stays at 5 V */
	}

	if(stusb_read_rdo(&rdo) != HAL_OK){
		return false;
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

#ifdef STUSB_DEBUG
	/* Diagnostic build: dump the controller's own NVM settings on the screen.
	 * It negotiates its first contract from them before the MCU is up, so they
	 * are what a source (a laptop port, for instance) sees at plug-in. Held long
	 * enough to photograph, which avoids needing a serial console. */
	{
		uint8_t nvm[STUSB_NVM_SECTORS * STUSB_NVM_SECTOR_SIZE];

		if(stusb_nvm_read(nvm) == HAL_OK){
			LCD_show_nvm_dump(nvm);
			HAL_Delay(8000);
		}
	}
#endif

	STUSB_GEN1S_RDO_REG_STATUS_RegTypeDef rdo = {0};
	bool  caps_unreadable = false;
	float boot_voltage    = 0.0f;

	/* Wait for the chip to report a settled contract. Note the STUSB4500 keeps
	 * its state across an MCU reset (it is powered from VBUS), so on a plain
	 * reboot a contract negotiated by an earlier run is still in force. */
	(void)wait_for_sink_ready(1500);
	HAL_Delay(100);                       /* let VBUS finish settling */

	/* Read the source's own capability list - but only when there is something to
	 * gain and the source has already proven it speaks PD (see
	 * PD_RENEGOTIATE_MIN_V). At 5 V nothing is asked at all. The soft reset
	 * inside discover_offers() needs the settled contract above - before that the
	 * source may ignore it, which would look like "capabilities unreadable". */
	boot_voltage    = sensors_read_bus_voltage_instant();
	debug_print_int(DEBUG_INFO,"PD boot mV", (int)(boot_voltage * 1000.0f));

	pd_offer_count  = 0;
	if(boot_voltage >= PD_RENEGOTIATE_MIN_V){
		pd_offer_count  = discover_offers();
		caps_unreadable = (pd_offer_count == 0u);
	}

	/* Pick the profile to run on: the user's stored preference (a voltage) when
	 * the source still offers it, otherwise the most powerful advertised one. */
	if(pd_offer_count > 0u){
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

		pd_offer_selected = chosen;
		if(request_offer(chosen)){
			return;                       /* contract adopted and applied */
		}
		debug_print_str(DEBUG_WARNING,"Chosen profile rejected - falling back");
	}
	else if(caps_unreadable){
		debug_print_str(DEBUG_WARNING,"PD capabilities unreadable");
	}
	else{
		/* 5 V link: nothing was asked, the chip's own contract stays. */
		debug_print_str(DEBUG_INFO,"PD link left as negotiated");
	}

	/* Nothing selectable: keep the contract the chip negotiated on its own from
	 * its NVM settings (PDO1, 5 V on a plain supply; whatever the source and the
	 * NVM profile agree on otherwise). Nothing is written to the DPM registers in
	 * that case, so VBUS never drops - only the power limit is derived from the
	 * RDO. */
	pd_offer_selected = PD_OFFER_NONE;

	if((stusb_read_rdo(&rdo) != HAL_OK) || (rdo.b.Object_Pos == 0)){
		pd_status = PD_STATUS_NO_CONTRACT;
		debug_print_str(DEBUG_INFO,"No USB-PD contract");
		return;   /* leave power_source = POWER_DC */
	}

	/* Let VBUS settle at the negotiated voltage before measuring it. */
	wait_for_vbus_settle();
	apply_contract(&rdo);

	if(caps_unreadable){
		/* The 5 V contract is fine, only the capability list is missing - keep
		 * that visible instead of letting apply_contract() report a clean OK
		 * (the info line then shows "PD?R" next to the input voltage). */
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

	/* The source changed its mind: keep the station alive on the profile it
	 * was using before. */
	debug_print_str(DEBUG_WARNING,"PD profile switch failed");
	if((previous != PD_OFFER_NONE) && (previous != index)){
		(void)request_offer(previous);
	}
	return false;
}
