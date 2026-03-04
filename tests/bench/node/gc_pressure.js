let i = 0;
while (i < 1000000) {
    let t = { x: i, y: i * 2, z: i + 1 };
    i++;
}
console.log(i);
