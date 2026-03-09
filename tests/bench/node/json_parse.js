const data = { name: "John Doe", age: 42, active: true, scores: [95, 87, 92, 78, 88] };
let sum = 0;
for (let i = 0; i < 100000; i++) {
    const j = JSON.stringify(data);
    const t = JSON.parse(j);
    sum += t.age;
}
console.log(sum);
