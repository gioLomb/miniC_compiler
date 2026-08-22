int arr[10];
int a = 9;
int main(){
    int result = test_regalloc_store_index(1);
    return 0;
}
int test_regalloc_store_index(int idx) {
    int x = 99;
    arr[idx] = 7;
    int result = x + arr[idx];
    return result + a;
}