int arr[10];
int a = 9;
int test_regalloc_store_index() {
    int x;
    int idx;
    int result;
    /* idx usato come indice STORE, x live dopo.
       Se regalloc non vede idx come "use" in STORE,
       può colorare idx e x con stesso fisico → x corrotto */
    x = 99;
    idx = 3;
    arr[idx] = 7;
    result = x + arr[idx];
    return result + a;  /* deve essere 99 + 7 = 106 */
}