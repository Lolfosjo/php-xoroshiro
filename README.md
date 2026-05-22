# Xoroshiro PHP Extension

Native PHP extension implementing a `xoroshiro128+` random number generator compatible with the RNG behavior used by Minecraft Java Edition 1.18+ world generation.

Designed for:
- PocketMine-MP 5
- PHP 8.x
- Seed tooling
- World generation research
- Deterministic simulation

---

## Features

- Native C extension (`xoroshiro.so`)
- Exact 64-bit operations
- Minecraft-compatible RNG behavior
- Splitter support
- Gaussian and triangular distributions
- Fast bounded integer generation
- IDE autocompletion stubs included

---

## Usage

```php
<?php

$rng = new Xoroshiro(12345);

echo $rng->nextI32() . PHP_EOL;
echo $rng->nextF64() . PHP_EOL;

$child = $rng->split();

echo $child->nextI64() . PHP_EOL;
```

---

## Example: bounded integers

```php
<?php

$rng = new Xoroshiro(12345);

$value = $rng->nextBoundedI32(100);

echo $value;
```

---

## Example: splitter API

```php
<?php

$rng = new Xoroshiro(12345);

$splitter = $rng->nextSplitter();

$overworld = $splitter->splitString("overworld");
$nether = $splitter->splitString("nether");
```

---

## 64-bit Integer Notes

PHP integers are signed.

Values above `PHP_INT_MAX` will appear negative even though the internal bit pattern is correct.

To view unsigned values:

```php
sprintf('%u', $value);
```

---

## IDE Stubs

This repository includes IDE stubs for:
- PhpStorm
- VSCode Intelephense
- static analysis tools

Do not include stub files in production.

---

## Compatibility

- PHP 8.0+
- 64-bit platforms only
- Linux
- PocketMine-MP 5

---

## Legal

This project is an independent implementation and is **not affiliated with or endorsed by** Mojang Studios or Microsoft.

Minecraft is a trademark of Microsoft.

This project does not include:
- Minecraft assets
- Decompiled Minecraft source code
- Mojang proprietary code

The implementation is based on publicly documented algorithms and observed compatibility behavior.

---

## License

MIT License.
