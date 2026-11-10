import { Stats } from './lib/stats.js';

function main() {
  const s = new Stats([1, 2, 3]);
  console.log(s.sum(), s.sumOfSquares());
}

main();
