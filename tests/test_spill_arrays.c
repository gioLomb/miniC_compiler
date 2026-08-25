/* test_spill_arrays.c
 *
 * Obiettivo: array in loop (indice = induction variable candidato a SR)
 * seguito da un blocco con 15 letture arr[k] contemporaneamente vive
 * (> k=14 registri), forzando spill sui risultati di IR_LOAD_ARR.
 * Verifica anche che STORE_ARR/LOAD_ARR non vengano scambiati da LICM
 * (LOAD_ARR e' impuro in licm.c: nessuna alias analysis) e che il ciclo
 * di scrittura arr[i]=i*2 sfrutti la strength reduction (i*2 -> shift).
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
