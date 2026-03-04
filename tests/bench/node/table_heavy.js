let t = {};
for (let i = 0; i < 100000; i++) {
    t["key" + i] = i * i;
}
let sum = 0;
for (let i = 0; i < 100000; i++) {
    sum += t["key" + i];
}
console.log(sum);
