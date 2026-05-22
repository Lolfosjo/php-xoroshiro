<?php
/**
 * Xoroshiro PHP Extension — IDE stubs
 *
 * This file is for IDE autocompletion only. Do not include it in production.
 * The actual implementation lives in the compiled C extension (xoroshiro.so).
 *
 * Compatible with PocketMine-MP 5 / PHP 8.x
 */

/**
 * Xoroshiro128+ random number generator.
 *
 * Port of Minecraft's Xoroshiro128+ implementation.
 * All 64-bit operations are exact on PHP 8 (64-bit platform).
 * Large unsigned values (>PHP_INT_MAX) are returned as negative ints —
 * use `sprintf('%u', $v)` or pack()/unpack() if you need the unsigned string.
 */
final class Xoroshiro
{
    /**
     * Create a generator by mixing the seed through Stafford-13.
     * Equivalent to Xoroshiro::from_seed() in Rust.
     */
    public function __construct(int $seed) {}

    /**
     * Static constructor — identical to new Xoroshiro($seed).
     */
    public static function fromSeed(int $seed): static {}

    /**
     * Create a generator from a seed without the Stafford-13 mixing step.
     * Equivalent to Xoroshiro::from_seed_unmixed() in Rust.
     */
    public static function fromSeedUnmixed(int $seed): static {}

    /**
     * Create a generator directly from two 64-bit state words.
     * Pass PHP int values (bit-identical to uint64 in C on 64-bit PHP).
     */
    public static function fromLoHi(int $lo, int $hi): static {}

    /**
     * Generate the next signed 32-bit integer.
     * Range: PHP_INT_MIN..PHP_INT_MAX (but only 32 bits of entropy).
     */
    public function nextI32(): int {}

    /**
     * Generate a random int in [0, $bound).
     *
     * @param int $bound Must be > 0.
     * @throws \InvalidArgumentException if $bound <= 0.
     */
    public function nextBoundedI32(int $bound): int {}

    /**
     * Generate a random int in [$min, $max] inclusive.
     */
    public function nextInbetweenI32(int $min, int $max): int {}

    /**
     * Generate the next 64-bit value (returned as a PHP signed int).
     * Bit pattern is identical to Rust's next_i64().
     */
    public function nextI64(): int {}

    /** Generate a random boolean. */
    public function nextBool(): bool {}

    /** Generate a random float in [0, 1) with 32-bit precision. */
    public function nextF32(): float {}

    /** Generate a random float in [0, 1) with 53-bit (double) precision. */
    public function nextF64(): float {}

    /** Generate a normally distributed random float (mean 0, stddev 1). */
    public function nextGaussian(): float {}

    /**
     * Generate a triangular-distributed random float.
     *
     * @param float $mode   The mode (peak) of the triangle distribution.
     * @param float $spread Half-width of the distribution.
     */
    public function nextTriangular(float $mode, float $spread): float {}

    /**
     * Create an independent child generator (fork).
     * Consumes two 64-bit values from the current generator's state.
     */
    public function split(): static {}

    /**
     * Obtain a splitter that can derive further independent generators.
     * Consumes two 64-bit values from the current generator's state.
     */
    public function nextSplitter(): XoroshiroSplitter {}

    /**
     * Return the raw internal state as ['lo' => int, 'hi' => int].
     * Useful for serialisation / persistence.
     */
    public function getState(): array {}
}

/**
 * Derives independent Xoroshiro generators from a fixed splitter state.
 *
 * Equivalent to XoroshiroSplitter in Rust.
 * Immutable — all split* methods return a new Xoroshiro without modifying
 * the splitter itself.
 */
final class XoroshiroSplitter
{
    /**
     * Derive a generator whose seed is the MD5 hash of $seed XOR'd with
     * the splitter state. Mirrors split_string() in Rust.
     */
    public function splitString(string $seed): Xoroshiro {}

    /**
     * Derive a generator by XOR-ing $seed with both state words.
     * Mirrors split_u64() in Rust.
     */
    public function splitU64(int $seed): Xoroshiro {}

    /**
     * Derive a generator seeded by the hash of a block position.
     * Mirrors split_pos() in Rust.
     */
    public function splitPos(int $x, int $y, int $z): Xoroshiro {}

    /**
     * Derive a generator from explicit lo/hi words XOR'd with the splitter state.
     * Mirrors from_lo_and_hi() in Rust.
     */
    public function fromLoHi(int $lo, int $hi): Xoroshiro {}
}

/**
 * Apply the Stafford-13 mixing function to a 64-bit integer.
 *
 * @param int $value Input (PHP int = 64-bit on 64-bit PHP).
 * @return int Mixed value (same bit-width).
 */
function xoroshiro_mix_stafford13(int $value): int {}

/**
 * Compute Minecraft's block-position hash for (x, y, z).
 *
 * @param int $x Block X coordinate (32-bit).
 * @param int $y Block Y coordinate (32-bit).
 * @param int $z Block Z coordinate (32-bit).
 * @return int 64-bit hash (returned as PHP signed int).
 */
function xoroshiro_hash_block_pos(int $x, int $y, int $z): int {}
