// Non-reproducible random source for destructive obfuscation. Seeded from the OS entropy pool on
// every construction; the seed is never stored anywhere (axiom A1: no side channel to invert).
#pragma once
#include <cstdint>
#include <random>

namespace pm {

class Rng {
 public:
  Rng() {
    std::random_device rd;
    uint64_t a = ((uint64_t)rd() << 32) ^ rd();
    uint64_t b = ((uint64_t)rd() << 32) ^ rd();
    s_[0] = a ? a : 0x9E3779B97F4A7C15ull;
    s_[1] = b ? b : 0xD1B54A32D192ED03ull;
    for (int i = 0; i < 8; ++i) next();
  }
  uint64_t next() {  // xoroshiro128+
    uint64_t s0 = s_[0], s1 = s_[1];
    uint64_t r = s0 + s1;
    s1 ^= s0;
    s_[0] = ((s0 << 55) | (s0 >> 9)) ^ s1 ^ (s1 << 14);
    s_[1] = (s1 << 36) | (s1 >> 28);
    return r;
  }
  double uniform() { return (next() >> 11) * (1.0 / 9007199254740992.0); }  // [0,1)
  int below(int n) { return n <= 1 ? 0 : (int)(uniform() * n); }
 private:
  uint64_t s_[2];
};

}  // namespace pm
