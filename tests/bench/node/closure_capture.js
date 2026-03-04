function makeAdder(x, y) { return function() { return x + y; }; }
let makers = [];
for (let i = 0; i < 10000; i++) makers.push(makeAdder(i, i * 2));
let sum = 0;
for (let i = 0; i < 10000; i++) sum += makers[i]();
console.log(sum);
