/* Host-side check of the STUSB4500 register decoding: PD message header bit
 * positions, fixed-PDO units, RDO fields and the sink-PDO template mask.
 *
 * No hardware and no ARM toolchain needed - these are plain arithmetic
 * macros, so a host compiler validates them against known-good PD words.
 */
#include <stdio.h>
#include <stdint.h>

#include "stusb4500_regs.h"

static int failures;

static void expect(const char *what, long got, long want)
{
    if (got != want) {
        printf("FAIL %-26s got %ld want %ld\n", what, got, want);
        failures++;
    } else {
        printf("ok   %-26s %ld\n", what, got);
    }
}

int main(void)
{
    /* 20 V / 3.25 A fixed PDO, the shape a 65 W charger advertises. */
    uint32_t pdo = ((uint32_t)(20000u / 50u) << 10) | (3250u / 10u);
    /* Source_Capabilities header: type 1, spec rev 2.0, source, 4 objects. */
    uint16_t src_caps = (uint16_t)(0x01u | (1u << 6) | (0x01u << 8) | (4u << 12));
    /* Accept: control message, message id 3. */
    uint16_t accept = (uint16_t)(STUSB_MSG_ACCEPT | (3u << 9));
    /* RDO: object position 2, 3 A operating, 3.25 A max. */
    uint32_t rdo = 325u | (300u << 10) | (2u << 28);
    /* PDO1 template with flags set in bits 29..20. */
    uint32_t sink = (0x2C04B12Cu & STUSB_PDO_TEMPLATE_MASK) | STUSB_PDO_WORD(15000u, 3000u);

    expect("pdo type", STUSB_PDO_TYPE(pdo), STUSB_PDO_FIXED);
    expect("pdo mV", STUSB_PDO_MV(pdo), 20000);
    expect("pdo mA", STUSB_PDO_MA(pdo), 3250);

    expect("caps header type", STUSB_HDR_MSG_TYPE(src_caps), STUSB_MSG_SOURCE_CAPABILITIES);
    expect("caps header objects", STUSB_HDR_OBJECTS(src_caps), 4);
    expect("caps header id", STUSB_HDR_MSG_ID(src_caps), 0);

    expect("accept type", STUSB_HDR_MSG_TYPE(accept), STUSB_MSG_ACCEPT);
    expect("accept objects", STUSB_HDR_OBJECTS(accept), 0);
    expect("accept id", STUSB_HDR_MSG_ID(accept), 3);

    expect("rdo object position", STUSB_RDO_OBJECT_POS(rdo), 2);
    expect("rdo operating mA", STUSB_RDO_OPER_CURRENT(rdo), 3000);
    expect("rdo max mA", STUSB_RDO_MAX_CURRENT(rdo), 3250);

    expect("sink pdo type", STUSB_PDO_TYPE(sink), STUSB_PDO_FIXED);
    expect("sink pdo mV", STUSB_PDO_MV(sink), 15000);
    expect("sink pdo mA", STUSB_PDO_MA(sink), 3000);
    expect("sink pdo flags kept", (long)(sink & 0x3C000000u), 0x2C000000);

    printf("%s\n", failures ? "FAILED" : "all pd register checks passed");
    return failures ? 1 : 0;
}
