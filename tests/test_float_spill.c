/* Test: many live float values force spilling.
 * Also checks that AST optimizer does not rebalance float + chains (IEEE semantics).
 */

float main() {
    float f1;  float f2;  float f3;  float f4;  float f5;
    float f6;  float f7;  float f8;  float f9;  float f10;
    float f11; float f12; float f13; float f14; float f15;
    float f16; float f17; float f18; float f19; float f20;
    float sum;

    f1 = 1.5;  f2 = 2.5;  f3 = 3.5;  f4 = 4.5;  f5 = 5.5;
    f6 = 6.5;  f7 = 7.5;  f8 = 8.5;  f9 = 9.5;  f10 = 10.5;
    f11 = 11.5; f12 = 12.5; f13 = 13.5; f14 = 14.5; f15 = 15.5;
    f16 = 16.5; f17 = 17.5; f18 = 18.5; f19 = 19.5; f20 = 20.5;

    sum = f1 + f2 + f3 + f4 + f5 + f6 + f7 + f8 + f9 + f10 +
          f11 + f12 + f13 + f14 + f15 + f16 + f17 + f18 + f19 + f20;

    return sum;
}
