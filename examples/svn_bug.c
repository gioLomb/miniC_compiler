
int force_spill(int x) {
    int v1  = x + 1;
    int v2  = x + 2;
    int v3  = x + 3;
    int v4  = x + 4;
    int v5  = x + 5;
    int v6  = x + 6;
    int v7  = x + 7;
    int v8  = x + 8;
    int v9  = x + 9;
    int v10 = x + 10;
    int v11 = x + 11;
    int v12 = x + 12;
    int v13 = x + 13;
    int v14 = x + 14;
    int v15 = x + 15;
    int v16 = x + 16;
    int v17 = x + 17;
    int v18 = x + 18;
    int v19 = x + 19;
    int v20 = x + 20;

    // Tutte e 20 le variabili sono LIVE qui, forzando lo spill
    return v1 + v2 + v3 + v4 + v5 + v6 + v7 + v8 + v9 + v10 +
           v11 + v12 + v13 + v14 + v15 + v16 + v17 + v18 + v19 + v20;
}