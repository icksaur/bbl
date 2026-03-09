function makeTree(depth) {
    if (depth <= 0) return { left: null, right: null, val: 1 };
    return { left: makeTree(depth - 1), right: makeTree(depth - 1), val: depth };
}
function checkTree(t) {
    if (t.left === null) return t.val;
    return t.val + checkTree(t.left) + checkTree(t.right);
}
const minDepth = 4, maxDepth = 14;
const stretch = makeTree(maxDepth + 1);
let sum = 0;
for (let d = minDepth; d <= maxDepth; d += 2) {
    let iterations = 1 << (maxDepth - d);
    let check = 0;
    for (let i = 0; i < iterations; i++) {
        check += checkTree(makeTree(d));
    }
    sum += check;
}
console.log(sum);
