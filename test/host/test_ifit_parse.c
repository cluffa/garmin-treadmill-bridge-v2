#include <assert.h>
#include <math.h>
#include <stdio.h>
#include "ifit_parse.h"

static int approx(float a, float b) { return fabsf(a - b) < 0.001f; }

int main(void) {
    float spd, inc;

    /* Valid data frame: header 00 12 01 04, speed 8.00 km/h (raw 800=0x0320,
     * LE at [10,11]), incline +1.5% (raw 150=0x0096, LE at [12,13]). */
    uint8_t f[20] = {0};
    f[0]=0x00; f[1]=0x12; f[2]=0x01; f[3]=0x04;
    f[5]=0x2e;                /* T6.5S model discriminator */
    f[10]=0x20; f[11]=0x03;   /* 0x0320 = 800 */
    f[12]=0x96; f[13]=0x00;   /* 0x0096 = 150 */
    assert(ifit_parse_data(f, sizeof f, &spd, &inc));
    assert(approx(spd, 8.0f * 1000.0f / 3600.0f));   /* 2.2222 m/s */
    assert(approx(inc, 1.5f));

    /* Negative incline: -1.5% = raw -150 = 0xFF6A (LE: 6A FF). */
    f[12]=0x6A; f[13]=0xFF;
    assert(ifit_parse_data(f, sizeof f, &spd, &inc));
    assert(approx(inc, -1.5f));

    /* Rejections: wrong length, wrong header. */
    assert(!ifit_parse_data(f, 19, &spd, &inc));
    uint8_t bad[20] = {0};
    bad[0]=0x00; bad[1]=0x12; bad[2]=0x02; bad[3]=0x04;   /* [2]=0x02, not a data frame */
    assert(!ifit_parse_data(bad, sizeof bad, &spd, &inc));

    printf("ifit_parse: OK\n");
    return 0;
}
