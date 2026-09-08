#include <stdio.h>
#include <stdint.h>
#include <math.h>

#define N_TAPS   73
#define N_SAMPLES 100

static const double h[N_TAPS] = {
    0.025804122, 0.020552684, 0.028396631, 0.037923047, 0.049296639, 0.062671826, 0.078188509,
    0.095967794, 0.116107786, 0.138679546, 0.163723333, 0.191245225, 0.221214225, 0.253559961,
    0.288171034, 0.324894126, 0.363533898, 0.403853725, 0.445577304, 0.488391123, 0.53194777,
    0.575870065, 0.61975593,  0.663183933, 0.705719403, 0.746921012, 0.786347677, 0.82356567,
    0.858155769, 0.889720317, 0.917890027, 0.942330394, 0.962747575, 0.978893597, 0.990570797,
    0.997635377, 1.0,         0.997635377, 0.990570797, 0.978893597, 0.962747575, 0.942330394,
    0.917890027, 0.889720317, 0.858155769, 0.82356567,  0.786347677, 0.746921012, 0.705719403,
    0.663183933, 0.61975593,  0.575870065, 0.53194777,  0.488391123, 0.445577304, 0.403853725,
    0.363533898, 0.324894126, 0.288171034, 0.253559961, 0.221214225, 0.191245225, 0.163723333,
    0.138679546, 0.116107786, 0.095967794, 0.078188509, 0.062671826, 0.049296639, 0.037923047,
    0.028396631, 0.020552684, 0.025804122
};


// Coefficients: Q1.15 (16 bits, 15 fraction bits)
int16_t float_to_q1_15(double val) {
    int32_t r = (int32_t)lround(val * 32768.0);
    if (r > 32767)  r = 32767;
    if (r < -32768) r = -32768;
    return (int16_t)r;
}

// Input: Q2.10 (12 bits, 10 fraction bits)
int16_t float_to_q2_10(double val) {
    int32_t r = (int32_t)lround(val * 1024.0);
    if (r > 2047)  r = 2047;
    if (r < -2048) r = -2048;
    return (int16_t)r;
}
double q2_10_to_float(int16_t v) { return (double)v / 1024.0; }


// Output: Q3.13 (16 bits, 13 fraction bits)
int16_t float_to_q3_13(double val) {
    int32_t r = (int32_t)lround(val * 8192.0);
    if (r > 32767)  r = 32767;
    if (r < -32768) r = -32768;
    return (int16_t)r;
}
double q3_13_to_float(int16_t v) { return (double)v / 8192.0; }


int main(void) {
    //  1: quantize the coefficients once
    int16_t h_q15[N_TAPS];
    for (int k = 0; k < N_TAPS; k++)
        h_q15[k] = float_to_q1_15(h[k]);
 

 //  2: generate 100 input samples 
    // A mix of a slow and a fast sine wave, scaled to stay inside Q2.10's
    // range, so the low-pass filtering effect is visible in the output.

    double x_float[N_SAMPLES];
    int16_t x_q10[N_SAMPLES];
    for (int n = 0; n < N_SAMPLES; n++) {
        double slow = 0.6 * sin(2.0 * M_PI * n / 50.0);   // low frequency: should pass through
        double fast = 0.3 * sin(2.0 * M_PI * n / 4.0);    // high frequency: should be attenuated
        x_float[n] = slow + fast;
        x_q10[n]   = float_to_q2_10(x_float[n]);
    }


    // 3: run the convolution in FIXED-POINT integer arithmetic 
    // This must match the RTL's arithmetic exactly: Q1.15 x Q2.10 -> Q(.,25)
    // product, accumulated, then rounded and saturated down to Q3.13.
    int16_t y_q13[N_SAMPLES];
    double  y_float[N_SAMPLES];
 
    FILE *fin  = fopen("input_samples.mem", "w");
    FILE *fout = fopen("expected_output.mem", "w");
    FILE *frep = fopen("golden_model_report.csv", "w");
    fprintf(frep, "n,x_float,x_q10_hex,y_float,y_q13_hex\n");


for (int n = 0; n < N_SAMPLES; n++) {
        int64_t acc = 0; // wide enough to safely hold the full sum
        for (int k = 0; k < N_TAPS; k++) {
            int sample_idx = n - k;
            int16_t x_val = (sample_idx >= 0) ? x_q10[sample_idx] : 0; // zero-fill before startup
            acc += (int64_t)h_q15[k] * (int64_t)x_val; // Q(.,25) product, same as the DSP48's multiply
        }

        
        // Round and saturate down from Q(.,25) to Q3.13 -- identical logic to fir_filter_top.sv
        const int SHIFT = 25 - 13; // = 12
        int64_t rounded = acc + ((int64_t)1 << (SHIFT - 1));
        int64_t shifted = rounded >> SHIFT;
        if (shifted > 32767)  shifted = 32767;
        if (shifted < -32768) shifted = -32768;
        y_q13[n]   = (int16_t)shifted;
        y_float[n] = q3_13_to_float(y_q13[n]);
 
        fprintf(fin,  "%03x\n", (uint16_t)x_q10[n] & 0xFFF);
        fprintf(fout, "%04x\n", (uint16_t)y_q13[n]);
        fprintf(frep, "%d,%.6f,%03x,%.6f,%04x\n",
                n, x_float[n], (uint16_t)x_q10[n] & 0xFFF, y_float[n], (uint16_t)y_q13[n]);
    }
 
    fclose(fin); fclose(fout); fclose(frep);
 
    // print first and last few for a quick sanity check
    printf("n    x_float     y_float\n");
    for (int n = 0; n < 5; n++)
        printf("%3d  %+.6f   %+.6f\n", n, x_float[n], y_float[n]);
    printf("...\n");
    for (int n = N_SAMPLES-5; n < N_SAMPLES; n++)
        printf("%3d  %+.6f   %+.6f\n", n, x_float[n], y_float[n]);
 
    printf("\nWrote input_samples.mem, expected_output.mem, golden_model_report.csv\n");
    return 0;
}
 
