import { add, square } from './math.js';

export class Stats {
  constructor(values) {
    this.values = values;
  }

  sum() {
    return this.values.reduce((acc, v) => add(acc, v), 0);
  }

  sumOfSquares() {
    return this.values.map((v) => square(v)).reduce((a, b) => add(a, b), 0);
  }
}
