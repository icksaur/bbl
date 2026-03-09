function spectralNorm(n) {
    const A = (i, j) => 1 / ((i + j) * (i + j + 1) / 2 + i + 1);
    const u = new Float64Array(n).fill(1);
    const v = new Float64Array(n).fill(0);
    for (let k = 0; k < 10; k++) {
        for (let i = 0; i < n; i++) {
            let s = 0; for (let j = 0; j < n; j++) s += A(i, j) * u[j]; v[i] = s;
        }
        for (let i = 0; i < n; i++) {
            let s = 0; for (let j = 0; j < n; j++) s += A(i, j) * v[j]; u[i] = s;
        }
    }
    let vbv = 0, vv = 0;
    for (let i = 0; i < n; i++) { vbv += u[i] * v[i]; vv += v[i] * v[i]; }
    console.log(vbv);
}
spectralNorm(500);
