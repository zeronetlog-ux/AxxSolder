#ifndef __STUSB4500_H
#define __STUSB4500_H

#include "stm32g4xx_hal.h"
#include "stdbool.h"

#define STUSB4500_ADDR         0x28

#define VAL_STUSB4500_ID   0x25   /* DEVICE_ID of the production part      */
#define VAL_STUSB4500_ID_B 0x21   /* DEVICE_ID of the evaluation part      */

/* Raw value of REG_DEVICE_ID read by stusb_check_connection(); 0 = read failed.
 * Used by the UI to explain why PD was not detected. */
extern uint8_t stusb_chip_id;

typedef union {
  uint32_t d32;
  struct {
    uint32_t MaxCurrent                     :       10; //Bits 9..0
    uint32_t OperatingCurrent               :       10;
    uint8_t reserved_22_20                  :       3;
    uint8_t UnchunkedMess_sup               :       1;
    uint8_t UsbSuspend                      :       1;
    uint8_t UsbComCap                       :       1;
    uint8_t CapaMismatch                    :       1;
    uint8_t GiveBack                        :       1;
    uint8_t Object_Pos                      :       3; //Bits 30..28 (3-bit)
    uint8_t reserved_31		                :       1; //Bits 31
  } b;
} STUSB_GEN1S_RDO_REG_STATUS_RegTypeDef;

/* --- Source capabilities and PD messages ---
 *
 * The STUSB4500 does expose the source's Source_Capabilities: the last received
 * PD message sits in its RX buffer (RX_BYTE_CNT / RX_HEADER / RX_DATA_OBJ) and
 * is announced by ALERT_STATUS_1.PRT_STATUS_AL. A soft reset makes the source
 * send its capabilities again, so the list is read by
 * stusb_read_source_pdos() instead of probing current/voltage combinations.
 */
#define STUSB_RX_OBJECT_MAX 7          /* PD message: at most 7 data objects  */
#define STUSB_SRC_PDO_MAX   STUSB_RX_OBJECT_MAX

/* One fixed-supply PDO of the source. */
typedef struct {
	uint16_t mv;                       /* voltage, mV                         */
	uint16_t ma;                       /* current rating, mA (10 mA units)    */
} stusb_pdo_t;

/* One captured PD message. `objects[]` holds the data objects when is_data. */
typedef struct {
	uint8_t  message_type;
	uint8_t  message_id;
	uint8_t  object_count;             /* 0 = control message                 */
	bool     is_data;
	uint32_t objects[STUSB_RX_OBJECT_MAX];
} stusb_msg_t;

typedef enum {
	STUSB_CONTRACT_OK = 0,
	STUSB_CONTRACT_REJECTED,           /* source said Reject, or fell back    */
	STUSB_CONTRACT_TIMEOUT             /* no answer within the window         */
} stusb_contract_t;

/* Cable detection: raw register values plus the two decoded flags. */
typedef struct {
	bool    attached;                  /* a plug is detected on either CC     */
	bool    cc2;                       /* the flipped orientation is in use   */
	uint8_t port_status;               /* REG_PORT_STATUS (0x0E)              */
	uint8_t type_c_status;             /* REG_TYPE_C_STATUS (0x15)            */
} stusb_cable_t;

HAL_StatusTypeDef stusb_init();
HAL_StatusTypeDef stusb_soft_reset();
HAL_StatusTypeDef stusb_check_connection();
bool stusb_is_sink_ready();
bool stusb_is_sink_connected();
HAL_StatusTypeDef stusb_read_rdo(STUSB_GEN1S_RDO_REG_STATUS_RegTypeDef *Nego_RDO);
HAL_StatusTypeDef stusb_set_valid_pdo(uint8_t valid_count);

/* Clear the pending alerts and unmask the ones the sink reacts to. Must run
 * after the chip answers on I2C: the chip is powered from VBUS and keeps its
 * state across an MCU reset, so stale alerts would otherwise hide the first
 * PD message of this session. */
void stusb_alerts_clear_unmask(void);

/* Read one PD message out of the RX buffer. Returns false when no (complete,
 * consistent) message is available: no PRT alert, no MSG_RECEIVED, or a byte
 * count that does not match the object count (buffer overwritten mid-read).
 * Reading the status registers clears them, so a missed message is not seen
 * twice - the caller retries instead. */
bool stusb_poll_message(stusb_msg_t *msg);

/* Read the source's fixed-supply PDOs. Triggers `attempts` soft resets, and
 * within each one polls the RX buffer for up to `per_try_ms` for the
 * Source_Capabilities message. Returns the number of entries written to `out`
 * (0 = the capabilities could not be read). */
uint8_t stusb_read_source_pdos(stusb_pdo_t *out, uint8_t max,
                               uint16_t per_try_ms, uint8_t attempts);

/* Ask the source for one (mV, mA) contract: PDO2 is programmed from the PDO1
 * template, the PDO count is raised to two and a soft reset starts the
 * renegotiation. Returns what the source answered (Accept + PS_RDY and a
 * matching RDO means OK). */
stusb_contract_t stusb_request_contract(uint16_t mv, uint16_t ma);

/* Read the Type-C attach state and orientation. */
void stusb_read_cable(stusb_cable_t *cable);

/* Read the controller's own NVM settings into `out`. The chip negotiates its
 * first contract from these settings before the MCU boots, so they explain what
 * a source sees at plug-in. Non-destructive: the NVM is only read, and the test
 * mode is left again afterwards.
 *
 * Five sectors of 8 bytes; sector 3 holds the sink PDO count and the PDO
 * currents, sector 4 the PDO2/PDO3 voltages. */
#define STUSB_NVM_SECTORS     5u
#define STUSB_NVM_SECTOR_SIZE 8u
HAL_StatusTypeDef stusb_nvm_read(uint8_t *out);

#endif

