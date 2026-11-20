#include "power_source.h"
#include "main.h"        /* sensor_values */
#include "stusb4500.h"
#include "sensors.h"     /* sensors_get_bus_voltage */
#include "debug.h"
#include <stdbool.h>
#include <math.h>        /* fabsf */

/* USB-PD power limit safety margin. The negotiated contract (measured bus
 * voltage x negotiated current) is multiplied by this factor to produce the
 * value we actually allow the heater to draw, giving headroom for inrush and
 * cable losses. Set to 1.0 = "trust the contract's number." */
#define USB_PD_POWER_REDUCTION_FACTOR 1.0f

/* Candidate sink profiles, requested HIGHEST POWER FIRST. We can't reliably
 * read the charger's advertised capabilities on this STUSB4500 (the PD message
 * buffer poll returns nothing here), so instead of guessing we PROBE: request
 * each profile in turn and let the charger accept or reject it. The STUSB4500
 * only forms a PDO2 contract if the source can supply that voltage AND current;
 * otherwise it falls back to PDO1 (5 V). We detect acceptance from the RDO
 * Object_Pos (the contracted SOURCE PDO index): == 1 is the 5 V fallback,
 * >= 2 means our profile matched. The first profile the charger accepts is
 * therefore the highest power it can deliver:
 *   20 V/5 A charger -> 100 W, 20 V/3 A -> 60 W, 15 V/3 A -> 45 W, 9 V/3 A -> 27 W.
 * The rungs are ordered by wattage (highest first) and include the 4.5 A and
 * 2.25 A ratings so a 90 W (20 V/4.5 A) or 45 W (20 V/2.25 A) charger is matched.
 * A 5 V-only source - or one that only offers high voltage below 3 A - accepts
 * none of these and we hold the safe 5 V baseline. A typical charger is matched
 * in the first 1-4 probes; the whole ladder is at most
 * 7 probes x PD_RENEGOTIATE_SETTLE_MS before the iron is ready. */
typedef struct { uint16_t mv; uint16_t ma; } pd_profile_t;
static const pd_profile_t pd_probe_ladder[] = {
	{20000, 5000},   /* 100 W */
	{20000, 4500},   /*  90 W */
	{20000, 3000},   /*  60 W */
	{20000, 2250},   /*  45 W  (20 V chargers without a 3 A rung) */
	{15000, 3000},   /*  45 W  (15 V-max chargers) */
	{12000, 3000},   /*  36 W */
	{ 9000, 3000},   /*  27 W */
};
#define PD_PROBE_COUNT (sizeof(pd_probe_ladder)/sizeof(pd_probe_ladder[0]))

/* Time to let a renegotiation settle before reading the RDO to learn whether
 * the requested PDO2 was accepted. */
#define PD_RENEGOTIATE_SETTLE_MS 300

/* --- Public state --- */
power_source_t power_source = POWER_DC;
pd_status_t    pd_status    = PD_STATUS_NO_CHIP;
uint8_t        pd_chip_id;

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

	/* FAST PATH. The STUSB4500 negotiates its NVM-programmed PDO set by itself
	 * as soon as the cable is attached, i.e. long before this code runs. Keep
	 * that contract only when it is already a strong one (>= 15 V and >= 2 A,
	 * i.e. at least 30 W): otherwise the chip's default PDO would throttle the
	 * iron to a fraction of what the charger can deliver, and the probe ladder
	 * below asks for something better. */
	(void)wait_for_sink_ready(1500);
	HAL_Delay(100);                       /* let VBUS finish settling */

	if((stusb_read_rdo(&rdo) == HAL_OK) && (rdo.b.Object_Pos != 0)){
		float v     = sensors_read_bus_voltage_instant();
		float i     = rdo.b.OperatingCurrent * 0.01f;   /* what the chip asked for  */
		float i_max = rdo.b.MaxCurrent * 0.01f;         /* what the source offers   */

		debug_print_int(DEBUG_INFO,"Auto contract mV", (int)(v * 1000.0f));
		debug_print_int(DEBUG_INFO,"Auto contract mA", (int)(i * 1000.0f));
		debug_print_int(DEBUG_INFO,"Source PDO max mA", (int)(i_max * 1000.0f));

		if((v >= 15.0f) && (i >= 2.0f)){
			apply_contract(&rdo);
			return;
		}

		/* The chip's NVM PDO may ask for far less current than the source
		 * offers (e.g. 20 V/1 A while the charger can do 20 V/3 A). The RDO
		 * carries the source's own figure in MaxCurrent, so request the same
		 * voltage at that current - one renegotiation instead of the ladder. */
		if((v >= 15.0f) && (i_max > (i + 0.1f))){
			uint16_t mv = (uint16_t)(((uint16_t)(v * 1000.0f) / 50u) * 50u);   /* 50 mV steps */

			stusb_update_pdo(2, mv, (uint16_t)(i_max * 1000.0f));
			stusb_set_valid_pdo(2);
			stusb_soft_reset();
			HAL_Delay(PD_RENEGOTIATE_SETTLE_MS);

			if((stusb_read_rdo(&rdo) == HAL_OK) && (rdo.b.Object_Pos != 0) &&
			   ((rdo.b.OperatingCurrent * 0.01f) >= 2.0f)){
				debug_print_str(DEBUG_INFO,"Source PDO accepted at its max current");
				wait_for_vbus_settle();
				apply_contract(&rdo);
				return;
			}
		}
	}

	/* SLOW PATH. The automatic contract is weak (or unknown), so ask for more.
	 * PDO1 (5 V) is left untouched, so every rejected request falls back to a
	 * safe 5 V contract - no brown-out - which also makes the explicit 5 V
	 * baseline unnecessary here. Probe the ladder, highest power first, and stop
	 * at the first profile the charger accepts. */
	bool accepted = false;
	for(uint8_t i = 0; i < PD_PROBE_COUNT; i++){
		stusb_update_pdo(2, pd_probe_ladder[i].mv, pd_probe_ladder[i].ma);
		stusb_set_valid_pdo(2);
		stusb_soft_reset();
		HAL_Delay(PD_RENEGOTIATE_SETTLE_MS);   /* let the renegotiation complete */

		stusb_read_rdo(&rdo);
		debug_print_int(DEBUG_INFO,"Probe mV", pd_probe_ladder[i].mv);
		debug_print_int(DEBUG_INFO," -> Object_Pos", rdo.b.Object_Pos);
		/* RDO Object_Pos is the SOURCE capability index that got contracted
		 * (1-based), NOT our sink slot. Source PDO 1 is always 5 V, so
		 * Object_Pos == 1 means PDO2 was rejected and the link fell back to 5 V.
		 * Any Object_Pos >= 2 means PDO2 matched a source PDO at the requested
		 * voltage => the charger supports this profile => accept it. */
		if(rdo.b.Object_Pos >= 2){
			accepted = true;
			break;
		}
	}

	/* If nothing was accepted, return to a clean 5 V baseline contract. */
	if(!accepted){
		debug_print_str(DEBUG_WARNING,"No high-power PDO accepted - holding 5V");
		request_5v_baseline();
		HAL_Delay(PD_RENEGOTIATE_SETTLE_MS);
		stusb_read_rdo(&rdo);
	}

	/* Confirm an explicit PD contract exists. */
	if(rdo.b.Object_Pos == 0){
		pd_status = PD_STATUS_NO_CONTRACT;
		debug_print_str(DEBUG_INFO,"No USB-PD contract");
		return;   /* leave power_source = POWER_DC */
	}

	/* Let VBUS settle at the negotiated voltage before measuring it. */
	wait_for_vbus_settle();
	apply_contract(&rdo);
}
