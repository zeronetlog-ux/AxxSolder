#include "stusb4500.h"
#include "stusb4500_regs.h"
#include "debug.h"

#ifdef STUSB_DEBUG
#include <stdio.h>
#include <string.h>
extern UART_HandleTypeDef huart1;
#endif

extern I2C_HandleTypeDef hi2c1;

/* Value of REG_DEVICE_ID from the last stusb_check_connection() (0 = read failed). */
uint8_t stusb_chip_id;

/* Every transfer is a handful of bytes, so the timeout only matters when the
 * bus is stuck - keep it short enough for the polling loops below. */
#define STUSB_I2C_TIMEOUT_MS 50u

/* Window for Accept/Reject/PS_RDY after a renegotiation was started. */
#define STUSB_CONTRACT_TIMEOUT_MS 600u

/* The RDO register follows PS_RDY by a few milliseconds; give it this long to
 * report the new object position before calling the request rejected. */
#define STUSB_CONTRACT_SETTLE_MS  500u

static HAL_StatusTypeDef stusb_read_register(uint8_t reg, uint8_t *data, uint8_t len)
{
  return HAL_I2C_Mem_Read(&hi2c1, (STUSB4500_ADDR << 1), reg, I2C_MEMADD_SIZE_8BIT, data, len, STUSB_I2C_TIMEOUT_MS);
}

static HAL_StatusTypeDef stusb_write_register(uint8_t reg, const uint8_t *data, uint8_t len)
{
  return HAL_I2C_Mem_Write(&hi2c1, (STUSB4500_ADDR << 1), reg, I2C_MEMADD_SIZE_8BIT,
                           (uint8_t *)(uintptr_t)data, len, STUSB_I2C_TIMEOUT_MS);
}

static uint32_t le32(const uint8_t *p)
{
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* ---------------------------------------------------------------- logging --
 * Compiled in with -DSTUSB_DEBUG=ON (tools/build.sh --debug). Release builds
 * expand every call to nothing, so the diagnostics cost no flash there.
 */
#ifdef STUSB_DEBUG

static void stusb_log(const char *text)
{
  HAL_UART_Transmit(&huart1, (uint8_t *)(uintptr_t)text, (uint16_t)strlen(text), 100);
}

static void stusb_log_reg(const char *name, uint8_t value)
{
  char line[32];

  (void)snprintf(line, sizeof(line), "%s=%02X\n", name, (unsigned)value);
  stusb_log(line);
}

static void stusb_log_pdo(uint8_t index, uint32_t pdo)
{
  char line[44];

  (void)snprintf(line, sizeof(line), "  pdo%u %u.%02uV %u.%02uA type%u\n",
                 (unsigned)index,
                 (unsigned)(STUSB_PDO_MV(pdo) / 1000u), (unsigned)((STUSB_PDO_MV(pdo) % 1000u) / 10u),
                 (unsigned)(STUSB_PDO_MA(pdo) / 1000u), (unsigned)((STUSB_PDO_MA(pdo) % 1000u) / 10u),
                 (unsigned)STUSB_PDO_TYPE(pdo));
  stusb_log(line);
}

static void stusb_log_msg(const stusb_msg_t *msg)
{
  char line[40];

  (void)snprintf(line, sizeof(line), "msg type=%u objects=%u id=%u\n",
                 (unsigned)msg->message_type, (unsigned)msg->object_count, (unsigned)msg->message_id);
  stusb_log(line);
}

#else

#define stusb_log(text)          ((void)0)
#define stusb_log_reg(name, val) ((void)0)
#define stusb_log_pdo(idx, pdo)  ((void)0)
#define stusb_log_msg(msg)       ((void)0)

#endif /* STUSB_DEBUG */

HAL_StatusTypeDef stusb_check_connection(){
	HAL_StatusTypeDef ret;
	uint8_t chipID = 0;

	ret = stusb_read_register(STUSB_REG_DEVICE_ID, &chipID, 1);
	stusb_chip_id = chipID;

	if (ret != HAL_OK) {
		return HAL_ERROR;
	}
	/* 0x25 = STUSB4500, 0x21 = STUSB4500B: both are valid parts, the B
	 * revision would otherwise be treated as "no PD controller at all". */
	if ((chipID == VAL_STUSB4500_ID) || (chipID == VAL_STUSB4500_ID_B)) {
		return HAL_OK;
	}
	return HAL_ERROR;
}

/* Clear the pending alerts and unmask the ones this firmware reacts to.
 *
 * The 12-byte burst covers ALERT_STATUS_1 (0x0B) through PRT_STATUS (0x16);
 * those are read-to-clear, so the previous session's flags (the chip is powered
 * from VBUS and survives an MCU reset) and a MSG_RECEIVED left over from an
 * earlier exchange are gone. Without this the first PD message of the session
 * can be consumed as a stale one. */
void stusb_alerts_clear_unmask(void)
{
	uint8_t alerts[12];
	uint8_t mask = (uint8_t)~STUSB_ALERT_UNMASKED;

	(void)stusb_read_register(STUSB_REG_ALERT_STATUS_1, alerts, sizeof(alerts));
	(void)stusb_write_register(STUSB_REG_ALERT_STATUS_1_MASK, &mask, 1);
}

HAL_StatusTypeDef stusb_init(){
	stusb_alerts_clear_unmask();
	return HAL_OK;
}

/* Read one PD message out of the RX buffer.
 *
 * Order and checks follow the reference driver:
 *   ALERT_STATUS_1 + mask -> PRT_STATUS.MSG_RECEIVED -> RX_HEADER ->
 *   RX_BYTE_CNT (must equal objects x 4) -> RX_DATA_OBJ.
 * The byte count check rejects a buffer that was overwritten between the
 * header and the payload (the sink's own Request, Accept, PS_RDY...). */
bool stusb_poll_message(stusb_msg_t *msg)
{
	uint8_t  alerts[2];
	uint8_t  status;
	uint8_t  prt = 0;
	uint8_t  byteCount = 0;
	uint8_t  headerBytes[2];
	uint16_t header;
	uint8_t  objects;
	uint8_t  data[STUSB_RX_OBJECT_MAX * 4u];

	if (msg == NULL) {
		return false;
	}

	/* Status and mask are adjacent: one read gets both and clears the alerts. */
	if (stusb_read_register(STUSB_REG_ALERT_STATUS_1, alerts, 2) != HAL_OK) {
		return false;
	}
	status = (uint8_t)(alerts[0] & (uint8_t)~alerts[1]);
	if ((status & STUSB_ALERT_PRT_STATUS) == 0u) {
		return false;
	}

	if (stusb_read_register(STUSB_REG_PRT_STATUS, &prt, 1) != HAL_OK) {
		return false;
	}
	if ((prt & STUSB_PRT_MSG_RECEIVED) == 0u) {
		return false;
	}

	if (stusb_read_register(STUSB_REG_RX_HEADER, headerBytes, 2) != HAL_OK) {
		return false;
	}
	header  = (uint16_t)(headerBytes[0] | ((uint16_t)headerBytes[1] << 8));
	objects = STUSB_HDR_OBJECTS(header);

	if (objects > STUSB_RX_OBJECT_MAX) {
		return false;
	}

	if (stusb_read_register(STUSB_REG_RX_BYTE_CNT, &byteCount, 1) != HAL_OK) {
		return false;
	}
	if (byteCount != (uint8_t)(objects * 4u)) {
		stusb_log_reg("rx byte count", byteCount);
		return false;
	}

	if (objects > 0u) {
		if (stusb_read_register(STUSB_REG_RX_DATA_OBJ, data, (uint8_t)(objects * 4u)) != HAL_OK) {
			return false;
		}
		for (uint8_t i = 0; i < objects; i++) {
			msg->objects[i] = le32(&data[i * 4u]);
		}
	}

	msg->message_type = STUSB_HDR_MSG_TYPE(header);
	msg->message_id   = STUSB_HDR_MSG_ID(header);
	msg->object_count = objects;
	msg->is_data      = (objects > 0u);

	stusb_log_msg(msg);
	return true;
}

/* Copy the fixed-supply PDOs of a Source_Capabilities message into `out`. */
static uint8_t decode_source_pdos(const stusb_msg_t *msg, stusb_pdo_t *out, uint8_t max)
{
	uint8_t count = 0;

	for (uint8_t i = 0; (i < msg->object_count) && (count < max); i++) {
		uint32_t pdo = msg->objects[i];

		/* PPS/APDO, variable and battery supplies are not used by the
		 * station: only fixed voltages make sense for the heater. */
		if (STUSB_PDO_TYPE(pdo) != STUSB_PDO_FIXED) {
			continue;
		}

		out[count].mv = (i == 0u) ? 5000u : STUSB_PDO_MV(pdo);
		out[count].ma = STUSB_PDO_MA(pdo);
		stusb_log_pdo(count, pdo);
		count++;
	}

	return count;
}

uint8_t stusb_read_source_pdos(stusb_pdo_t *out, uint8_t max,
                               uint16_t per_try_ms, uint8_t attempts)
{
	stusb_msg_t msg;

	if ((out == NULL) || (max == 0u) || (attempts == 0u)) {
		return 0;
	}

	for (uint8_t attempt = 0; attempt < attempts; attempt++) {
		uint32_t deadline;

		/* A soft reset makes the source send its Source_Capabilities again. */
		if (stusb_soft_reset() != HAL_OK) {
			stusb_log("soft reset failed\n");
			return 0;
		}

		deadline = HAL_GetTick() + per_try_ms;
		while (HAL_GetTick() < deadline) {
			if (!stusb_poll_message(&msg)) {
				continue;
			}
			if (msg.is_data && (msg.message_type == STUSB_MSG_SOURCE_CAPABILITIES)) {
				return decode_source_pdos(&msg, out, max);
			}
		}
	}

	stusb_log("no source capabilities\n");
	return 0;
}

static HAL_StatusTypeDef stusb_write_snk_pdo(uint8_t number, uint32_t pdo)
{
	uint8_t raw[4];

	raw[0] = (uint8_t)(pdo & 0xFFu);
	raw[1] = (uint8_t)((pdo >> 8) & 0xFFu);
	raw[2] = (uint8_t)((pdo >> 16) & 0xFFu);
	raw[3] = (uint8_t)((pdo >> 24) & 0xFFu);

	return stusb_write_register((uint8_t)(STUSB_REG_DPM_SNK_PDO1 + (4u * (number - 1u))), raw, 4);
}

stusb_contract_t stusb_request_contract(uint16_t mv, uint16_t ma, stusb_dpm_trace_t *trace)
{
	STUSB_GEN1S_RDO_REG_STATUS_RegTypeDef rdo = {0};
	stusb_msg_t msg;
	uint8_t     pdo1[4];
	uint8_t     raw[4];
	uint32_t    pdo;
	uint8_t     number;
	uint8_t     pdo_addr;
	bool        accepted = false;
	uint32_t    deadline;

	if (trace != NULL) {
		trace->count_written  = 0;
		trace->pdo_written    = 0;
		trace->count_readback = 0;
		trace->pdo_readback   = 0;
		trace->count_after    = 0;
		trace->pdo_after      = 0;
	}

	/* PDO1 carries the flags every sink PDO needs (USB communication capable,
	 * power/data roles...); only the voltage and current fields are replaced. */
	if (stusb_read_register(STUSB_REG_DPM_SNK_PDO1, pdo1, 4) != HAL_OK) {
		stusb_log("cannot read PDO1\n");
		return STUSB_CONTRACT_TIMEOUT;
	}

	/* 5 V is PDO1: the PD spec fixes its voltage, so only the current is
	 * programmed and the sink keeps a single PDO. Asking for 5 V through PDO2
	 * would advertise two identical PDOs. */
	number   = (mv <= 5000u) ? 1u : 2u;
	pdo_addr = (uint8_t)(STUSB_REG_DPM_SNK_PDO1 + (4u * (number - 1u)));
	pdo      = (le32(pdo1) & STUSB_PDO_TEMPLATE_MASK) |
	           ((number == 1u) ? STUSB_PDO_WORD(5000u, ma) : STUSB_PDO_WORD(mv, ma));

	/* Count first, then the PDO - the order the reference driver uses. */
	if ((stusb_set_valid_pdo(number) != HAL_OK) ||
	    (stusb_write_snk_pdo(number, pdo) != HAL_OK)) {
		stusb_log("request: i2c failed\n");
		return STUSB_CONTRACT_TIMEOUT;
	}

	if (trace != NULL) {
		trace->count_written = number;
		trace->pdo_written   = pdo;
		(void)stusb_read_register(STUSB_REG_DPM_PDO_NUMB, &trace->count_readback, 1);
		if (stusb_read_register(pdo_addr, raw, 4) == HAL_OK) {
			trace->pdo_readback = le32(raw);
		}
	}

	if (stusb_soft_reset() != HAL_OK) {
		stusb_log("request: i2c failed\n");
		return STUSB_CONTRACT_TIMEOUT;
	}

	/* Accept, then PS_RDY once the source has moved to the new voltage. The
	 * Source_Capabilities and other data messages in this window are consumed
	 * and ignored. */
	deadline = HAL_GetTick() + STUSB_CONTRACT_TIMEOUT_MS;
	while (HAL_GetTick() < deadline) {
		if (!stusb_poll_message(&msg) || msg.is_data) {
			continue;
		}
		if (msg.message_type == STUSB_MSG_REJECT) {
			stusb_log("contract rejected\n");
			return STUSB_CONTRACT_REJECTED;
		}
		if (msg.message_type == STUSB_MSG_ACCEPT) {
			accepted = true;
		}
		if (accepted && (msg.message_type == STUSB_MSG_PS_RDY)) {
			break;
		}
	}

	if (!accepted) {
		stusb_log("contract timeout\n");
		return STUSB_CONTRACT_TIMEOUT;
	}

	/* What does the controller report after the exchange? */
	if (trace != NULL) {
		(void)stusb_read_register(STUSB_REG_DPM_PDO_NUMB, &trace->count_after, 1);
		if (stusb_read_register(pdo_addr, raw, 4) == HAL_OK) {
			trace->pdo_after = le32(raw);
		}
	}

	/* The RDO tells which source PDO the link settled on (0 = none). Its object
	 * position is the SOURCE PDO number and has nothing to do with our sink PDO
	 * index - comparing the two used to reject every contract that landed on a
	 * source PDO other than 1 or 2, which is most of them. The granted operating
	 * current is taken from the RDO by the caller. */
	deadline = HAL_GetTick() + STUSB_CONTRACT_SETTLE_MS;
	do {
		if (stusb_read_rdo(&rdo) != HAL_OK) {
			return STUSB_CONTRACT_TIMEOUT;
		}
		if (rdo.b.Object_Pos != 0u) {
			stusb_log("contract ok\n");
			return STUSB_CONTRACT_OK;
		}
		HAL_Delay(10);
	} while (HAL_GetTick() < deadline);

	stusb_log("no contract\n");
	return STUSB_CONTRACT_TIMEOUT;
}

HAL_StatusTypeDef stusb_nvm_read(uint8_t *out)
{
	uint8_t value;
	uint8_t sector;
	uint32_t deadline;

	if (out == NULL) {
		return HAL_ERROR;
	}

	/* Unlock the NVM controller and enter read mode. */
	value = STUSB_FTP_PASSWORD_VALUE;
	if (stusb_write_register(STUSB_REG_FTP_CUST_PASSWORD, &value, 1) != HAL_OK) {
		return HAL_ERROR;
	}

	value = 0x00u;                    /* NVM internal controller reset */
	if (stusb_write_register(STUSB_REG_FTP_CTRL_0, &value, 1) != HAL_OK) {
		return HAL_ERROR;
	}

	value = (uint8_t)(STUSB_FTP_CUST_PWR | STUSB_FTP_CUST_RST_N);
	if (stusb_write_register(STUSB_REG_FTP_CTRL_0, &value, 1) != HAL_OK) {
		return HAL_ERROR;
	}

	for (sector = 0; sector < STUSB_NVM_SECTORS; sector++) {
		value = (uint8_t)(STUSB_FTP_CUST_PWR | STUSB_FTP_CUST_RST_N);
		if (stusb_write_register(STUSB_REG_FTP_CTRL_0, &value, 1) != HAL_OK) {
			return HAL_ERROR;
		}

		value = (uint8_t)(STUSB_FTP_OP_READ & STUSB_FTP_CUST_OPCODE);
		if (stusb_write_register(STUSB_REG_FTP_CTRL_1, &value, 1) != HAL_OK) {
			return HAL_ERROR;
		}

		value = (uint8_t)((sector & STUSB_FTP_CUST_SECT) |
		                  STUSB_FTP_CUST_PWR | STUSB_FTP_CUST_RST_N | STUSB_FTP_CUST_REQ);
		if (stusb_write_register(STUSB_REG_FTP_CTRL_0, &value, 1) != HAL_OK) {
			return HAL_ERROR;
		}

		/* FTP_CUST_REQ is cleared by the NVM controller when the operation is
		 * done. */
		deadline = HAL_GetTick() + 50u;
		do {
			if (stusb_read_register(STUSB_REG_FTP_CTRL_0, &value, 1) != HAL_OK) {
				return HAL_ERROR;
			}
			if ((value & STUSB_FTP_CUST_REQ) == 0u) {
				break;
			}
		} while (HAL_GetTick() < deadline);

		if ((value & STUSB_FTP_CUST_REQ) != 0u) {
			stusb_log("nvm read timeout\n");
			break;
		}

		if (stusb_read_register(STUSB_REG_RW_BUFFER,
		                        &out[sector * STUSB_NVM_SECTOR_SIZE],
		                        STUSB_NVM_SECTOR_SIZE) != HAL_OK) {
			return HAL_ERROR;
		}
	}

	/* Leave test mode again: release the reset and clear the password. */
	value = STUSB_FTP_CUST_RST_N;
	(void)stusb_write_register(STUSB_REG_FTP_CTRL_0, &value, 1);
	value = 0x00u;
	(void)stusb_write_register(STUSB_REG_FTP_CUST_PASSWORD, &value, 1);

	return (sector == STUSB_NVM_SECTORS) ? HAL_OK : HAL_ERROR;
}

void stusb_read_cable(stusb_cable_t *cable)
{
	uint8_t port = 0;
	uint8_t typec = 0;

	if (cable == NULL) {
		return;
	}

	cable->attached      = false;
	cable->cc2           = false;
	cable->port_status   = 0;
	cable->type_c_status = 0;

	if ((stusb_read_register(STUSB_REG_PORT_STATUS, &port, 1) != HAL_OK) ||
	    (stusb_read_register(STUSB_REG_TYPE_C_STATUS, &typec, 1) != HAL_OK)) {
		return;
	}

	cable->port_status   = port;
	cable->type_c_status = typec;
	/* CC_ATTACH_STATE (bit 0) and CC_ATTACH_MODE (bits 7..5) both report the
	 * plug; either one being set means a cable is attached. */
	cable->attached = ((port & (STUSB_PORT_ATTACHED | STUSB_PORT_ATTACH_MODE_MASK)) != 0u);
	cable->cc2      = ((typec & STUSB_TYPEC_REVERSE) != 0u);

	stusb_log_reg("port", port);
	stusb_log_reg("typec", typec);
}

bool stusb_is_sink_ready() {
  uint8_t data = 0;

  if (stusb_read_register(STUSB_REG_PE_FSM, &data, 1) != HAL_OK) {
	  return false;
  }
  stusb_log_reg("pe fsm", data);
  /* PE_SNK_READY / PE_SNK_READY_SENDING - the contract is in place. */
  return (data == STUSB_PE_SNK_READY) || (data == STUSB_PE_SNK_READY_SENDING);
}

bool stusb_is_sink_connected() {
  uint8_t data = 0;

  if (stusb_read_register(STUSB_REG_PORT_STATUS, &data, 1) != HAL_OK) {
	  return false;
  }
  /* Any non-zero CC_ATTACH_MODE means a plug is detected. */
  return ((data & STUSB_PORT_ATTACH_MODE_MASK) != 0u);
}

HAL_StatusTypeDef stusb_read_rdo(STUSB_GEN1S_RDO_REG_STATUS_RegTypeDef *Nego_RDO) {
  return stusb_read_register(STUSB_REG_RDO_REG_STATUS, (uint8_t *) Nego_RDO, 4);
}

HAL_StatusTypeDef stusb_set_valid_pdo(uint8_t valid_count) {
  if (valid_count > 3) {
    return HAL_ERROR;
  }
  return stusb_write_register(STUSB_REG_DPM_PDO_NUMB, &valid_count, 1);
}

/* Renegotiate with the source: a soft reset makes it repeat its capabilities
 * and lets the chip re-request the sink PDOs that are currently programmed. */
HAL_StatusTypeDef stusb_soft_reset(){
	uint8_t cmd[]={STUSB_CMD_SOFT_RESET,0x00};
	HAL_StatusTypeDef ret = stusb_write_register(STUSB_REG_TX_HEADER, cmd, 2);
	if(ret != HAL_OK){ return ret; }
	cmd[0] = STUSB_CMD_SEND;
	return stusb_write_register(STUSB_REG_PD_COMMAND_CTRL, cmd, 1);
}

