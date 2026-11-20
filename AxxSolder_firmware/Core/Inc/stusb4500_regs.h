#ifndef STUSB4500_REGS_H
#define STUSB4500_REGS_H

/* STUSB4500 register map and bit definitions - only the subset the firmware
 * uses.
 *
 * The values mirror the register header of the reference driver
 * (STUSB400-arduino/src/dat/STUSB4500_register.h), which follows the STUSB4500
 * datasheet register map. Deliberately free of bitfields: the PD message
 * header and the PDO/RDO words are decoded with shifts and masks, so the code
 * does not depend on the compiler's bitfield layout.
 */

#include <stdint.h>

/* --- Register addresses --- */
#define STUSB_REG_ALERT_STATUS_1        0x0Bu  /* RC: pending alerts            */
#define STUSB_REG_ALERT_STATUS_1_MASK   0x0Cu  /* RW: 1 = alert masked          */
#define STUSB_REG_PORT_STATUS_TRANS     0x0Du  /* RC: attach transitions        */
#define STUSB_REG_PORT_STATUS           0x0Eu  /* RO: CC attach state / mode    */
#define STUSB_REG_TYPEC_MONITORING_1    0x10u  /* RO: VBUS ready and friends    */
#define STUSB_REG_CC_STATUS             0x11u  /* RO: CC line status            */
#define STUSB_REG_TYPE_C_STATUS         0x15u  /* RO: bit 7 = CC2 side active   */
#define STUSB_REG_PRT_STATUS            0x16u  /* RC: PD protocol status        */
#define STUSB_REG_PD_COMMAND_CTRL       0x1Au  /* RW: send a PD command         */
#define STUSB_REG_RESET_CTRL            0x23u  /* RW: software reset            */
#define STUSB_REG_PE_FSM                0x29u  /* RO: policy engine state       */
#define STUSB_REG_DEVICE_ID             0x2Fu
#define STUSB_REG_RX_BYTE_CNT           0x30u
#define STUSB_REG_RX_HEADER             0x31u  /* 16 bit                        */
#define STUSB_REG_RX_DATA_OBJ           0x33u  /* up to 7 x 32 bit, 4 apart     */
#define STUSB_REG_TX_HEADER             0x51u  /* 16 bit                        */
#define STUSB_REG_DPM_PDO_NUMB          0x70u
#define STUSB_REG_DPM_SNK_PDO1          0x85u  /* 3 x 32 bit, 4 bytes apart     */
#define STUSB_REG_RDO_REG_STATUS        0x91u  /* 32 bit                        */

/* --- ALERT_STATUS_1 / ALERT_STATUS_1_MASK bits --- */
#define STUSB_ALERT_PHY_STATUS          0x01u
#define STUSB_ALERT_PRT_STATUS          0x02u  /* PD message traffic            */
#define STUSB_ALERT_PD_TYPEC_STATUS     0x08u
#define STUSB_ALERT_HW_FAULT_STATUS     0x10u
#define STUSB_ALERT_MONITORING_STATUS   0x20u
#define STUSB_ALERT_CC_DETECTION_STATUS 0x40u
#define STUSB_ALERT_HARD_RESET          0x80u

/* Alerts kept unmasked by stusb_alerts_clear_unmask(): everything the sink is
 * expected to react to. PHY and HW_FAULT stay masked, as in the reference
 * driver - nothing here handles them. */
#define STUSB_ALERT_UNMASKED \
    (STUSB_ALERT_PRT_STATUS | STUSB_ALERT_PD_TYPEC_STATUS | \
     STUSB_ALERT_MONITORING_STATUS | STUSB_ALERT_CC_DETECTION_STATUS | \
     STUSB_ALERT_HARD_RESET)

/* --- PORT_STATUS (0x0E) --- */
#define STUSB_PORT_ATTACHED            0x01u   /* CC_ATTACH_STATE             */
#define STUSB_PORT_ATTACH_MODE_MASK     0xE0u  /* CC_ATTACH_MODE (bits 7..5)  */

/* --- TYPE_C_STATUS (0x15) --- */
#define STUSB_TYPEC_REVERSE             0x80u  /* 0 = CC1 active, 1 = CC2     */

/* --- PRT_STATUS (0x16) --- */
#define STUSB_PRT_HWRESET_RECEIVED      0x01u
#define STUSB_PRT_HWRESET_DONE          0x02u
#define STUSB_PRT_MSG_RECEIVED          0x04u  /* a PD message is in the RX buffer */
#define STUSB_PRT_MSG_SENT              0x08u
#define STUSB_PRT_BIST_RECEIVED         0x10u
#define STUSB_PRT_TX_ERROR              0x80u

/* --- PD commands: TX_HEADER low byte + PD_COMMAND_CTRL --- */
#define STUSB_CMD_SOFT_RESET            0x0Du
#define STUSB_CMD_SEND                  0x26u

/* --- PE_FSM (0x29) states --- */
#define STUSB_PE_SNK_READY              0x18u
#define STUSB_PE_SNK_READY_SENDING      0x19u

/* --- PD message header (16 bit) --- */
#define STUSB_HDR_MSG_TYPE(h)   ((uint8_t)((h) & 0x1Fu))
#define STUSB_HDR_MSG_ID(h)     ((uint8_t)(((h) >> 9) & 0x07u))
#define STUSB_HDR_OBJECTS(h)    ((uint8_t)(((h) >> 12) & 0x07u))

/* --- PD message types (5 bit for PD 3.0) --- */
#define STUSB_MSG_SOURCE_CAPABILITIES   0x01u
#define STUSB_MSG_REQUEST               0x02u
#define STUSB_MSG_ACCEPT                0x03u
#define STUSB_MSG_REJECT                0x04u
#define STUSB_MSG_PS_RDY                0x06u
#define STUSB_MSG_GET_SOURCE_CAP        0x07u

/* --- PDO word --- */
#define STUSB_PDO_TYPE(p)       ((uint8_t)(((p) >> 30) & 0x03u))
#define STUSB_PDO_FIXED         0u
#define STUSB_PDO_MV(p)         ((uint16_t)((((p) >> 10) & 0x3FFu) * 50u))
#define STUSB_PDO_MA(p)         ((uint16_t)(((p) & 0x3FFu) * 10u))
#define STUSB_PDO_WORD(mv, ma) \
    ((((uint32_t)((mv) / 50u)) << 10) | ((uint32_t)((ma) / 10u)))
/* Flags a sink PDO inherits from PDO1 (bits 29..20: dual role power/data, USB
 * comm, unconstrained power, higher capability...). The supply-type bits
 * 31..30 are cleared, i.e. the result is always a fixed-supply PDO. */
#define STUSB_PDO_TEMPLATE_MASK 0x3FF00000u

/* --- RDO word (REG_RDO_REG_STATUS) --- */
#define STUSB_RDO_MAX_CURRENT(r)  ((uint16_t)(((r) & 0x3FFu) * 10u))
#define STUSB_RDO_OPER_CURRENT(r) ((uint16_t)((((r) >> 10) & 0x3FFu) * 10u))
#define STUSB_RDO_OBJECT_POS(r)   ((uint8_t)(((r) >> 28) & 0x07u))

#endif /* STUSB4500_REGS_H */
