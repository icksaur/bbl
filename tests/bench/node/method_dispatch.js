class Counter { constructor() { this.n = 0; } inc() { this.n++; } get() { return this.n; } }
let c = new Counter();
for (let i = 0; i < 100000; i++) { c.inc(); c.inc(); c.inc(); c.inc(); c.get(); }
console.log(c.get());
