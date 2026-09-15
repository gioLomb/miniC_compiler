/* Test: array stores in a loop (SR candidate) followed by many simultaneous loads.
 * Forces spill of load results and checks that LICM does not move impure LOAD_ARR.
 */

int main() {
    int arr[20];
    int i;
    int sum;
    int b1; int b2; int b3; int b4; int b5; int b6; int b7; int b8;
    int b9; int b10; int b11; int b12; int b13; int b14; int b15;

    i = 0;
    while (i < 20) {
        arr[i] = i * 2;
        i = i + 1;
    }

    b1 = arr[0];   b2 = arr[1];   b3 = arr[2];   b4 = arr[3];   b5 = arr[4];
    b6 = arr[5];   b7 = arr[6];   b8 = arr[7];   b9 = arr[8];   b10 = arr[9];
    b11 = arr[10]; b12 = arr[11]; b13 = arr[12]; b14 = arr[13]; b15 = arr[14];

    sum = b1 + b2 + b3 + b4 + b5 + b6 + b7 + b8 +
          b9 + b10 + b11 + b12 + b13 + b14 + b15;

    return sum;
}
