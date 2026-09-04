int main() {
   int a = 5;
   int b;
   b = a * 3;
   int somma = foo(b,4,7);
   return somma * 2 +fee(2,3); 
}

int foo(int a,int b,int c,int d){
    return a*(b+c+d);
}

int fee(int a,int b){
    return a/b;
}
