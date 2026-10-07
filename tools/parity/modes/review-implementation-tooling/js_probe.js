// Does a JavaScript client "print the same text" as the canonical writer?
const f32 = new Float32Array(1), u32 = new Uint32Array(f32.buffer);
function fromBits(b){ u32[0]=b>>>0; return f32[0]; }
function bitsOf(x){ f32[0]=x; return u32[0]>>>0; }
for (const t of ["0.55","375","0.35","0.45","1e-7","3.4028235e+38","0.577","0.479","7.038531e-26"]) {
  const v = Math.fround(JSON.parse(t));
  console.log(t.padEnd(16), "-> JS toString of the float:", String(v), " bits", bitsOf(v).toString(16));
}
// The two double-rounding values: which texts read back exactly through JSON.parse + Math.fround?
for (const b of [0x15AE43FD, 0x95AE43FD]) {
  const x = fromBits(b);
  for (let p = 7; p <= 9; ++p) {
    const t = x.toPrecision(p);         // correctly rounded p-digit text of the binary64 value of the float
    const back = bitsOf(Math.fround(JSON.parse(t)));
    console.log(b.toString(16), "p="+p, t, "->", back.toString(16), back===b ? "OK" : "WRONG");
  }
}
