const data = "12,34,56,78,90,11,22,33,44,55,66,77,88,99,10,20,30,40,50,60";
const len = data.length;
let sum = 0;
for (let i = 0; i < 50000; i++) {
    let pos = 0;
    while (pos < len) {
        let comma = data.indexOf(",", pos);
        let end = comma === -1 ? len : comma;
        sum += parseInt(data.substring(pos, end), 10);
        pos = end + 1;
    }
}
console.log(sum);
